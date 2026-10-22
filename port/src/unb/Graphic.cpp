// Graphic.cpp - CGraphic on SDL3 GPU
//
// Behaviour follows the original unb/Graphic.cpp of perfect.exe (addresses below); the
// implementation is new. Main differences from the Direct3D 8 version:
//
// - Draws are recorded into a per-flush batch (one vertex buffer upload, one render pass) and
//   submitted when the frame ends or when something forces it: a texture upload, a clear, a
//   render-target switch or a screenshot. Same order of operations, far fewer API calls.
// - Pipeline state objects are built on demand from the D3D-style render state and cached.
// - P8 textures are stored as R8 indices; palettes live in one RGBA atlas and are looked up in
//   the fragment shader.
// - The scene is always drawn into texture slot 0 (a render target the size of
//   scrwidth x scrheight) and scaled into the window at the end of the frame. With screffect on,
//   slots 0 and 1 swap every frame so the previous frame can be sampled as texture 1, exactly
//   like the original ApplyScreenEffect.
// - Textures live in two tables, as in the original: the direct table (CGraphic::LoadTexture,
//   m_TexInfo in the original) and the managed table filled by CTexture (nGroup 0x0F).

#include "Graphic.h"
#include <cstdlib>
#include <dlfcn.h>
#include "GraphicShaders.h"
#include "GraphicShadersSPV.h"     // the same shaders as SPIR-V (Vulkan), from shaders/scene.glsl
#include "Texture.h"            // ps2:: decoders, shared with CTexture
#include "TexturePack.h"        // texture dumping and HD replacements

#include <algorithm>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

// getenv once per call site (debug hooks run in hot paths)
#define CACHED_ENV(name) ([]() -> const char * { static const char *v_ = getenv(name); return v_; }())

using namespace d3d;

// ---------------------------------------------------------------------------------------------
// Internal state

namespace {

// Unified vertex: TLVERTEX_TEX layout, used for both textured and untextured draws.
struct Vertex { float x, y, z, rhw; D3DCOLOR color; float u, v; };
static_assert(sizeof(Vertex) == 28, "matches the original FVF 0x144 stride");

struct VSParams { float targetW, targetH; };

struct FSParams {           // must match FSParams in GraphicShaders.h
    uint32_t mode, colorOp, alphaOp, alphaTest, alphaFunc;
    float    alphaRef;
    uint32_t addrU, addrV, linear, texW, texH, palX, palY;
};

struct PresentParams { float invGamma, uvOffsetX, uvOffsetY, uvScaleX, uvScaleY, uvMinX, uvMinY, uvMaxX, uvMaxY; };   // must match GraphicShaders.h, scene.glsl

// Render state in Direct3D 8 terms, with D3D defaults.
struct State {
    uint32_t zEnable = 1, zWrite = 1, zFunc = CMP_LESSEQUAL;
    uint32_t blendEnable = 0, srcBlend = BLEND_ONE, dstBlend = BLEND_ZERO, blendOp = BLENDOP_ADD;
    uint32_t cull = CULL_CCW, shade = SHADE_GOURAUD;
    uint32_t alphaTest = 0, alphaFunc = CMP_ALWAYS, alphaRef = 0;
    uint32_t colorOp = TOP_MODULATE, alphaOp = TOP_SELECTARG1;
    uint32_t addrU = TADDRESS_WRAP, addrV = TADDRESS_WRAP;
    uint32_t magFilter = TEXF_POINT, minFilter = TEXF_POINT;
    uint32_t fogEnable = 0, fogColor = 0;
    uint32_t colorWrite = 15;
};

// Must match MeshVS / MeshFS in GraphicShaders.h.
struct MeshVSParams {
    float fog[4], lightColor[3][4], m[4][4], lightLocal[3][4], base[4], stz[4];
    uint32_t kind, pad[3];
};
struct MeshFSParams {
    FSParams p;
    float fogR, fogG, fogB;
    uint32_t fogOn;
};

struct DrawCmd {
    uint64_t        pipeKey;
    uint32_t        firstVertex, vertexCount;
    SDL_GPUTexture *pTex;
    SDL_GPUSampler *pSampler;
    FSParams        fs;
    SDL_Rect        scissor;
    CGraphicMesh   *pMesh;          // set for 3D model draws
    uint32_t        meshParams;     // index into Impl::meshParams
};

constexpr uint64_t kKeyMesh = 1ull << 24;

constexpr uint32_t kPaletteAtlasW = 4096;   // 16 palettes of 256 entries per row
constexpr uint32_t kPaletteAtlasH = CGraphic::kNoPalette / 16;

} // namespace

static uint32_t g_frameSerial;      // frames begun (debugging)
static std::string g_prefDir;       // LoadConfig: settings folder (with a trailing /)
static CGraphic::DebugStats g_stats; // diagnostics (RECOMP_TRACE_TICKS)

struct CGraphicTexture {
    SDL_GPUTexture      *pTex = nullptr;
    uint32_t             nWidth = 0, nHeight = 0;      // GPU texture size
    uint32_t             nUVWidth = 0, nUVHeight = 0;  // size NormalizeUV divides by
    TexFormat            nFormat = TEXFMT_BGRA8;
    bool                 bRenderTarget = false;
    bool                 bDirty = false;
    std::vector<uint8_t> staging;                      // CPU copy filled through LockTexture
    uint32_t             gpuW = 0, gpuH = 0, gpuLevels = 1;  // the GPU texture (a replacement's size)
    bool                 gpuBC7 = false;               // ... holding a BC7 replacement
    std::shared_ptr<const texpack::Image> repl;        // texture pack replacement, until uploaded
    uint64_t             packHash = 0;                 // texels' hash, while waiting for the replacement
    bool                 packWaiting = false;
};

// A model's vertices: a range of the shared mesh arena (one vertex buffer for all models, so a
// frame binds a single buffer; SDL's per-buffer tracking was quadratic in the number of buffers).
struct CGraphicMesh {
    uint32_t                offset = 0;     // first vertex in the arena
    std::vector<MeshVertex> staging;        // kept for re-uploads when the arena grows
    bool                    bDirty = true;
};

struct CGraphic::Impl {
    SDL_Window *pWindow = nullptr;
    SDL_GPUShader *pVS[2] = {}, *pFS[2] = {};       // [0] Gouraud, [1] flat
    SDL_GPUShader *pMeshVS[2] = {}, *pMeshFS[2] = {};
    SDL_GPUShader *pPresentVS = nullptr, *pPresentFS = nullptr;
    SDL_GPUGraphicsPipeline *pPresentPipe = nullptr;
    std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline *> pipes;
    std::unordered_map<uint32_t, SDL_GPUSampler *> samplers;
    SDL_GPUSampler *pPresentSampler = nullptr;
    SDL_GPUSampler *pOverlaySampler = nullptr;      // nearest (CGraphic::SetOverlay)
    SDL_GPUTexture *pOverlayTex = nullptr;
    CGraphicTexture *pFeedback = nullptr;           // copy of the target for draws that sample it (FeedbackCopy)
    int overlayTexW = 0, overlayTexH = 0;
    uint32_t overlayVersion = 0;

    SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture *pDepth = nullptr;
    SDL_GPUTexture *pWhite = nullptr;               // bound when drawing untextured
    SDL_GPUTexture *pPaletteAtlas = nullptr;
    std::vector<uint8_t> paletteCPU;                // kPaletteAtlasW * kPaletteAtlasH * 4
    std::vector<uint32_t> dirtyPalettes;
    std::vector<bool> paletteDirty;
    std::vector<uint32_t> freePalettes;

    std::vector<CGraphicTexture *> tex;             // direct table; 0 and 1 are the scene targets
    std::vector<uint32_t> texPalette;               // palette per direct slot (nD3DP), lazily allocated
    std::vector<CGraphicTexture *> managedTex;      // CTexture table (nGroup 0x0F)
    std::vector<uint32_t> managedPalette;
    std::vector<CGraphicTexture *> dirtyTex;
    std::vector<CGraphicTexture *> packWaiting;     // drawn as the original until the replacement is decoded
    std::vector<CGraphicMesh *> meshes, dirtyMeshes;
    SDL_GPUBuffer *pMeshArena = nullptr;
    uint32_t arenaCap = 0;                              // vertices
    std::map<uint32_t, uint32_t> arenaFree;             // offset -> count, coalesced
    std::vector<std::pair<uint32_t, uint32_t>> arenaPendingFree;   // freed during this frame
    std::vector<std::pair<MeshVSParams, MeshFSParams>> meshParams;
    SDL_GPUBuffer *pMeshVSBuf = nullptr, *pMeshFSBuf = nullptr;   // meshParams on the GPU
    uint32_t meshParamCapacity = 0;
    TEXSIZE texSize = {16256, 256, 256};            // default override table from the binary
    uint32_t maxTexW = 1024, maxTexH = 1024;

    // Batch
    std::vector<Vertex> verts;
    std::vector<DrawCmd> draws;
    SDL_GPUBuffer *pVB = nullptr;
    uint32_t vbCapacity = 0;
    SDL_GPUTransferBuffer *pUpload = nullptr;
    uint32_t uploadCapacity = 0;
    bool pendingClearColor = false, pendingClearDepth = false;
    SDL_FColor clearColor = {0, 0, 0, 1};
    float clearDepth = 1.0f;

    // Current state
    State st;
    bool textured = false;                          // m_nTexID < 0x4000
    CGraphicTexture *pCurTex = nullptr;             // m_pTexInf
    uint32_t curPalette = CGraphic::kNoPalette;     // atlas palette for the bound texture
    int target = 0;                                 // render target slot
    TLVERTEX_TEX texVertex[4] = {};                 // m_TexVertex
    TLVERTEX vertex2D[4] = {};                      // m_Vertex2D
    SDL_Rect viewport[2] = {};                      // m_Viewport: full screen, inset
    int nViewport = 0;
    SDL_Rect border[4] = {};                        // m_rcBorder
    float gamma = 1.0f;
    // deferred presentation (see SetDeferredPresent)
    bool deferPresent = false;
    bool framePending = false;
    SDL_GPUTexture *pPresentTex = nullptr;
    std::recursive_mutex gpuLock;           // game thread records/submits, window thread presents
};

static CGraphic::Impl *g = nullptr;

GRAPHIC_CONFIG     CGraphic::m_Config;
SDL_GPUDevice     *CGraphic::m_pDevice = nullptr;
TexFormat          CGraphic::m_nTexFormat = TEXFMT_P8;
PrimitiveType      CGraphic::m_nPrimType = TRIANGLESTRIP;
char               CGraphic::m_szLastError[256] = "";

// ---------------------------------------------------------------------------------------------
// Helpers

static bool Fail(char *buf, const char *what)
{
    SDL_snprintf(buf, 256, "%s: %s", what, SDL_GetError());
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "CGraphic: %s", buf);
    return false;
}
#define FAIL(what) Fail(CGraphic::m_szLastError, what)

static SDL_GPUBlendFactor ToBlend(uint32_t b)
{
    switch (b) {
    case BLEND_ZERO:         return SDL_GPU_BLENDFACTOR_ZERO;
    case BLEND_SRCCOLOR:     return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case BLEND_INVSRCCOLOR:  return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case BLEND_SRCALPHA:     return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case BLEND_INVSRCALPHA:  return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case BLEND_DESTALPHA:    return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case BLEND_INVDESTALPHA: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case BLEND_DESTCOLOR:    return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case BLEND_INVDESTCOLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case BLEND_SRCALPHASAT:  return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    default:                 return SDL_GPU_BLENDFACTOR_ONE;
    }
}

static SDL_GPUBlendOp ToBlendOp(uint32_t op)
{
    switch (op) {
    case BLENDOP_SUBTRACT:    return SDL_GPU_BLENDOP_SUBTRACT;
    case BLENDOP_REVSUBTRACT: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case BLENDOP_MIN:         return SDL_GPU_BLENDOP_MIN;
    case BLENDOP_MAX:         return SDL_GPU_BLENDOP_MAX;
    default:                  return SDL_GPU_BLENDOP_ADD;
    }
}

static SDL_GPUCompareOp ToCompare(uint32_t c)
{
    switch (c) {
    case CMP_NEVER:        return SDL_GPU_COMPAREOP_NEVER;
    case CMP_LESS:         return SDL_GPU_COMPAREOP_LESS;
    case CMP_EQUAL:        return SDL_GPU_COMPAREOP_EQUAL;
    case CMP_LESSEQUAL:    return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case CMP_GREATER:      return SDL_GPU_COMPAREOP_GREATER;
    case CMP_NOTEQUAL:     return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case CMP_GREATEREQUAL: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default:               return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

static SDL_GPUPrimitiveType ToTopology(uint32_t t)
{
    switch (t) {
    case POINTLIST: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
    case LINELIST:  return SDL_GPU_PRIMITIVETYPE_LINELIST;
    case LINESTRIP: return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
    case TRIANGLELIST:
    case TRIANGLEFAN:  return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;   // fans are expanded on record
    default:        return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    }
}

// Everything that ends up in an SDL_GPUGraphicsPipeline, packed into a cache key.
static uint64_t PipeKey(uint32_t topology, const State &s)
{
    uint64_t k = topology & 7;
    k |= (uint64_t)(s.blendEnable & 1) << 3;
    k |= (uint64_t)(s.srcBlend & 15) << 4;
    k |= (uint64_t)(s.dstBlend & 15) << 8;
    k |= (uint64_t)(s.blendOp & 7) << 12;
    k |= (uint64_t)(s.zEnable ? 1 : 0) << 15;
    k |= (uint64_t)(s.zWrite ? 1 : 0) << 16;
    k |= (uint64_t)(s.zFunc & 15) << 17;
    k |= (uint64_t)(s.cull & 3) << 21;
    k |= (uint64_t)(s.shade == SHADE_FLAT ? 1 : 0) << 23;
    k |= (uint64_t)(~s.colorWrite & 15) << 25;      // bit 24 is kKeyMesh
    return k;
}

// A shader from MSL source (Metal; `prefix` adds #defines) or the matching SPIR-V build (Vulkan;
// variants are separate builds, see GraphicShadersSPV.h), whichever the device takes.
template <size_t N>
static SDL_GPUShader *MakeShader(const char *src, const char *entry, const uint32_t (&spv)[N], SDL_GPUShaderStage stage,
                                 uint32_t samplers, const char *prefix = "", uint32_t uniforms = 1,
                                 uint32_t storage = 0)
{
    std::string code = std::string(prefix) + src;
    SDL_GPUShaderCreateInfo ci = {};
    if (SDL_GetGPUShaderFormats(CGraphic::GetDevice()) & SDL_GPU_SHADERFORMAT_MSL) {
        ci.code_size = code.size();
        ci.code = reinterpret_cast<const Uint8 *>(code.c_str());
        ci.entrypoint = entry;
        ci.format = SDL_GPU_SHADERFORMAT_MSL;
    } else {
        ci.code_size = sizeof(spv);
        ci.code = reinterpret_cast<const Uint8 *>(spv);
        ci.entrypoint = "main";
        ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    }
    ci.stage = stage;
    ci.num_samplers = samplers;
    ci.num_uniform_buffers = uniforms;
    ci.num_storage_buffers = storage;
    return SDL_CreateGPUShader(CGraphic::GetDevice(), &ci);
}

static SDL_GPUGraphicsPipeline *GetPipeline(uint64_t key)
{
    auto it = g->pipes.find(key);
    if (it != g->pipes.end()) return it->second;
    bool mesh = (key & kKeyMesh) != 0;

    State s;
    uint32_t topology = key & 7;
    s.blendEnable = (key >> 3) & 1;
    s.srcBlend = (key >> 4) & 15;
    s.dstBlend = (key >> 8) & 15;
    s.blendOp = (key >> 12) & 7;
    s.zEnable = (key >> 15) & 1;
    s.zWrite = (key >> 16) & 1;
    s.zFunc = (key >> 17) & 15;
    s.cull = (key >> 21) & 3;
    s.colorWrite = ~(uint32_t)(key >> 25) & 15;
    bool flat = (key >> 23) & 1;

    SDL_GPUVertexBufferDescription vb = {};
    vb.slot = 0;
    vb.pitch = mesh ? sizeof(MeshVertex) : sizeof(Vertex);
    vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute attrs2D[3] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Vertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(Vertex, color)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, u)},
    };
    SDL_GPUVertexAttribute attrsMesh[3] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(MeshVertex, x)},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(MeshVertex, nx)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(MeshVertex, u)},
    };
    SDL_GPUVertexAttribute *attrs = mesh ? attrsMesh : attrs2D;

    SDL_GPUColorTargetDescription ct = {};
    ct.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    if (s.blendEnable) {
        // D3D8 has no separate alpha blend, so the same factors apply to alpha.
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = ct.blend_state.src_alpha_blendfactor = ToBlend(s.srcBlend);
        ct.blend_state.dst_color_blendfactor = ct.blend_state.dst_alpha_blendfactor = ToBlend(s.dstBlend);
        ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = ToBlendOp(s.blendOp);
    }
    if (s.colorWrite != 15) {
        // D3DCOLORWRITEENABLE bits match SDL_GPUColorComponentFlags (R, G, B, A = 1, 2, 4, 8)
        ct.blend_state.enable_color_write_mask = true;
        ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)s.colorWrite;
    }

    SDL_GPUGraphicsPipelineCreateInfo pi = {};
    pi.vertex_shader = mesh ? g->pMeshVS[flat] : g->pVS[flat];
    pi.fragment_shader = mesh ? g->pMeshFS[flat] : g->pFS[flat];
    pi.vertex_input_state.vertex_buffer_descriptions = &vb;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attrs;
    pi.vertex_input_state.num_vertex_attributes = 3;
    pi.primitive_type = ToTopology(topology);
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    // D3D treats clockwise triangles (as seen on screen) as front faces. SDL GPU evaluates
    // winding in the same on-screen orientation here, so the mapping is direct.
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    pi.rasterizer_state.cull_mode = s.cull == CULL_CCW ? SDL_GPU_CULLMODE_BACK
                                  : s.cull == CULL_CW  ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_NONE;
    // 3D: clamp depth instead of clipping at the far plane. The sky sits right at depth 1.0, and
    // clipping removed its pixels along GPU tile rows (magenta lines); the GS never clipped on Z.
    pi.rasterizer_state.enable_depth_clip = !mesh;
    pi.depth_stencil_state.enable_depth_test = s.zEnable;
    pi.depth_stencil_state.enable_depth_write = s.zEnable && s.zWrite;
    pi.depth_stencil_state.compare_op = ToCompare(s.zFunc);
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = g->depthFormat;
    pi.target_info.has_depth_stencil_target = true;

    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(CGraphic::GetDevice(), &pi);
    if (!p) FAIL("SDL_CreateGPUGraphicsPipeline");
    g->pipes[key] = p;
    return p;
}

static SDL_GPUSampler *GetSampler(uint32_t addrU, uint32_t addrV, bool linear)
{
    uint32_t key = (addrU & 3) | (addrV & 3) << 2 | (linear ? 1u : 0u) << 4;
    auto it = g->samplers.find(key);
    if (it != g->samplers.end()) return it->second;
    auto mode = [](uint32_t a) {
        return a == TADDRESS_CLAMP  ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE
             : a == TADDRESS_MIRROR ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                                    : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    };
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    // only texture pack replacements have mip levels; the game's own textures have one
    si.mipmap_mode = linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.max_lod = 1000.0f;
    si.address_mode_u = mode(addrU);
    si.address_mode_v = mode(addrV);
    si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler *s = SDL_CreateGPUSampler(CGraphic::GetDevice(), &si);
    g->samplers[key] = s;
    return s;
}

static SDL_GPUTexture *MakeGPUTexture(SDL_GPUTextureFormat fmt, uint32_t w, uint32_t h, SDL_GPUTextureUsageFlags usage,
                                      uint32_t levels = 1)
{
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = fmt;
    ti.usage = usage;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = levels;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(CGraphic::GetDevice(), &ti);
}

static uint32_t BytesPerTexel(TexFormat f) { return f == TEXFMT_P8 ? 1 : 4; }

static void MarkDirty(CGraphicTexture *t)
{
    if (!t->bDirty) {
        t->bDirty = true;
        g->dirtyTex.push_back(t);
    }
}

// Make sure the persistent upload buffer can hold `size` bytes.
static bool ReserveUpload(uint32_t size)
{
    if (size <= g->uploadCapacity) return true;
    if (g->pUpload) SDL_ReleaseGPUTransferBuffer(CGraphic::GetDevice(), g->pUpload);
    uint32_t cap = SDL_max(size, g->uploadCapacity * 2);
    SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, cap, 0};
    g->pUpload = SDL_CreateGPUTransferBuffer(CGraphic::GetDevice(), &ti);
    g->uploadCapacity = g->pUpload ? cap : 0;
    return g->pUpload != nullptr;
}

// Texture packs: the replacement for a dirty texture's new texels (or none), with the GPU texture
// resized to it. PNG replacements larger than the original get mip levels down to the original
// size, generated on the GPU; BC7 ones bring their own. Draws recorded with the old GPU texture
// are pointed at the new one.
static void ResolveReplacement(CGraphicTexture *t)
{
    if (t->nFormat != TEXFMT_BGRA8 || t->bRenderTarget) return;
    uint64_t hash = texpack::Hash(t->staging.data(), t->nWidth, t->nHeight);
    texpack::Dump(hash, t->staging.data(), t->nWidth, t->nHeight);
    std::shared_ptr<const texpack::Image> r = texpack::Find(hash);
    // not decoded yet: the original texels for now, BeginFrame uploads again when it is ready
    t->packHash = hash;
    bool waiting = !r && texpack::Has(hash);
    if (waiting && !t->packWaiting) g->packWaiting.push_back(t);
    t->packWaiting = waiting;
    // The mip chain ends at the original size: below it, averaged alpha fails the alpha tests
    // (fences, wires and lattices thinned out or vanished), and the original drew level 0 there.
    uint32_t w = r ? r->w : t->nWidth, h = r ? r->h : t->nHeight, levels = 1;
    bool bc7 = r && r->bc7;
    if (bc7) {
        levels = r->levels;
    } else {
        uint32_t scale = SDL_max(1u, SDL_min(w / t->nWidth, h / t->nHeight));
        while ((2u << (levels - 1)) <= scale) levels++;
    }
    if (w != t->gpuW || h != t->gpuH || levels != t->gpuLevels || bc7 != t->gpuBC7) {
        SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 && !bc7 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        SDL_GPUTexture *nt = MakeGPUTexture(bc7 ? SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                                            w, h, usage, levels);
        if (!nt) {                                      // keep the texture as it is
            SDL_Log("texture pack: cannot create a %ux%u texture: %s", w, h, SDL_GetError());
            if (r && t->gpuW == t->nWidth && t->gpuH == t->nHeight) t->repl = nullptr;
            return;
        }
        for (DrawCmd &d : g->draws) if (d.pTex == t->pTex) d.pTex = nt;
        SDL_ReleaseGPUTexture(CGraphic::GetDevice(), t->pTex);
        t->pTex = nt;
        t->gpuW = w;
        t->gpuH = h;
        t->gpuLevels = levels;
        t->gpuBC7 = bc7;
    }
    t->repl = r;
}

// A replacement an exact power-of-two multiple of the original gets the original texels as its
// smallest mip level (after the generated ones): from a distance it looks exactly like the game's
// texture, whose grain an upscaler may have smoothed away.
static bool OriginalAsLastMip(const CGraphicTexture *t)
{
    uint32_t s = t->gpuLevels - 1;
    return t->repl && !t->repl->bc7 && s && t->gpuW == t->nWidth << s && t->gpuH == t->nHeight << s;
}

static uint32_t UploadBytes(const CGraphicTexture *t)
{
    return t->repl ? (uint32_t)(t->repl->data.size() + (OriginalAsLastMip(t) ? t->staging.size() : 0))
                   : (uint32_t)t->staging.size();
}

// Submit everything recorded so far: texture/palette uploads, then the batched draws.
static void ArenaRelease(uint32_t offset, uint32_t count);

static bool Flush()
{
    std::lock_guard<std::recursive_mutex> lock(g->gpuLock);
    SDL_GPUDevice *dev = CGraphic::GetDevice();

    uint32_t vbytes = (uint32_t)(g->verts.size() * sizeof(Vertex));
    uint32_t uploadSize = vbytes;
    if (texpack::Active()) for (CGraphicTexture *t : g->dirtyTex) ResolveReplacement(t);
    for (CGraphicTexture *t : g->dirtyTex) uploadSize += UploadBytes(t);
    for (CGraphicMesh *m : g->dirtyMeshes) uploadSize += (uint32_t)(m->staging.size() * sizeof(MeshVertex));
    uploadSize += (uint32_t)g->dirtyPalettes.size() * 256 * 4;
    uint32_t nParams = (uint32_t)g->meshParams.size();
    uploadSize += nParams * (uint32_t)(sizeof(MeshVSParams) + sizeof(MeshFSParams));
    if (nParams > g->meshParamCapacity) {
        if (g->pMeshVSBuf) SDL_ReleaseGPUBuffer(dev, g->pMeshVSBuf);
        if (g->pMeshFSBuf) SDL_ReleaseGPUBuffer(dev, g->pMeshFSBuf);
        g->meshParamCapacity = SDL_max(nParams, g->meshParamCapacity * 2);
        SDL_GPUBufferCreateInfo vi = {SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, g->meshParamCapacity * (uint32_t)sizeof(MeshVSParams), 0};
        SDL_GPUBufferCreateInfo fi = {SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, g->meshParamCapacity * (uint32_t)sizeof(MeshFSParams), 0};
        g->pMeshVSBuf = SDL_CreateGPUBuffer(dev, &vi);
        g->pMeshFSBuf = SDL_CreateGPUBuffer(dev, &fi);
        if (!g->pMeshVSBuf || !g->pMeshFSBuf) return FAIL("SDL_CreateGPUBuffer (mesh parameters)");
    }

    bool haveWork = !g->draws.empty() || g->pendingClearColor || g->pendingClearDepth;
    if (!haveWork && uploadSize == 0) return true;
    g_stats.flushes++;
    g_stats.drawCmds += (uint32_t)g->draws.size();
    // debugging: RECOMP_TRACE_FLUSH=<frame serial> prints who flushed during that frame
    if (const char *t = CACHED_ENV("RECOMP_TRACE_FLUSH"); t && g_frameSerial == strtoul(t, nullptr, 10)) {
        Dl_info info = {};
        void *caller = __builtin_return_address(0);
        dladdr(caller, &info);
        fprintf(stderr, "flush from %s (draws %zu, upload %u bytes)\n", info.dli_sname ? info.dli_sname : "?",
                g->draws.size(), uploadSize);
    }
    g_stats.uploadBytes += uploadSize;
    g_stats.vertexBytes += vbytes;
    for (CGraphicTexture *t : g->dirtyTex) { g_stats.texUploads++; g_stats.texUploadBytes += UploadBytes(t); }
    for (CGraphicMesh *m : g->dirtyMeshes) g_stats.meshUploadBytes += (uint32_t)(m->staging.size() * sizeof(MeshVertex));

    if (vbytes > g->vbCapacity) {
        if (g->pVB) SDL_ReleaseGPUBuffer(dev, g->pVB);
        g->vbCapacity = SDL_max(vbytes, g->vbCapacity * 2);
        SDL_GPUBufferCreateInfo bi = {SDL_GPU_BUFFERUSAGE_VERTEX, g->vbCapacity, 0};
        g->pVB = SDL_CreateGPUBuffer(dev, &bi);
        if (!g->pVB) return FAIL("SDL_CreateGPUBuffer");
    }

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return FAIL("SDL_AcquireGPUCommandBuffer");

    if (uploadSize) {
        if (!ReserveUpload(uploadSize)) { SDL_CancelGPUCommandBuffer(cmd); return FAIL("SDL_CreateGPUTransferBuffer"); }
        uint8_t *map = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, g->pUpload, true));
        uint32_t off = 0;
        std::vector<SDL_GPUTexture *> mipmapped;        // replacements: mip chain after the uploads
        std::vector<std::pair<CGraphicTexture *, uint32_t>> lastMips;  // and the original texels as the last
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);

        if (vbytes) {
            memcpy(map + off, g->verts.data(), vbytes);
            SDL_GPUTransferBufferLocation src = {g->pUpload, off};
            SDL_GPUBufferRegion dst = {g->pVB, 0, vbytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            off += vbytes;
        }
        for (CGraphicTexture *t : g->dirtyTex) {
            t->bDirty = false;
            if (t->repl && t->repl->bc7) {              // all mip levels, as compressed in the file
                const texpack::Image &r = *t->repl;
                memcpy(map + off, r.data.data(), r.data.size());
                for (uint32_t l = 0, lo = off; l < r.levels; lo += texpack::BC7LevelSize(r.w, r.h, l), l++) {
                    SDL_GPUTextureTransferInfo src = {g->pUpload, lo, 0, 0};
                    SDL_GPUTextureRegion dst = {};
                    dst.texture = t->pTex; dst.mip_level = l; dst.d = 1;
                    dst.w = SDL_max(r.w >> l, 1u); dst.h = SDL_max(r.h >> l, 1u);
                    SDL_UploadToGPUTexture(copy, &src, &dst, false);
                }
                off += (uint32_t)r.data.size();
                t->repl.reset();
                continue;
            }
            const uint8_t *data = t->repl ? t->repl->data.data() : t->staging.data();
            uint32_t bytes = t->repl ? (uint32_t)t->repl->data.size() : (uint32_t)t->staging.size();
            memcpy(map + off, data, bytes);
            SDL_GPUTextureTransferInfo src = {g->pUpload, off, t->gpuW, t->gpuH};
            SDL_GPUTextureRegion dst = {};
            dst.texture = t->pTex; dst.w = t->gpuW; dst.h = t->gpuH; dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            off += bytes;
            if (t->gpuLevels > 1) mipmapped.push_back(t->pTex);
            if (OriginalAsLastMip(t)) {
                memcpy(map + off, t->staging.data(), t->staging.size());
                lastMips.push_back({t, off});
                off += (uint32_t)t->staging.size();
            }
            t->repl.reset();                            // the GPU has it; the pack's cache may keep it
        }
        g->dirtyTex.clear();
        for (CGraphicMesh *m : g->dirtyMeshes) {
            uint32_t bytes = (uint32_t)(m->staging.size() * sizeof(MeshVertex));
            memcpy(map + off, m->staging.data(), bytes);
            SDL_GPUTransferBufferLocation src = {g->pUpload, off};
            SDL_GPUBufferRegion dst = {g->pMeshArena, (uint32_t)(m->offset * sizeof(MeshVertex)), bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
            off += bytes;
            m->bDirty = false;
        }
        g->dirtyMeshes.clear();
        for (uint32_t id : g->dirtyPalettes) {
            uint32_t px = (id % 16) * 256, py = id / 16;
            memcpy(map + off, &g->paletteCPU[((size_t)py * kPaletteAtlasW + px) * 4], 256 * 4);
            SDL_GPUTextureTransferInfo src = {g->pUpload, off, 256, 1};
            SDL_GPUTextureRegion dst = {};
            dst.texture = g->pPaletteAtlas; dst.x = px; dst.y = py; dst.w = 256; dst.h = 1; dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            off += 256 * 4;
            g->paletteDirty[id] = false;
        }
        g->dirtyPalettes.clear();
        if (nParams) {
            // split the pairs into the two storage buffers
            uint32_t vsBytes = nParams * (uint32_t)sizeof(MeshVSParams), fsBytes = nParams * (uint32_t)sizeof(MeshFSParams);
            for (uint32_t i = 0; i < nParams; i++) {
                memcpy(map + off + i * sizeof(MeshVSParams), &g->meshParams[i].first, sizeof(MeshVSParams));
                memcpy(map + off + vsBytes + i * sizeof(MeshFSParams), &g->meshParams[i].second, sizeof(MeshFSParams));
            }
            SDL_GPUTransferBufferLocation src = {g->pUpload, off};
            SDL_GPUBufferRegion dst = {g->pMeshVSBuf, 0, vsBytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            src.offset = off + vsBytes;
            dst = {g->pMeshFSBuf, 0, fsBytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            off += vsBytes + fsBytes;
        }
        SDL_EndGPUCopyPass(copy);
        SDL_UnmapGPUTransferBuffer(dev, g->pUpload);
        for (SDL_GPUTexture *t : mipmapped) SDL_GenerateMipmapsForGPUTexture(cmd, t);
        if (!lastMips.empty()) {
            SDL_GPUCopyPass *mipCopy = SDL_BeginGPUCopyPass(cmd);
            for (auto &lm : lastMips) {
                CGraphicTexture *t = lm.first;
                SDL_GPUTextureTransferInfo src = {g->pUpload, lm.second, t->nWidth, t->nHeight};
                SDL_GPUTextureRegion dst = {};
                dst.texture = t->pTex; dst.mip_level = t->gpuLevels - 1; dst.w = t->nWidth; dst.h = t->nHeight; dst.d = 1;
                SDL_UploadToGPUTexture(mipCopy, &src, &dst, false);
            }
            SDL_EndGPUCopyPass(mipCopy);
        }
    }

    if (haveWork) {
        CGraphicTexture &rt = *g->tex[g->target];

        // 2D and 3D draws go to separate render passes: on Metal's tile-based renderer, mixing 2D
        // (border clears at z = 1.0) with 3D (sky meshes also at z = 1.0) in one pass caused depth
        // precision artifacts at tile boundaries (magenta lines in the sky). The passes follow the
        // draw order (one per run of 2D or 3D draws): the game clears the 3D background with a 2D
        // rectangle drawn first (depth test "always"), which an all-3D-first order put on top of
        // the scene (only the 2D was left: Vulkan, in the driving tutorial). The first 3D pass
        // starts with a cleared depth buffer, as the 3D-first order did.
        SDL_GPUSampler *palSampler = GetSampler(TADDRESS_CLAMP, TADDRESS_CLAMP, false);
        bool depthCleared = false;
        size_t i = 0, n = g->draws.size();
        do {
            bool mesh = i < n && g->draws[i].pMesh != nullptr;
            size_t e = i;
            while (e < n && (g->draws[e].pMesh != nullptr) == mesh) e++;

            SDL_GPUColorTargetInfo color = {};
            color.texture = rt.pTex;
            color.load_op = g->pendingClearColor ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            color.store_op = SDL_GPU_STOREOP_STORE;
            color.clear_color = g->clearColor;
            SDL_GPUDepthStencilTargetInfo depth = {};
            depth.texture = g->pDepth;
            bool clearDepth = g->pendingClearDepth || (mesh && !depthCleared);
            depth.load_op = clearDepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depth.clear_depth = mesh ? 1.0f : g->clearDepth;
            if (mesh) depthCleared = true;
            g->pendingClearColor = g->pendingClearDepth = false;
            SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);

            SDL_GPUGraphicsPipeline *bound = nullptr;
            bool haveScissor = false, haveTex = false, haveFs = false, haveMeshVp = false;
            SDL_Rect lastScissor = {}, lastMeshVp = {};
            SDL_GPUTexture *lastTex = nullptr;
            SDL_GPUSampler *lastSampler = nullptr;
            FSParams lastFs = {};
            if (mesh) {
                SDL_BindGPUVertexStorageBuffers(pass, 0, &g->pMeshVSBuf, 1);
                SDL_BindGPUFragmentStorageBuffers(pass, 0, &g->pMeshFSBuf, 1);
                SDL_GPUBufferBinding mb = {g->pMeshArena, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &mb, 1);
            } else if (e > i) {
                VSParams vs = {(float)rt.nWidth, (float)rt.nHeight};
                SDL_PushGPUVertexUniformData(cmd, 0, &vs, sizeof(vs));
                if (!g->verts.empty()) {
                    SDL_GPUBufferBinding vbb = {g->pVB, 0};
                    SDL_BindGPUVertexBuffers(pass, 0, &vbb, 1);
                }
                SDL_GPUViewport vp = {0, 0, (float)rt.nWidth, (float)rt.nHeight, 0, 1};
                SDL_SetGPUViewport(pass, &vp);
            }
            for (size_t k = i; k < e; k++) {
                const DrawCmd &d = g->draws[k];
                SDL_GPUGraphicsPipeline *p = GetPipeline(d.pipeKey);
                if (!p) continue;
                if (p != bound) { SDL_BindGPUGraphicsPipeline(pass, p); bound = p; g_stats.pipelineBinds++; }
                if (!haveScissor || memcmp(&d.scissor, &lastScissor, sizeof(SDL_Rect))) {
                    SDL_SetGPUScissor(pass, &d.scissor);
                    lastScissor = d.scissor;
                    haveScissor = true;
                }
                if (!haveTex || d.pTex != lastTex || d.pSampler != lastSampler) {
                    SDL_GPUTextureSamplerBinding tb[2] = {{d.pTex, d.pSampler}, {g->pPaletteAtlas, palSampler}};
                    SDL_BindGPUFragmentSamplers(pass, 0, tb, 2);
                    lastTex = d.pTex;
                    lastSampler = d.pSampler;
                    haveTex = true;
                }
                if (mesh) {
                    if (!haveMeshVp || memcmp(&d.scissor, &lastMeshVp, sizeof(SDL_Rect))) {
                        SDL_GPUViewport mvp = {(float)d.scissor.x, (float)d.scissor.y,
                                               (float)d.scissor.w, (float)d.scissor.h, 0, 1};
                        SDL_SetGPUViewport(pass, &mvp);
                        lastMeshVp = d.scissor;
                        haveMeshVp = true;
                    }
                    SDL_DrawGPUPrimitives(pass, d.vertexCount, 1, d.firstVertex, d.meshParams);
                } else {
                    if (!haveFs || memcmp(&d.fs, &lastFs, sizeof(FSParams))) {
                        SDL_PushGPUFragmentUniformData(cmd, 0, &d.fs, sizeof(d.fs));
                        lastFs = d.fs;
                        haveFs = true;
                    }
                    SDL_DrawGPUPrimitives(pass, d.vertexCount, 1, d.firstVertex, 0);
                }
            }
            SDL_EndGPURenderPass(pass);
            i = e;
        } while (i < n);
    }

    g->verts.clear();
    g->draws.clear();
    g->meshParams.clear();
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return FAIL("SDL_SubmitGPUCommandBuffer");
    // Ranges freed while this frame's draws still used them become reusable now: later uploads
    // go into later command buffers, which the GPU orders after these draws.
    for (auto &f : g->arenaPendingFree) ArenaRelease(f.first, f.second);
    g->arenaPendingFree.clear();
    return true;
}

// Texture, sampler and combine parameters of the current state for one draw.
static void FillTexture(DrawCmd &d)
{
    State &s = g->st;
    d.fs.alphaTest = s.alphaTest;
    d.fs.alphaFunc = s.alphaFunc;
    d.fs.alphaRef = (s.alphaRef & 0xff) / 255.0f;
    d.fs.colorOp = s.colorOp;
    d.fs.alphaOp = s.alphaOp;
    if (g->textured && g->pCurTex && g->pCurTex->pTex) {
        CGraphicTexture &t = *g->pCurTex;
        bool linear = s.magFilter == TEXF_LINEAR || s.minFilter == TEXF_LINEAR;
        d.pTex = t.pTex;
        d.pSampler = GetSampler(s.addrU, s.addrV, linear);
        d.fs.mode = t.nFormat == TEXFMT_P8 ? 2 : 1;
        d.fs.addrU = s.addrU;
        d.fs.addrV = s.addrV;
        d.fs.linear = linear;
        d.fs.texW = t.nWidth;
        d.fs.texH = t.nHeight;
        uint32_t pal = g->curPalette < CGraphic::kNoPalette ? g->curPalette : 0;
        d.fs.palX = (pal % 16) * 256;
        d.fs.palY = pal / 16;
    } else {
        d.pTex = g->pWhite;
        d.pSampler = GetSampler(TADDRESS_CLAMP, TADDRESS_CLAMP, false);
        d.fs.mode = 0;
    }
}

// A draw that samples the scene target it draws into: the game blends its own picture onto itself
// (shifted by a line, at half strength, to soften it). A color target read in its own render pass
// is undefined in Vulkan (noise over the sky) and Metal; the PS2 read what was drawn so far. So:
// flush the recorded draws, copy the target and let the draw read the copy.
static CGraphicTexture *MakeRenderTarget(uint32_t w, uint32_t h);

static CGraphicTexture *FeedbackCopy()
{
    CGraphicTexture &rt = *g->tex[g->target];
    if (!Flush()) return nullptr;
    SDL_GPUDevice *dev = CGraphic::GetDevice();
    if (g->pFeedback && (g->pFeedback->nWidth != rt.nWidth || g->pFeedback->nHeight != rt.nHeight)) {
        SDL_ReleaseGPUTexture(dev, g->pFeedback->pTex);
        delete g->pFeedback;
        g->pFeedback = nullptr;
    }
    if (!g->pFeedback && !(g->pFeedback = MakeRenderTarget(rt.nWidth, rt.nHeight))) return nullptr;
    g->pFeedback->nUVWidth = rt.nUVWidth;
    g->pFeedback->nUVHeight = rt.nUVHeight;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return nullptr;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation src = {}, dst = {};
    src.texture = rt.pTex;
    dst.texture = g->pFeedback->pTex;
    SDL_CopyGPUTextureToTexture(copy, &src, &dst, rt.nWidth, rt.nHeight, 1, false);
    SDL_EndGPUCopyPass(copy);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return nullptr;
    return g->pFeedback;
}

// Record vertices for the current state. Fans become lists because SDL GPU has no fan topology.
static bool Record(const Vertex *v, uint32_t n)
{
    // sampling the target being drawn into: read a copy of it (FeedbackCopy)
    CGraphicTexture *sampled = g->pCurTex;
    if (g->textured && sampled && sampled == g->tex[g->target]) {
        CGraphicTexture *copy = FeedbackCopy();
        if (!copy) return false;
        g->pCurTex = copy;
    }
    struct Restore { CGraphicTexture *&p; CGraphicTexture *v; ~Restore() { p = v; } } restore{g->pCurTex, sampled};

    if (CACHED_ENV("RECOMP_SKIP_2D") && g_frameSerial >= strtoul(CACHED_ENV("RECOMP_SKIP_2D"), nullptr, 10)) return true;   // debugging
    // debugging: RECOMP_SKIP_2D_SEQ="<first frame>:<step>" skips 2D draws [k*step, (k+1)*step) of each
    // frame during the k-th block of 60 frames
    {
        static uint32_t serial, index;
        if (serial != g_frameSerial) { serial = g_frameSerial; index = 0; }
        uint32_t i = index++;
        if (const char *seq = CACHED_ENV("RECOMP_SKIP_2D_SEQ")) {
            uint32_t first = (uint32_t)strtoul(seq, nullptr, 10), step = (uint32_t)strtoul(strchr(seq, ':') + 1, nullptr, 10);
            if (g_frameSerial >= first && i / step == (g_frameSerial - first) / 60) return true;
        }
    }
    State &s = g->st;
    DrawCmd d = {};
    d.firstVertex = (uint32_t)g->verts.size();
    if (g->nViewport >= 0 && g->nViewport < 2) d.scissor = g->viewport[g->nViewport];
    CGraphicTexture &rt = *g->tex[g->target];
    if (d.scissor.w == 0) d.scissor = {0, 0, (int)rt.nWidth, (int)rt.nHeight};

    uint32_t topo = (uint32_t)CGraphic::m_nPrimType;
    if (topo == TRIANGLEFAN) {
        for (uint32_t i = 1; i + 1 < n; i++) {
            g->verts.push_back(v[0]); g->verts.push_back(v[i]); g->verts.push_back(v[i + 1]);
        }
        d.vertexCount = n >= 3 ? (n - 2) * 3 : 0;
        topo = TRIANGLELIST;
    } else if (topo == TRIANGLESTRIP) {
        // Strips (mostly 4-vertex sprites) become lists so consecutive draws with the same state
        // can be merged below; odd triangles keep the winding a strip gives them.
        for (uint32_t i = 0; i + 2 < n; i++) {
            const Vertex &a = v[i], &b = v[i + 1], &c = v[i + 2];
            if (i & 1) { g->verts.push_back(b); g->verts.push_back(a); }
            else { g->verts.push_back(a); g->verts.push_back(b); }
            g->verts.push_back(c);
        }
        d.vertexCount = n >= 3 ? (n - 2) * 3 : 0;
        topo = TRIANGLELIST;
    } else {
        g->verts.insert(g->verts.end(), v, v + n);
        d.vertexCount = n;
    }
    if (d.vertexCount == 0) return true;

    d.pipeKey = PipeKey(topo, s);
    FillTexture(d);
    if (const char *t = CACHED_ENV("RECOMP_TRACE_2D")) {        // debugging: "<frame serial>"
        static uint32_t serial, index;
        if (serial != g_frameSerial) { serial = g_frameSerial; index = 0; }
        if (g_frameSerial == strtoul(t, nullptr, 10) && index < 12)
            fprintf(stderr, "2d draw %u: topo %u n %u v0 (%.1f,%.1f,%.7f) v1 (%.1f,%.1f) color %08x tex %d z %u/%u/%u blend %u cw %u atest %u/%u\n",
                    index, topo, n, v[0].x, v[0].y, v[0].z, n > 1 ? v[n - 1].x : 0, n > 1 ? v[n - 1].y : 0, v[0].color,
                    d.fs.mode, s.zEnable, s.zWrite, s.zFunc, s.blendEnable, s.colorWrite, s.alphaTest, s.alphaFunc);
        index++;
    }
    // Append to the previous draw when nothing differs (HUD sprites come in long runs).
    if (topo == TRIANGLELIST && !g->draws.empty()) {
        DrawCmd &last = g->draws.back();
        if (!last.pMesh && last.pipeKey == d.pipeKey && last.pTex == d.pTex && last.pSampler == d.pSampler &&
            last.firstVertex + last.vertexCount == d.firstVertex && !memcmp(&last.scissor, &d.scissor, sizeof(SDL_Rect)) &&
            !memcmp(&last.fs, &d.fs, sizeof(FSParams))) {
            last.vertexCount += d.vertexCount;
            g_stats.mergedDraws++;
            return true;
        }
    }
    g->draws.push_back(d);
    return true;
}

static CGraphicTexture *MakeRenderTarget(uint32_t w, uint32_t h)
{
    auto *t = new CGraphicTexture();
    t->pTex = MakeGPUTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, w, h,
                             SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
    t->nWidth = t->nUVWidth = w;
    t->nHeight = t->nUVHeight = h;
    t->nFormat = TEXFMT_BGRA8;
    t->bRenderTarget = true;
    if (!t->pTex) { delete t; return nullptr; }
    return t;
}

// ---------------------------------------------------------------------------------------------
// Setup / teardown

bool CGraphic::LoadConfig(int argc, char **argv)
{
    // The original read HKCU\Software\TAITO\<game>; this reads the same keys from graphics.ini.
    // RECOMP_PREF_DIR replaces the settings folder (benchmarks and tests use their own)
    const char *override = getenv("RECOMP_PREF_DIR");
    char *pref = override ? SDL_strdup((std::string(override) + "/").c_str()) : SDL_GetPrefPath("TAITO", "DengoFinal");
    if (pref) {
        g_prefDir = pref;
        std::string path = std::string(pref) + "graphics.ini";
        SDL_free(pref);
        if (FILE *f = fopen(path.c_str(), "r")) {
            struct { const char *name; int *value; } keys[] = {
                {"windowed", &m_Config.windowed}, {"gpu", &m_Config.gpu}, {"scrwidth", &m_Config.scrwidth},
                {"scrheight", &m_Config.scrheight}, {"bbcount", &m_Config.bbcount}, {"scrformat", &m_Config.scrformat},
                {"refrate", &m_Config.refrate}, {"zformat", &m_Config.zformat}, {"texformat", &m_Config.texformat},
                {"texmodulate", &m_Config.texmodulate}, {"fog", &m_Config.fog}, {"texsize", &m_Config.texsize},
                {"screffect", &m_Config.screffect}, {"snapshot", &m_Config.snapshot},
                {"crop", &m_Config.crop}, {"texdump", &m_Config.texdump}, {"texpack", &m_Config.texpack},
                {"texpackmem", &m_Config.texpackmem},
            };
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char key[64]; int value;
                if (sscanf(line, " %63[^= ] = %d", key, &value) != 2) continue;
                for (auto &k : keys) if (SDL_strcasecmp(k.name, key) == 0) *k.value = value;
            }
            fclose(f);
        }
    }
    if (argc > 1 && (strcmp(argv[1], "W") == 0 || strcmp(argv[1], "w") == 0)) m_Config.windowed = 1;
    return true;
}

bool CGraphic::Init(SDL_Window *window)
{
    g = new Impl();
    g->pWindow = window;
    g->tex.assign(kMaxTextures, nullptr);
    g->texPalette.assign(kMaxTextures, kNoPalette);
    g->managedTex.assign(kMaxTextures, nullptr);
    g->managedPalette.assign(kMaxTextures, kNoPalette);
    g->paletteCPU.assign((size_t)kPaletteAtlasW * kPaletteAtlasH * 4, 0);
    g->paletteDirty.assign(kNoPalette, false);
    for (uint32_t i = kNoPalette; i-- > 0;) g->freePalettes.push_back(i);   // hand out low IDs first

    // RECOMP_GPU_DEBUG=1: SDL GPU debug mode (validation layers when installed)
    m_pDevice = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, CACHED_ENV("RECOMP_GPU_DEBUG") != nullptr,
                                    nullptr);
    if (!m_pDevice) return FAIL("SDL_CreateGPUDevice (Metal or Vulkan)");
    SDL_Log("Graphic: SDL GPU on %s", SDL_GetGPUDeviceDriver(m_pDevice));
    if (!SDL_ClaimWindowForGPUDevice(m_pDevice, window)) return FAIL("SDL_ClaimWindowForGPUDevice");
    SDL_SetGPUSwapchainParameters(m_pDevice, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    // Like the original's blocking D3D Present: acquiring the next swapchain image waits for
    // vsync, which paces the game (its own frame limiter then rarely has to sleep).
    SDL_SetGPUAllowedFramesInFlight(m_pDevice, 1);
    if (!m_Config.windowed) SDL_SetWindowFullscreen(window, true);

    m_nTexFormat = m_Config.texformat == 0 ? TEXFMT_P8 : TEXFMT_BGRA8;
    // texture packs work on BGRA texels (with P8 the palette is applied on the GPU)
    if (m_nTexFormat == TEXFMT_BGRA8) {
        // compressed (DDS) replacements need BC7 (Apple GPUs, desktop Vulkan); PNGs work everywhere
        bool bc7 = SDL_GPUTextureSupportsFormat(m_pDevice, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                                SDL_GPU_TEXTUREUSAGE_SAMPLER);
        texpack::Init(g_prefDir, m_Config.texdump != 0, m_Config.texpack != 0, (uint32_t)SDL_max(m_Config.texpackmem, 0), bc7);
    }
    else if (m_Config.texdump)
        SDL_Log("texture dumps and packs need texformat = 1 (32-bit textures)");
    static const uint32_t sizes[4] = {256, 512, 1024, 1024};
    g->maxTexW = g->maxTexH = sizes[SDL_clamp(m_Config.texsize, 0, 3)];
    // Original default: MODULATE2X unless texmodulate == 2 (PS2 vertex colours use 0x80 as 1.0).
    g->st.colorOp = GetTextureModulate();

    const SDL_GPUTextureFormat depthCandidates[] = {
        SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREFORMAT_D16_UNORM};
    for (SDL_GPUTextureFormat f : depthCandidates) {
        if (SDL_GPUTextureSupportsFormat(m_pDevice, f, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            g->depthFormat = f;
            break;
        }
    }

    const char *flatPrefix = "#define COLOR_INTERP [[flat]]\n";
    g->pVS[0] = MakeShader(kSceneShaderMSL, "vs_main", kSpvVSMain, SDL_GPU_SHADERSTAGE_VERTEX, 0);
    g->pFS[0] = MakeShader(kSceneShaderMSL, "fs_main", kSpvFSMain, SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
    g->pVS[1] = MakeShader(kSceneShaderMSL, "vs_main", kSpvVSMainFlat, SDL_GPU_SHADERSTAGE_VERTEX, 0, flatPrefix);
    g->pFS[1] = MakeShader(kSceneShaderMSL, "fs_main", kSpvFSMainFlat, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, flatPrefix);
    g->pMeshVS[0] = MakeShader(kSceneShaderMSL, "vs_mesh", kSpvVSMesh, SDL_GPU_SHADERSTAGE_VERTEX, 0, "", 0, 1);
    // RECOMP_DEBUG_MESH_UV (debugging, Metal only): mesh fragments output their texture coordinates
    const char *meshFSPrefix = CACHED_ENV("RECOMP_DEBUG_MESH_UV") ? "#define DEBUG_MESH_UV 1\n"
                             : CACHED_ENV("RECOMP_DEBUG_MESH_ID") ? "#define DEBUG_MESH_ID 1\n"
                             : CACHED_ENV("RECOMP_DEBUG_MESH_TEX") ? "#define DEBUG_MESH_TEX 1\n"
                             : CACHED_ENV("RECOMP_DEBUG_MESH_READ") ? "#define DEBUG_MESH_READ 1\n"
                             : CACHED_ENV("RECOMP_DEBUG_MESH_SHADE") ? "#define DEBUG_MESH_SHADE 1\n"
                             : CACHED_ENV("RECOMP_DEBUG_MESH_PARAMS") ? "#define DEBUG_MESH_PARAMS 1\n"
                             : CACHED_ENV("RECOMP_DEBUG_MESH_COLOR") ? "#define DEBUG_MESH_COLOR 1\n" : "";
    g->pMeshFS[0] = MakeShader(kSceneShaderMSL, "fs_mesh", kSpvFSMesh, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, meshFSPrefix, 0, 1);
    g->pMeshVS[1] = MakeShader(kSceneShaderMSL, "vs_mesh", kSpvVSMeshFlat, SDL_GPU_SHADERSTAGE_VERTEX, 0, flatPrefix, 0, 1);
    g->pMeshFS[1] = MakeShader(kSceneShaderMSL, "fs_mesh", kSpvFSMeshFlat, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, flatPrefix, 0, 1);
    if (!g->pMeshVS[0] || !g->pMeshFS[0] || !g->pMeshVS[1] || !g->pMeshFS[1]) return FAIL("SDL_CreateGPUShader (mesh)");
    g->pPresentVS = MakeShader(kPresentShaderMSL, "vs_present", kSpvVSPresent, SDL_GPU_SHADERSTAGE_VERTEX, 0);
    g->pPresentFS = MakeShader(kPresentShaderMSL, "fs_present", kSpvFSPresent, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
    if (!g->pVS[0] || !g->pFS[0] || !g->pVS[1] || !g->pFS[1] || !g->pPresentVS || !g->pPresentFS)
        return FAIL("SDL_CreateGPUShader");

    SDL_GPUColorTargetDescription ct = {};
    ct.format = SDL_GetGPUSwapchainTextureFormat(m_pDevice, window);
    SDL_GPUGraphicsPipelineCreateInfo pi = {};
    pi.vertex_shader = g->pPresentVS;
    pi.fragment_shader = g->pPresentFS;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.target_info.color_target_descriptions = &ct;
    pi.target_info.num_color_targets = 1;
    g->pPresentPipe = SDL_CreateGPUGraphicsPipeline(m_pDevice, &pi);
    if (!g->pPresentPipe) return FAIL("SDL_CreateGPUGraphicsPipeline (present)");
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g->pPresentSampler = SDL_CreateGPUSampler(m_pDevice, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    g->pOverlaySampler = SDL_CreateGPUSampler(m_pDevice, &si);

    // Scene targets (texture slots 0 and 1), depth buffer, helpers.
    uint32_t w = (uint32_t)m_Config.scrwidth, h = (uint32_t)m_Config.scrheight;
    g->tex[0] = MakeRenderTarget(w, h);
    g->tex[1] = MakeRenderTarget(w, h);
    if (!g->tex[0] || !g->tex[1]) return FAIL("render targets");
    g->pDepth = MakeGPUTexture(g->depthFormat, w, h, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
    g->pPaletteAtlas = MakeGPUTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, kPaletteAtlasW, kPaletteAtlasH,
                                      SDL_GPU_TEXTUREUSAGE_SAMPLER);
    CGraphicTexture *white = CreateTexture(1, 1, TEXFMT_BGRA8);
    if (!g->pDepth || !g->pPaletteAtlas || !white) return FAIL("SDL_CreateGPUTexture");
    white->staging = {0xff, 0xff, 0xff, 0xff};

    // Upload the white texel and clear both scene targets.
    for (int i = 1; i >= 0; i--) {
        g->target = i;
        g->pendingClearColor = g->pendingClearDepth = true;
        if (!Flush()) return false;
    }
    g->pWhite = white->pTex;                         // keep the GPU texture, drop the wrapper
    delete white;

    // Viewports and letterbox borders, as in the original (640x480 layout scaled to the target).
    auto sx = [w](int v) { return (int)((uint32_t)v * w / 640); };
    auto sy = [h](int v) { return (int)((uint32_t)v * h / 480); };
    g->viewport[0] = {0, 0, (int)w, (int)h};
    g->viewport[1] = {sx(20), sy(30), sx(600), sy(420)};
    g->border[0] = {0, 0, (int)w, sy(30)};
    g->border[1] = {0, sy(450), (int)w, (int)h - sy(450)};
    g->border[2] = {0, sy(30), sx(20), sy(420)};
    g->border[3] = {sx(620), sy(30), (int)w - sx(620), sy(420)};
    return true;
}

void CGraphic::Term()
{
    texpack::Term();
    if (!g) return;
    if (m_pDevice) {
        SDL_WaitForGPUIdle(m_pDevice);
        std::vector<CGraphicTexture *> all = g->tex;
        all.insert(all.end(), g->managedTex.begin(), g->managedTex.end());
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        for (CGraphicTexture *t : all) {
            if (!t) continue;
            if (t->pTex) SDL_ReleaseGPUTexture(m_pDevice, t->pTex);
            delete t;
        }
        for (auto &p : g->pipes) SDL_ReleaseGPUGraphicsPipeline(m_pDevice, p.second);
        for (auto &s : g->samplers) SDL_ReleaseGPUSampler(m_pDevice, s.second);
        if (g->pPresentPipe) SDL_ReleaseGPUGraphicsPipeline(m_pDevice, g->pPresentPipe);
        if (g->pPresentSampler) SDL_ReleaseGPUSampler(m_pDevice, g->pPresentSampler);
        if (g->pOverlaySampler) SDL_ReleaseGPUSampler(m_pDevice, g->pOverlaySampler);
        if (g->pOverlayTex) SDL_ReleaseGPUTexture(m_pDevice, g->pOverlayTex);
        if (g->pFeedback) { SDL_ReleaseGPUTexture(m_pDevice, g->pFeedback->pTex); delete g->pFeedback; }
        for (SDL_GPUShader *s : {g->pVS[0], g->pVS[1], g->pFS[0], g->pFS[1], g->pMeshVS[0], g->pMeshVS[1],
                                 g->pMeshFS[0], g->pMeshFS[1], g->pPresentVS, g->pPresentFS})
            if (s) SDL_ReleaseGPUShader(m_pDevice, s);
        for (CGraphicMesh *m : g->meshes) delete m;
        if (g->pMeshArena) SDL_ReleaseGPUBuffer(m_pDevice, g->pMeshArena);
        for (SDL_GPUTexture *t : {g->pDepth, g->pWhite, g->pPaletteAtlas}) if (t) SDL_ReleaseGPUTexture(m_pDevice, t);
        if (g->pVB) SDL_ReleaseGPUBuffer(m_pDevice, g->pVB);
        if (g->pUpload) SDL_ReleaseGPUTransferBuffer(m_pDevice, g->pUpload);
        if (g->pWindow) SDL_ReleaseWindowFromGPUDevice(m_pDevice, g->pWindow);
        SDL_DestroyGPUDevice(m_pDevice);
        m_pDevice = nullptr;
    }
    delete g;
    g = nullptr;
}

bool CGraphic::Reset() { return true; }

// ---------------------------------------------------------------------------------------------
// Frame

CGraphic::DebugStats &CGraphic::GetDebugStats() { g_stats.pipelines = g ? (uint32_t)g->pipes.size() : 0; return g_stats; }

uint32_t CGraphic::GetFrameSerial() { return g_frameSerial; }
void CGraphic::HoldRecordingLock(bool) {}
bool CGraphic::Threaded() { return false; }
uint64_t CGraphic::FrameLoadNs() { return 0; }

bool CGraphic::BeginFrame()
{
    g_frameSerial++;
    // texture pack replacements decoded since the last frame: upload them in place of the originals
    for (size_t i = 0; i < g->packWaiting.size();) {
        CGraphicTexture *t = g->packWaiting[i];
        if (t->packWaiting && !texpack::Find(t->packHash)) { i++; continue; }
        if (t->packWaiting) { t->packWaiting = false; MarkDirty(t); }
        g->packWaiting[i] = g->packWaiting.back();
        g->packWaiting.pop_back();
    }
    // Original: BeginScene, then render into texture slot 0 (screffect) or clear the letterbox
    // borders, then SetViewport(2, TRUE) to re-apply the current viewport.
    if (!SetRenderTargetTexture(0)) return false;
    // The original cleared the letterbox borders only without screffect. With screffect the
    // two scene textures alternate, so anything drawn outside the game's own clear (text
    // scrolling past the 20/30-pixel margins) piled up there as trails; clear them every frame.
    if (!ClearBorders()) return false;
    return SetViewport(2, true);
}

static bool PresentTexture(SDL_GPUTexture *pScene, uint32_t w, uint32_t h);

bool CGraphic::EndFrame(bool bSnapshotKey)
{
    if (!Flush()) return false;
    if (m_Config.snapshot && bSnapshotKey) PrintScreen();

    std::lock_guard<std::recursive_mutex> lock(g->gpuLock);
    CGraphicTexture &scene = *g->tex[0];
    if (g->deferPresent) {
        // hand the frame to the window thread: copy it into the present texture
        if (!g->pPresentTex)
            g->pPresentTex = MakeGPUTexture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, scene.nWidth, scene.nHeight,
                                            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_pDevice);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src = {}, dst = {};
        src.texture = scene.pTex;
        dst.texture = g->pPresentTex;
        SDL_CopyGPUTextureToTexture(copy, &src, &dst, scene.nWidth, scene.nHeight, 1, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        g->framePending = true;
    } else if (!PresentTexture(scene.pTex, scene.nWidth, scene.nHeight)) {
        return false;
    }
    // ApplyScreenEffect: keep this frame as texture slot 1 for the next one.
    if (m_Config.screffect) std::swap(g->tex[0], g->tex[1]);
    return true;
}

void CGraphic::SetDeferredPresent(bool bDefer) { if (g) g->deferPresent = bDefer; }

bool CGraphic::PresentPending()
{
    if (!g) return false;
    std::lock_guard<std::recursive_mutex> lock(g->gpuLock);
    if (!g->framePending || !g->pPresentTex) return false;
    g->framePending = false;
    return PresentTexture(g->pPresentTex, g->tex[0]->nWidth, g->tex[0]->nHeight);
}

// CGraphic::SetOverlay: the image (rows top-down) and its version, drawn by PresentTexture
static struct {
    std::mutex lock;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    uint32_t version = 0;
} g_overlay;

void CGraphic::SetOverlay(const uint8_t *pRGBA, int nWidth, int nHeight)
{
    std::lock_guard<std::mutex> lk(g_overlay.lock);
    if (!pRGBA || nWidth <= 0 || nHeight <= 0) { g_overlay.w = g_overlay.h = 0; return; }
    g_overlay.px.assign(pRGBA, pRGBA + (size_t)nWidth * nHeight * 4);
    g_overlay.w = nWidth;
    g_overlay.h = nHeight;
    g_overlay.version++;
}

bool CGraphic::HasFpsCounter() { return false; }
bool CGraphic::FpsCounter() { return false; }
void CGraphic::SetFpsCounter(bool) {}

// Uploads a changed overlay image in cmd (before the render pass); the overlay's size, 0 = none.
static void UploadOverlay(SDL_GPUCommandBuffer *cmd, int &w, int &h)
{
    std::lock_guard<std::mutex> lk(g_overlay.lock);
    w = g_overlay.w;
    h = g_overlay.h;
    if (!w) return;
    SDL_GPUDevice *dev = CGraphic::GetDevice();
    if (g->pOverlayTex && g->overlayVersion == g_overlay.version) return;
    if (!g->pOverlayTex || g->overlayTexW != w || g->overlayTexH != h) {
        if (g->pOverlayTex) SDL_ReleaseGPUTexture(dev, g->pOverlayTex);
        g->pOverlayTex = MakeGPUTexture(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, (uint32_t)w, (uint32_t)h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        g->overlayTexW = w;
        g->overlayTexH = h;
        if (!g->pOverlayTex) { w = 0; return; }
    }
    uint32_t size = (uint32_t)w * (uint32_t)h * 4;
    SDL_GPUTransferBufferCreateInfo ti = {};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = size;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    if (!tb) { w = 0; return; }
    memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), g_overlay.px.data(), size);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = tb;
    SDL_GPUTextureRegion dst = {};
    dst.texture = g->pOverlayTex;
    dst.w = (Uint32)w;
    dst.h = (Uint32)h;
    dst.d = 1;
    SDL_UploadToGPUTexture(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    g->overlayVersion = g_overlay.version;
}

static bool PresentTexture(SDL_GPUTexture *pScene, uint32_t w, uint32_t h)
{
    SDL_GPUDevice *m_pDevice = CGraphic::GetDevice();
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_pDevice);
    if (!cmd) return FAIL("SDL_AcquireGPUCommandBuffer");
    SDL_GPUTexture *swap = nullptr;
    Uint32 sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, g->pWindow, &swap, &sw, &sh)) {
        SDL_CancelGPUCommandBuffer(cmd);
        return FAIL("SDL_WaitAndAcquireGPUSwapchainTexture");
    }
    if (swap) {
        int ow = 0, oh = 0;
        UploadOverlay(cmd, ow, oh);
        // Scale the scene into the window, keeping its aspect ratio. With `crop` only the game's
        // canvas is shown: the PS2 game draws its picture at (20,30)-(620,450) of the 640x480
        // layout (the same share of its 640x448 frame on both axes, so it displays at 4:3) and
        // leaves the rest to TV overscan; 3D, effects and fades outside it are not meant to be seen.
        // The canvas then displays at the target's own aspect (4:3 for 640x480; with a wider
        // scrwidth the 2D is stretched and the 3D aspect setting widens the view).
        float u0 = 0, v0 = 0, us = 1, vs = 1, aspect = (float)w / (float)h;
        if (CGraphic::m_Config.crop) {
            u0 = 20.0f / 640.0f; v0 = 30.0f / 480.0f; us = 600.0f / 640.0f; vs = 420.0f / 480.0f;
        }
        float vw = SDL_min((float)sw, (float)sh * aspect), vh = vw / aspect;
        SDL_GPUColorTargetInfo color = {};
        color.texture = swap;
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        color.clear_color = {0, 0, 0, 1};
        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
        SDL_GPUViewport vp = {(sw - vw) * 0.5f, (sh - vh) * 0.5f, vw, vh, 0, 1};
        SDL_SetGPUViewport(pass, &vp);
        SDL_BindGPUGraphicsPipeline(pass, g->pPresentPipe);
        SDL_GPUTextureSamplerBinding tb = {pScene, g->pPresentSampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &tb, 1);
        // sampling stays half a texel inside the shown part: bilinear filtering at the screen edge
        // would otherwise mix in the overscan margin (stripes at the edges with crop)
        float hu = 0.5f / (float)w, hv = 0.5f / (float)h;
        PresentParams pp = {1.0f / (g->gamma > 0 ? g->gamma : 1.0f), u0, v0, us, vs, u0 + hu, v0 + hv, u0 + us - hu, v0 + vs - hv};
        SDL_PushGPUFragmentUniformData(cmd, 0, &pp, sizeof(pp));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        if (ow) {                                   // the overlay: centred, integer scale, opaque
            int scale = SDL_max(1, SDL_min((int)sw * 15 / 16 / ow, (int)sh * 15 / 16 / oh));
            SDL_GPUViewport ovp = {(float)(((int)sw - ow * scale) / 2), (float)(((int)sh - oh * scale) / 2),
                                   (float)(ow * scale), (float)(oh * scale), 0, 1};
            SDL_SetGPUViewport(pass, &ovp);
            SDL_GPUTextureSamplerBinding ob = {g->pOverlayTex, g->pOverlaySampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &ob, 1);
            PresentParams op = {1.0f, 0, 0, 1, 1, 0, 0, 1, 1};
            SDL_PushGPUFragmentUniformData(cmd, 0, &op, sizeof(op));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return FAIL("SDL_SubmitGPUCommandBuffer");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Handle-based textures and palettes

uint32_t CGraphic::PowerOfTwo(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

uint32_t CGraphic::GetMaxTextureWidth() { return g ? g->maxTexW : 1024; }
uint32_t CGraphic::GetMaxTextureHeight() { return g ? g->maxTexH : 1024; }

CGraphicTexture *CGraphic::CreateTexture(uint32_t nWidth, uint32_t nHeight, TexFormat fmt)
{
    auto *t = new CGraphicTexture();
    t->nFormat = fmt;
    t->nWidth = t->nUVWidth = SDL_max(nWidth, 1u);
    t->nHeight = t->nUVHeight = SDL_max(nHeight, 1u);
    t->pTex = MakeGPUTexture(fmt == TEXFMT_P8 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM : SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
                             t->nWidth, t->nHeight, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!t->pTex) { delete t; FAIL("SDL_CreateGPUTexture"); return nullptr; }
    t->staging.assign((size_t)t->nWidth * t->nHeight * BytesPerTexel(fmt), 0);
    t->gpuW = t->nWidth;
    t->gpuH = t->nHeight;
    MarkDirty(t);
    g_stats.liveTextures++; g_stats.texCreates++;
    return t;
}

void CGraphic::DestroyTexture(CGraphicTexture *pTex)
{
    if (!pTex) return;
    Flush();                                            // recorded draws may still use it
    g->dirtyTex.erase(std::remove(g->dirtyTex.begin(), g->dirtyTex.end(), pTex), g->dirtyTex.end());
    if (pTex->packWaiting)
        g->packWaiting.erase(std::remove(g->packWaiting.begin(), g->packWaiting.end(), pTex), g->packWaiting.end());
    if (g->pCurTex == pTex) g->pCurTex = nullptr;
    for (auto &p : g->managedTex) if (p == pTex) p = nullptr;
    for (auto &p : g->tex) if (p == pTex) p = nullptr;
    if (pTex->pTex) SDL_ReleaseGPUTexture(m_pDevice, pTex->pTex);
    delete pTex;
    g_stats.liveTextures--; g_stats.texDestroys++;
}

void *CGraphic::LockTexture(CGraphicTexture *pTex, uint32_t *pPitch)
{
    if (!pTex || pTex->bRenderTarget) return nullptr;
    if (pPitch) *pPitch = pTex->nWidth * BytesPerTexel(pTex->nFormat);
    return pTex->staging.data();
}

void CGraphic::UnlockTexture(CGraphicTexture *pTex)
{
    if (!pTex || pTex->bRenderTarget) return;
    if (!pTex->bDirty) Flush();                         // draws recorded so far saw the old texels
    MarkDirty(pTex);
}

void CGraphic::SetTextureUVSize(CGraphicTexture *pTex, uint32_t nWidth, uint32_t nHeight)
{
    if (pTex) { pTex->nUVWidth = nWidth; pTex->nUVHeight = nHeight; }
}

uint32_t CGraphic::AllocPalette()
{
    if (g->freePalettes.empty()) return kNoPalette;
    uint32_t id = g->freePalettes.back();
    g->freePalettes.pop_back();
    return id;
}

void CGraphic::FreePalette(uint32_t nPalID)
{
    if (nPalID < kNoPalette) g->freePalettes.push_back(nPalID);
}

bool CGraphic::UploadPalette(uint32_t nPalID, const uint8_t *pRGBA, uint32_t nCount)
{
    if (nPalID >= kNoPalette) return false;
    if (!g->paletteDirty[nPalID]) Flush();             // keep earlier draws on the old palette
    uint32_t px = (nPalID % 16) * 256, py = nPalID / 16;
    memcpy(&g->paletteCPU[((size_t)py * kPaletteAtlasW + px) * 4], pRGBA, SDL_min(nCount, 256u) * 4);
    if (!g->paletteDirty[nPalID]) {
        g->paletteDirty[nPalID] = true;
        g->dirtyPalettes.push_back(nPalID);
    }
    return true;
}

void CGraphic::SetManagedTexture(uint32_t nHandle, CGraphicTexture *pTex)
{
    if (nHandle < kMaxTextures) g->managedTex[nHandle] = pTex;
}

void CGraphic::SetManagedPalette(uint32_t nHandle, uint32_t nPalID)
{
    if (nHandle < kMaxTextures) g->managedPalette[nHandle] = nPalID;
}

// ---------------------------------------------------------------------------------------------
// ID-based textures (the original direct table, m_TexInfo)

bool CGraphic::SetGamma(float fGamma, bool bStore)
{
    if (fGamma > 0.0f && bStore) g->gamma = fGamma;
    return true;
}

TEXSIZE *CGraphic::FindTexSize(int nID)
{
    return g->texSize.nID == nID ? &g->texSize : nullptr;
}

bool CGraphic::LoadTexture(uint32_t nID, uint32_t nWidth, uint32_t nHeight)
{
    if (nID < 2 || nID >= kMaxTextures) return false;   // 0 and 1 are the scene targets
    CGraphicTexture *&t = g->tex[nID];
    if (TEXSIZE *ts = FindTexSize((int)nID)) {
        if (t) return true;                             // fixed-size slot already exists
        nWidth = ts->nWidth;
        nHeight = ts->nHeight;
    }
    // D3DXCheckTextureRequirements rounded up to powers of two and clamped to the device limit.
    nWidth = SDL_min(PowerOfTwo(nWidth), g->maxTexW);
    nHeight = SDL_min(PowerOfTwo(nHeight), g->maxTexH);
    if (t && t->nWidth == nWidth && t->nHeight == nHeight && t->nFormat == m_nTexFormat) return true;

    DestroyTexture(t);
    t = CreateTexture(nWidth, nHeight, m_nTexFormat);
    return t != nullptr;
}

void *CGraphic::LockTexture(uint32_t nID, uint32_t *pPitch)
{
    return nID < kMaxTextures ? LockTexture(g->tex[nID], pPitch) : nullptr;
}

void CGraphic::UnlockTexture(uint32_t nID)
{
    if (nID < kMaxTextures) UnlockTexture(g->tex[nID]);
}

bool CGraphic::LoadTexture(uint32_t nID, const void *pSrc, int x, int y, int w, int h, int nBits)
{
    if (nID < 2 || nID >= kMaxTextures || w <= 0 || h <= 0) return false;
    if (!LoadTexture(nID, (uint32_t)w, (uint32_t)h)) return false;
    CGraphicTexture *t = g->tex[nID];
    // The original passed no CLUT here, so indexed uploads only work with P8 textures.
    uint32_t pitch = 0;
    auto *dst = static_cast<uint8_t *>(LockTexture(t, &pitch));
    if (x + w > (int)t->nWidth || y + h > (int)t->nHeight) return false;
    bool ok = dst && ps2::Decode(static_cast<const uint8_t *>(pSrc), nBits, w, h, x, y, 1, 1, nullptr,
                                 t->nFormat, dst, pitch);
    UnlockTexture(t);
    return ok;
}

bool CGraphic::SetPalette(uint32_t nID, const void *pPs2Clut, uint32_t nCount)
{
    // The original only kept palettes for P8 textures.
    if (m_nTexFormat != TEXFMT_P8 || nID >= kMaxTextures) return false;
    uint32_t &pal = g->texPalette[nID];
    if (pal >= kNoPalette && (pal = AllocPalette()) >= kNoPalette) return false;
    uint8_t rgba[256 * 4] = {};
    ps2::ConvertClut(static_cast<const uint8_t *>(pPs2Clut), (int)SDL_min(nCount, 256u), rgba);
    return UploadPalette(pal, rgba, 256);
}

uint32_t CGraphic::GetTextureModulate()
{
    return m_Config.texmodulate == 2 ? TOP_MODULATE : TOP_MODULATE2X;
}

bool CGraphic::SetTexture(uint32_t nID, uint8_t nGroup, uint32_t nPalID)
{
    // nID >= 0x4000 selects untextured drawing. nGroup 0x0F selects CTexture's table for both
    // the texture and the palette; otherwise the palette comes from direct slot nPalID.
    g->textured = nID < kMaxTextures;
    if (!g->textured) { g->pCurTex = nullptr; return true; }
    bool managed = nGroup == kGroupManaged;
    g->pCurTex = managed ? g->managedTex[nID] : g->tex[nID];
    g->curPalette = nPalID >= kMaxTextures ? kNoPalette
                  : managed ? g->managedPalette[nPalID] : g->texPalette[nPalID];
    return g->pCurTex != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Immediate-mode 2D drawing

bool CGraphic::NormalizeUV(float *pu, float *pv)
{
    if (!g->textured || !g->pCurTex) return false;
    *pu /= (float)g->pCurTex->nUVWidth;
    *pv /= (float)g->pCurTex->nUVHeight;
    return true;
}

bool CGraphic::SetVertex(int i, D3DCOLOR color, const float *pos, float u, float v)
{
    if (i < 0 || i > 3) return false;
    if (!g->textured) {
        g->vertex2D[i] = {pos[0], pos[1], pos[2], 1.0f, color};
        return true;
    }
    if (!g->pCurTex) return false;
    g->texVertex[i] = {pos[0], pos[1], pos[2], 1.0f, color, u, v};
    return true;
}

bool CGraphic::AddStripVertex(int i, D3DCOLOR color, const float *pos, float u, float v)
{
    // Same data movement as the original (0x403b70): the GS emulator streams strip vertices
    // through slots 1..3, so for i == 3 the window moves down and the new vertex lands in slot 2.
    if (i < 1 || i > 3) return false;
    if (!g->textured) {
        TLVERTEX *vtx = g->vertex2D;
        if (i == 3) { vtx[1] = vtx[3]; i = 2; }
        vtx[0] = vtx[i];
        vtx[i] = {pos[0], pos[1], pos[2], 1.0f, color};
        return true;
    }
    if (!g->pCurTex) return false;
    TLVERTEX_TEX *vtx = g->texVertex;
    if (i == 3) { vtx[1] = vtx[3]; i = 2; }
    vtx[0] = vtx[i];
    vtx[i] = {pos[0], pos[1], pos[2], 1.0f, color, u, v};
    return true;
}

bool CGraphic::SetSpriteRect(D3DCOLOR color, const float *pos, float u, float v)
{
    // Vertex 0 is the top-left corner set by SetVertex(0, ...); pos/u/v give the bottom-right.
    // Builds a 4-vertex strip: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.
    if (!g->textured) {
        TLVERTEX *vtx = g->vertex2D, &a = vtx[0];
        vtx[3] = {pos[0], pos[1], pos[2], 1.0f, color};
        vtx[1] = {pos[0], a.y, a.z, a.rhw, a.color};
        vtx[2] = {a.x, pos[1], a.z, a.rhw, a.color};
        return true;
    }
    if (!g->pCurTex) return false;
    TLVERTEX_TEX *vtx = g->texVertex, &a = vtx[0];
    vtx[3] = {pos[0], pos[1], pos[2], 1.0f, color, u, v};
    vtx[1] = {pos[0], a.y, a.z, a.rhw, a.color, u, a.v};
    vtx[2] = {a.x, pos[1], a.z, a.rhw, a.color, a.u, v};
    return true;
}

bool CGraphic::DrawPrimitive(uint32_t nVertexCount, uint32_t nPrimCount)
{
    (void)nPrimCount;   // implied by the topology and vertex count
    // The original sets D3DRS_CULLMODE = D3DCULL_NONE on every call: the PS2 GS has no culling.
    g->st.cull = CULL_NONE;
    nVertexCount = SDL_min(nVertexCount, 4u);
    Vertex v[4];
    if (g->textured) {
        if (!g->pCurTex) return false;
        for (uint32_t i = 0; i < nVertexCount; i++) {
            const TLVERTEX_TEX &s = g->texVertex[i];
            v[i] = {s.x, s.y, s.z, s.rhw, s.color, s.u, s.v};
        }
    } else {
        for (uint32_t i = 0; i < nVertexCount; i++) {
            const TLVERTEX &s = g->vertex2D[i];
            v[i] = {s.x, s.y, s.z, s.rhw, s.color, 0, 0};
        }
    }
    return Record(v, nVertexCount);
}

// ---------------------------------------------------------------------------------------------
// Render state

void CGraphic::SetRenderState(RenderState state, uint32_t value)
{
    State &s = g->st;
    switch (state) {
    case RS_ZENABLE:          s.zEnable = value; break;
    case RS_ZWRITEENABLE:     s.zWrite = value; break;
    case RS_COLORWRITEENABLE: s.colorWrite = value & 15; break;
    case RS_ZFUNC:            s.zFunc = value; break;
    case RS_ALPHABLENDENABLE: s.blendEnable = value; break;
    case RS_SRCBLEND:         s.srcBlend = value; break;
    case RS_DESTBLEND:        s.dstBlend = value; break;
    case RS_BLENDOP:          s.blendOp = value; break;
    case RS_CULLMODE:         s.cull = value; break;
    case RS_SHADEMODE:        s.shade = value; break;
    case RS_ALPHATESTENABLE:  s.alphaTest = value; break;
    case RS_ALPHAFUNC:        s.alphaFunc = value; break;
    case RS_ALPHAREF:         s.alphaRef = value; break;
    // Fog is applied to 3D models only: pre-transformed 2D vertices carry no fog factor.
    case RS_FOGENABLE:        s.fogEnable = value; break;
    case RS_FOGCOLOR:         s.fogColor = value; break;
    // Dithering and edge antialiasing have no modern equivalent.
    case RS_DITHERENABLE: case RS_EDGEANTIALIAS: break;
    }
}

void CGraphic::SetTextureStageState(uint32_t stage, TextureStageState type, uint32_t value)
{
    if (stage != 0) return;     // the game only uses stage 0
    State &s = g->st;
    switch (type) {
    case TSS_COLOROP:   s.colorOp = value; break;
    case TSS_ALPHAOP:   s.alphaOp = value; break;
    case TSS_ADDRESSU:  s.addrU = value; break;
    case TSS_ADDRESSV:  s.addrV = value; break;
    case TSS_MAGFILTER: s.magFilter = value; break;
    case TSS_MINFILTER: s.minFilter = value; break;
    }
}

bool CGraphic::Clear(uint32_t flags, D3DCOLOR color, float z)
{
    // Drawn as a full-target quad (depth test always) instead of a render-pass clear, so the
    // frame stays in one GPU submission: the GS emulator clears depth several times per frame.
    if (!(flags & (CLEAR_TARGET | CLEAR_ZBUFFER))) return true;
    if (CACHED_ENV("RECOMP_CLEAR_FLUSH")) {             // A/B comparison: the render-pass clear
        if (!Flush()) return false;
        if (flags & CLEAR_TARGET) {
            g->pendingClearColor = true;
            g->clearColor = {((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f,
                             (color & 0xff) / 255.0f, ((color >> 24) & 0xff) / 255.0f};
        }
        if (flags & CLEAR_ZBUFFER) { g->pendingClearDepth = true; g->clearDepth = z; }
        return true;
    }
    State saved = g->st;
    bool savedTextured = g->textured;
    int savedVp = g->nViewport;
    PrimitiveType savedPrim = m_nPrimType;
    g->st.blendEnable = 0; g->st.alphaTest = 0; g->st.cull = CULL_NONE; g->st.shade = SHADE_GOURAUD;
    g->st.fogEnable = 0;
    g->st.zEnable = 1; g->st.zFunc = CMP_ALWAYS; g->st.zWrite = (flags & CLEAR_ZBUFFER) ? 1 : 0;
    g->st.colorWrite = (flags & CLEAR_TARGET) ? 15 : 0;
    g->textured = false;
    g->nViewport = -1;              // whole target
    m_nPrimType = TRIANGLESTRIP;
    CGraphicTexture &rt = *g->tex[g->target];
    float x0 = -0.5f, y0 = -0.5f, x1 = rt.nWidth - 0.5f, y1 = rt.nHeight - 0.5f;
    D3DCOLOR c = (flags & CLEAR_TARGET) ? color : 0;
    Vertex v[4] = {{x0, y0, z, 1, c, 0, 0}, {x1, y0, z, 1, c, 0, 0}, {x0, y1, z, 1, c, 0, 0}, {x1, y1, z, 1, c, 0, 0}};
    Record(v, 4);
    g->st = saved;
    g->textured = savedTextured;
    g->nViewport = savedVp;
    m_nPrimType = savedPrim;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Screen, viewport and render targets

bool CGraphic::ClearBorders()
{
    // Original: Clear(4, m_rcBorder, D3DCLEAR_TARGET, black). Rect clears are drawn as quads.
    State saved = g->st;
    bool savedTextured = g->textured;
    int savedVp = g->nViewport;
    PrimitiveType savedPrim = m_nPrimType;
    g->st.blendEnable = 0; g->st.zEnable = 0; g->st.alphaTest = 0; g->st.cull = CULL_NONE; g->st.shade = SHADE_GOURAUD;
    g->st.colorWrite = 15;
    g->textured = false;
    g->nViewport = -1;              // full-target scissor
    m_nPrimType = TRIANGLESTRIP;
    for (const SDL_Rect &r : g->border) {
        float x0 = (float)r.x - 0.5f, y0 = (float)r.y - 0.5f, x1 = x0 + r.w, y1 = y0 + r.h;
        Vertex v[4] = {{x0, y0, 0, 1, 0xff000000, 0, 0}, {x1, y0, 0, 1, 0xff000000, 0, 0},
                       {x0, y1, 0, 1, 0xff000000, 0, 0}, {x1, y1, 0, 1, 0xff000000, 0, 0}};
        Record(v, 4);
    }
    g->st = saved;
    g->textured = savedTextured;
    g->nViewport = savedVp;
    m_nPrimType = savedPrim;
    return true;
}

bool CGraphic::SetViewport(int nType, bool bApply)
{
    // nType 0 = full screen, 1 = inset; 2 re-applies the current one. When switching to a
    // viewport type with screffect on, the original cleared both render targets first.
    if (nType < 2 && bApply) {
        if (nType > g->nViewport && m_Config.screffect) {
            int saved = g->target;
            for (int i = 0; i < 2; i++) {
                if (!SetRenderTargetTexture(i) || !Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xff000000, 1.0f)) return false;
            }
            Flush();
            SetRenderTargetTexture(saved);
        }
        g->nViewport = nType;
    } else if (nType < 2) {
        g->nViewport = nType;
    }
    return true;
}

bool CGraphic::SetRenderTargetTexture(int nType)
{
    if (nType < 0 || nType > 1) return false;
    if (g->target == nType) return true;
    if (!Flush()) return false;
    g->target = nType;
    return true;
}

bool CGraphic::PrintScreen()
{
    // Reads back the finished scene and writes snapshot/YYMMDDhhmmssff.bmp, like the original
    // (which wrote host0:snapshot/... from the locked back buffer).
    if (!Flush()) return false;
    CGraphicTexture &scene = *g->tex[0];
    uint32_t size = scene.nWidth * scene.nHeight * 4;
    SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, size, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_pDevice, &ti);
    if (!tb) return FAIL("SDL_CreateGPUTransferBuffer (PrintScreen)");

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_pDevice);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = scene.pTex; src.w = scene.nWidth; src.h = scene.nHeight; src.d = 1;
    SDL_GPUTextureTransferInfo dst = {tb, 0, scene.nWidth, scene.nHeight};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) { SDL_ReleaseGPUTransferBuffer(m_pDevice, tb); return FAIL("SDL_SubmitGPUCommandBufferAndAcquireFence"); }
    SDL_WaitForGPUFences(m_pDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(m_pDevice, fence);

    bool ok = false;
    if (void *pixels = SDL_MapGPUTransferBuffer(m_pDevice, tb, false)) {
        // Opaque copy: the scene's alpha channel is not part of the picture.
        SDL_Surface *s = SDL_CreateSurfaceFrom((int)scene.nWidth, (int)scene.nHeight, SDL_PIXELFORMAT_BGRX32,
                                               pixels, (int)scene.nWidth * 4);
        if (s) {
            time_t now = time(nullptr);
            struct tm lt;
            localtime_r(&now, &lt);
            SDL_CreateDirectory("snapshot");
            char path[64];
            SDL_snprintf(path, sizeof(path), "snapshot/%02d%02d%02d%02d%02d%02d%02d.bmp", lt.tm_year % 100,
                         lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
                         (int)(SDL_GetTicks() % 1000 * 60 / 1000));
            ok = SDL_SaveBMP(s, path);
            if (ok) SDL_Log("CGraphic::PrintScreen: wrote %s", path);
            SDL_DestroySurface(s);
        }
        SDL_UnmapGPUTransferBuffer(m_pDevice, tb);
    }
    SDL_ReleaseGPUTransferBuffer(m_pDevice, tb);
    return ok || FAIL("PrintScreen");
}

// ---------------------------------------------------------------------------------------------
// 3D models

// ---- mesh arena: first-fit allocation of vertex ranges in one GPU buffer

static void ArenaRelease(uint32_t offset, uint32_t count)
{
    auto &fl = g->arenaFree;
    auto next = fl.lower_bound(offset);
    if (next != fl.begin()) {                       // merge with the previous free range
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) { offset = prev->first; count += prev->second; fl.erase(prev); }
    }
    if (next != fl.end() && offset + count == next->first) { count += next->second; fl.erase(next); }
    fl[offset] = count;
}

static bool ArenaGrow(uint32_t need)
{
    uint32_t cap = SDL_max(g->arenaCap ? g->arenaCap * 2 : 1u << 18, g->arenaCap + need);
    SDL_GPUBufferCreateInfo bi = {SDL_GPU_BUFFERUSAGE_VERTEX, (Uint32)(cap * sizeof(MeshVertex)), 0};
    SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(CGraphic::GetDevice(), &bi);
    if (!buf) return false;
    Flush();                                        // recorded draws use the old buffer
    if (g->pMeshArena) SDL_ReleaseGPUBuffer(CGraphic::GetDevice(), g->pMeshArena);
    g->pMeshArena = buf;
    ArenaRelease(g->arenaCap, cap - g->arenaCap);
    g->arenaCap = cap;
    for (CGraphicMesh *m : g->meshes)               // the new buffer starts empty
        if (!m->bDirty) { m->bDirty = true; g->dirtyMeshes.push_back(m); }
    return true;
}

static bool ArenaAlloc(uint32_t count, uint32_t &offset)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        for (auto it = g->arenaFree.begin(); it != g->arenaFree.end(); ++it) {
            if (it->second < count) continue;
            offset = it->first;
            uint32_t rest = it->second - count;
            g->arenaFree.erase(it);
            if (rest) g->arenaFree[offset + count] = rest;
            return true;
        }
        if (!ArenaGrow(count)) return false;
    }
    return false;
}

CGraphicMesh *CGraphic::CreateMesh(const MeshVertex *pVertices, uint32_t nCount)
{
    if (!nCount) return nullptr;
    auto *m = new CGraphicMesh();
    if (!ArenaAlloc(nCount, m->offset)) { delete m; FAIL("SDL_CreateGPUBuffer (mesh arena)"); return nullptr; }
    m->staging.assign(pVertices, pVertices + nCount);
    g->meshes.push_back(m);
    g->dirtyMeshes.push_back(m);
    g_stats.liveMeshes++; g_stats.meshCreates++;
    return m;
}

void CGraphic::DestroyMesh(CGraphicMesh *pMesh)
{
    if (!pMesh) return;
    g->dirtyMeshes.erase(std::remove(g->dirtyMeshes.begin(), g->dirtyMeshes.end(), pMesh), g->dirtyMeshes.end());
    g->meshes.erase(std::remove(g->meshes.begin(), g->meshes.end(), pMesh), g->meshes.end());
    g->arenaPendingFree.push_back({pMesh->offset, (uint32_t)pMesh->staging.size()});
    delete pMesh;
    g_stats.liveMeshes--; g_stats.meshDestroys++;
}

void CGraphic::DrawMeshStrips(CGraphicMesh *pMesh, MeshShader shader, const uint32_t *prims, const uint32_t *culls,
                              size_t n, const MeshConstants &c)
{
    uint32_t start = 0;
    for (size_t i = 0; i < n; i++) {
        DrawMesh(pMesh, shader, d3d::TRIANGLESTRIP, start, prims[i], culls[i], c);
        start += prims[i] + 2;
    }
}

bool CGraphic::DrawMesh(CGraphicMesh *pMesh, MeshShader shader, PrimitiveType type, uint32_t nFirst,
                        uint32_t nPrimCount, uint32_t nCull, const MeshConstants &c)
{
    if (!pMesh || !nPrimCount) return false;
    static const char *skip = getenv("RECOMP_SKIP_MESH");      // debugging: "all" or a shader number
    if (skip && (!strcmp(skip, "all") || atoi(skip) == (int)shader)) return true;
    uint32_t count = type == TRIANGLESTRIP ? nPrimCount + 2 : nPrimCount * 3;
    if (nFirst + count > pMesh->staging.size()) return false;

    DrawCmd d = {};
    d.pMesh = pMesh;
    d.firstVertex = pMesh->offset + nFirst;
    d.vertexCount = count;
    int vpIndex = g->nViewport >= 0 && g->nViewport < 2 ? g->nViewport : 0;
    d.scissor = g->viewport[vpIndex];
    State s = g->st;
    s.cull = nCull;             // the original set D3DRS_CULLMODE per strip
    d.pipeKey = PipeKey(type, s) | kKeyMesh;
    FillTexture(d);

    MeshVSParams vs = {};
    memcpy(vs.fog, c.fog, sizeof(vs.fog));
    memcpy(vs.lightColor, c.lightColor, sizeof(vs.lightColor));
    memcpy(vs.m, c.localToScreen, sizeof(vs.m));
    memcpy(vs.lightLocal, c.lightLocal, sizeof(vs.lightLocal));
    memcpy(vs.base, c.baseColor, sizeof(vs.base));
    memcpy(vs.stz, c.stzOffset, sizeof(vs.stz));
    vs.kind = shader;
    MeshFSParams fs = {};
    fs.p = d.fs;
    fs.fogOn = s.fogEnable && m_Config.fog ? 1 : 0;
    fs.fogR = ((s.fogColor >> 16) & 0xff) / 255.0f;
    fs.fogG = ((s.fogColor >> 8) & 0xff) / 255.0f;
    fs.fogB = (s.fogColor & 0xff) / 255.0f;
    // Strips of one model share their parameters: reuse the previous block when it is identical
    // (CVertex issues one draw per strip, thousands per frame).
    if (CACHED_ENV("RECOMP_TRACE_MESHREPEAT")) {          // debugging: distinct meshes per frame
        static uint32_t serial;
        static std::map<std::tuple<CGraphicMesh *, uint32_t, uint64_t, SDL_GPUTexture *>, uint32_t> seen;
        if (serial != g_frameSerial) {
            if (serial % 600 == 0 && !seen.empty()) {
                uint32_t draws = 0, maxRep = 0;
                for (auto &e : seen) { draws += e.second; maxRep = SDL_max(maxRep, e.second); }
                std::map<CGraphicMesh *, uint32_t> meshes;
                for (auto &e : seen) meshes[std::get<0>(e.first)] += e.second;
                fprintf(stderr, "mesh draws %u: distinct mesh+range+state %zu, distinct meshes %zu, max repeats %u\n",
                        draws, seen.size(), meshes.size(), maxRep);
            }
            seen.clear();
            serial = g_frameSerial;
        }
        seen[std::make_tuple(pMesh, nFirst, d.pipeKey, d.pTex)]++;
    }
    if (!g->meshParams.empty() && !memcmp(&g->meshParams.back().first, &vs, sizeof(vs)) &&
        !memcmp(&g->meshParams.back().second, &fs, sizeof(fs))) {
        d.meshParams = (uint32_t)g->meshParams.size() - 1;
        g->draws.push_back(d);
        g_stats.meshDraws++;
        return true;
    }
    d.meshParams = (uint32_t)g->meshParams.size();
    if (const char *t = CACHED_ENV("RECOMP_TRACE_MESHTEX")) {      // debugging: "<frame serial>"
        if (g_frameSerial == strtoul(t, nullptr, 10)) {
            int handle = -1;
            for (uint32_t i = 0; i < kMaxTextures; i++) if (g->managedTex[i] == g->pCurTex) { handle = (int)i; break; }
            fprintf(stderr, "mesh draw %u: texture %ux%u, managed handle %d\n", d.meshParams,
                    g->pCurTex ? g->pCurTex->nWidth : 0, g->pCurTex ? g->pCurTex->nHeight : 0, handle);
        }
    }
    g->meshParams.push_back({vs, fs});
    g->draws.push_back(d);
    g_stats.meshDraws++; g_stats.meshParamBlocks++;
    return true;
}

int CGraphic::DebugCompareManagedTexture(uint32_t nHandle, const char *prefix)
{
    if (nHandle >= kMaxTextures || !g->managedTex[nHandle]) return -1;
    CGraphicTexture *mt = g->managedTex[nHandle];
    if (mt->gpuW != mt->nWidth || mt->gpuH != mt->nHeight) return -1;   // the GPU holds a replacement
    std::lock_guard<std::recursive_mutex> lock(g->gpuLock);
    Flush();
    CGraphicTexture &t = *g->managedTex[nHandle];
    uint32_t bytes = t.nWidth * t.nHeight * BytesPerTexel(t.nFormat);
    SDL_GPUTransferBufferCreateInfo ti = {SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_pDevice, &ti);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_pDevice);
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = t.pTex; src.w = t.nWidth; src.h = t.nHeight; src.d = 1;
    SDL_GPUTextureTransferInfo dst = {tb, 0, t.nWidth, t.nHeight};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(m_pDevice, true, &fence, 1);
    SDL_ReleaseGPUFence(m_pDevice, fence);
    const uint8_t *gpu = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(m_pDevice, tb, false));
    int diff = 0;
    for (uint32_t i = 0; i + 3 < bytes; i += 4) diff += memcmp(gpu + i, t.staging.data() + i, 4) != 0;
    if (t.nFormat == TEXFMT_BGRA8) {
        char path[256];
        SDL_snprintf(path, sizeof(path), "%s_gpu.bmp", prefix);
        if (SDL_Surface *sf = SDL_CreateSurfaceFrom((int)t.nWidth, (int)t.nHeight, SDL_PIXELFORMAT_BGRA32, (void *)gpu, (int)t.nWidth * 4)) {
            SDL_SaveBMP(sf, path); SDL_DestroySurface(sf);
        }
        SDL_snprintf(path, sizeof(path), "%s_cpu.bmp", prefix);
        if (SDL_Surface *sf = SDL_CreateSurfaceFrom((int)t.nWidth, (int)t.nHeight, SDL_PIXELFORMAT_BGRA32, t.staging.data(), (int)t.nWidth * 4)) {
            SDL_SaveBMP(sf, path); SDL_DestroySurface(sf);
        }
    }
    SDL_UnmapGPUTransferBuffer(m_pDevice, tb);
    SDL_ReleaseGPUTransferBuffer(m_pDevice, tb);
    return diff;
}
