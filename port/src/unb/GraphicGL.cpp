// GraphicGL.cpp - CGraphic on OpenGL ES 3.0 (and desktop OpenGL 3.3 core for testing)
//
// Second backend next to Graphic.cpp (SDL3 GPU). Used where SDL GPU is not available, e.g. on
// RK3326 handhelds (Mali-G31: OpenGL ES 3.2 with the ARM driver, 3.1 with Panfrost, no Vulkan).
// Select it at build time (RENDERER=gl in recomp/Makefile).
//
// The state handling, vertex recording and CPU-side texture data follow Graphic.cpp; only the
// GPU side differs:
// - No GL headers: the ~70 entry points used are loaded through SDL_GL_GetProcAddress, so the
//   file builds anywhere and runs on GLES 3.0 or desktop GL 3.3 (shader version chosen at run time).
// - A GL context belongs to one thread. The game thread only records (draws, CPU-side texture,
//   mesh and palette data). With "threaded" (default) the GL work runs on a render thread (Job,
//   Capture/Execute), and three contexts of one share group split it up:
//     render thread   renderCtx, no surface: uploads and the scene, into an FBO;
//     present thread  ctx, the window's surface: blits finished scenes and swaps (PresentMain).
//                     On KMS the swap blocks until the GPU is done and the next vblank; here it
//                     does not hold up the next frame;
//     warm-up thread  warmCtx: compiles the shader variants of kWarmKeys missing from the program
//                     cache (shaders.bin) while the game starts (WarmMain).
//   Without a surfaceless context (or threaded = 0) the render thread presents itself.
//   The render thread also uploads textures as soon as the game unlocks them, in the time it waits
//   for the next job (UploadReq): a scenery change brings over a hundred textures that the next
//   frame draws with, and the game's own loading meanwhile leaves the render thread idle.
// - Draws are merged and sorted (see Execute); batched model draws keep their constants in
//   per-draw slots of a uniform buffer, models are drawn indexed (BuildIndexed), and fragment
//   shaders are specialised per state (ProgramKey) instead of branching per draw.
// - GL has no depth clamp (used for the sky at the far plane): mesh vertices are clamped to the
//   depth range in the vertex shader instead.
// - Render targets are FBOs. GL's texture origin is the bottom row, so render targets used as
//   textures (the screen effect samples the previous frame) are sampled with v flipped, and
//   PrintScreen flips rows. Ordinary textures are uploaded row 0 first, which matches D3D's v.
// - BGRA texels are uploaded as RGBA with a texture swizzle (GLES has no BGRA upload).

#include "Graphic.h"
#include "Texture.h"            // ps2:: decoders, shared with CTexture

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <tuple>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace d3d;

// ---------------------------------------------------------------------------------------------
// Minimal GL binding

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef unsigned int GLbitfield;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef struct __GLsync *GLsync;
typedef uint64_t GLuint64;

enum : GLenum {
    GL_NO_ERROR = 0, GL_ZERO = 0, GL_ONE = 1, GL_POINTS = 0, GL_LINES = 1, GL_LINE_STRIP = 3, GL_TRIANGLES = 4,
    GL_TRIANGLE_STRIP = 5, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR = 0x0301, GL_SRC_ALPHA = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_DST_ALPHA = 0x0304, GL_ONE_MINUS_DST_ALPHA = 0x0305, GL_DST_COLOR = 0x0306,
    GL_ONE_MINUS_DST_COLOR = 0x0307, GL_SRC_ALPHA_SATURATE = 0x0308, GL_NEVER = 0x0200, GL_LESS = 0x0201,
    GL_EQUAL = 0x0202, GL_LEQUAL = 0x0203, GL_GREATER = 0x0204, GL_NOTEQUAL = 0x0205, GL_GEQUAL = 0x0206,
    GL_ALWAYS = 0x0207, GL_FRONT = 0x0404, GL_BACK = 0x0405, GL_CW = 0x0900, GL_CCW = 0x0901,
    GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_BLEND = 0x0BE2, GL_SCISSOR_TEST = 0x0C11,
    GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05, GL_TEXTURE_2D = 0x0DE1, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402,
    GL_FLOAT = 0x1406, GL_RED = 0x1903, GL_RGBA = 0x1908, GL_NEAREST = 0x2600, GL_LINEAR = 0x2601,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803, GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_MIRRORED_REPEAT = 0x8370,
    GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008, GL_FUNC_SUBTRACT = 0x800A,
    GL_FUNC_REVERSE_SUBTRACT = 0x800B, GL_TEXTURE0 = 0x84C0, GL_ARRAY_BUFFER = 0x8892,
    GL_STREAM_DRAW = 0x88E0, GL_STATIC_DRAW = 0x88E4, GL_DYNAMIC_DRAW = 0x88E8, GL_FRAGMENT_SHADER = 0x8B30,
    GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82, GL_INFO_LOG_LENGTH = 0x8B84,
    GL_FRAMEBUFFER = 0x8D40, GL_RENDERBUFFER = 0x8D41, GL_COLOR_ATTACHMENT0 = 0x8CE0, GL_DEPTH_ATTACHMENT = 0x8D00,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5, GL_DEPTH_COMPONENT24 = 0x81A6, GL_DEPTH_COMPONENT16 = 0x81A5,
    GL_R8 = 0x8229, GL_RGBA8 = 0x8058, GL_TEXTURE_SWIZZLE_R = 0x8E42, GL_TEXTURE_SWIZZLE_B = 0x8E44,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100, GL_VERSION = 0x1F02, GL_RENDERER = 0x1F01,
    GL_TEXTURE_MAX_LEVEL = 0x813D, GL_UNIFORM_BUFFER = 0x8A11, GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 0x8A34,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_UNSIGNED_INT = 0x1405,
    GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST = 0x2701,
    GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_SYNC_FLUSH_COMMANDS_BIT = 0x1, GL_TIMEOUT_EXPIRED = 0x911B,
    GL_WAIT_FAILED = 0x911D, GL_TEXTURE_2D_ARRAY = 0x8C1A, GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88FF,
};
constexpr GLuint64 GL_TIMEOUT_IGNORED = ~0ull;

#define GL_FUNCS(X) \
    X(GLenum, GetError, (void)) \
    X(const unsigned char *, GetString, (GLenum)) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Enable, (GLenum)) \
    X(void, Disable, (GLenum)) \
    X(void, Clear, (GLbitfield)) \
    X(void, Finish, (void)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, ClearDepthf, (GLfloat)) \
    X(void, DepthFunc, (GLenum)) \
    X(void, DepthMask, (GLboolean)) \
    X(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, BlendEquation, (GLenum)) \
    X(void, CullFace, (GLenum)) \
    X(void, FrontFace, (GLenum)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, TexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) \
    X(void, DeleteSamplers, (GLsizei, const GLuint *)) \
    X(void, BindSampler, (GLuint, GLuint)) \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, GenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, DeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, BindRenderbuffer, (GLenum, GLuint)) \
    X(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint *)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteProgram, (GLuint)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, Uniform4iv, (GLint, GLsizei, const GLint *)) \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, DrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    X(void, DrawRangeElements, (GLenum, GLuint, GLuint, GLsizei, GLenum, const void *)) \
    X(void, GenerateMipmap, (GLenum)) \
    X(GLuint, GetUniformBlockIndex, (GLuint, const GLchar *)) \
    X(void, UniformBlockBinding, (GLuint, GLuint, GLuint)) \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
    X(void, GetIntegerv, (GLenum, GLint *)) \
    X(void, Flush, (void)) \
    X(GLsync, FenceSync, (GLenum, GLbitfield)) \
    X(GLenum, ClientWaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, WaitSync, (GLsync, GLbitfield, GLuint64)) \
    X(void, DeleteSync, (GLsync))

namespace {
struct GLApi {
#define X(ret, name, args) ret (*name) args = nullptr;
    GL_FUNCS(X)
#undef X
};
GLApi gl;
void (*glInvalidateFramebufferPtr)(GLenum, GLsizei, const GLenum *);    // GLES 3.0 / GL 4.3; optional
void (*glTexStorage2DPtr)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);     // GLES 3.0 / GL 4.2; optional
void (*glTexStorage3DPtr)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);    // GLES 3.0 / GL 4.2; optional

bool LoadGL()
{
    bool ok = true;
#define X(ret, name, args) \
    gl.name = reinterpret_cast<ret (*) args>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!gl.name) { SDL_Log("GraphicGL: missing gl" #name); ok = false; }
    GL_FUNCS(X)
#undef X
    glInvalidateFramebufferPtr = reinterpret_cast<void (*)(GLenum, GLsizei, const GLenum *)>(
        SDL_GL_GetProcAddress("glInvalidateFramebuffer"));
    glTexStorage2DPtr = reinterpret_cast<void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei)>(
        SDL_GL_GetProcAddress("glTexStorage2D"));
    glTexStorage3DPtr = reinterpret_cast<void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)>(
        SDL_GL_GetProcAddress("glTexStorage3D"));
    return ok;
}
} // namespace

// thread names (profiles, debuggers)
static void NameThread(const char *name)
{
#if defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#elif defined(__APPLE__)
    pthread_setname_np(name);
#else
    (void)name;
#endif
}

// getenv once per call site
#define CACHED_ENV(name) ([]() -> const char * { static const char *v_ = getenv(name); return v_; }())

// ---------------------------------------------------------------------------------------------
// Internal state (as Graphic.cpp)

namespace {

// 2D vertex: the original FVF 0x144 (28 bytes) plus the texture's array layer (set in Execute)
struct Vertex { float x, y, z, rhw; D3DCOLOR color; float u, v; float layer = 0; };
static_assert(sizeof(Vertex) == 32, "FVF 0x144 + layer");

struct FSParams {
    uint32_t mode, colorOp, alphaOp, alphaTest, alphaFunc;
    float    alphaRef;
    uint32_t addrU, addrV, linear, texW, texH, palX, palY;
    uint32_t flipV;             // sampling a render target (GL rows bottom-up)
};

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

struct MeshVSParams { float v[13][4]; int32_t kind; };      // fog, lightColor x3, m x4, lightLocal x3, base, stz
struct MeshFSParams { FSParams p; float fogR, fogG, fogB; uint32_t fogOn; };

// Everything a draw's shaders need, in one std140 uniform block ("Params"). All blocks of a flush
// go into one uniform buffer, so changing parameters between draws is a single BindBufferRange
// (Mali drivers spend tens of microseconds per glUniform* call on the RK3326's cores).
struct ParamBlock {
    float   vs[13][4];          // mesh: fog, lightColor x3, localToScreen x4, lightLocal x3, base, stz
    int32_t f0[4];              // mode, colorOp, alphaOp, alphaFunc
    int32_t f1[4];              // addrU, addrV, linear, flipV
    int32_t f2[4];              // texW, texH, palX, palY
    int32_t f3[4];              // alphaRef (0-255), mesh kind, 0, 0
    float   fog[4];             // fog colour
    float   target[4];          // render target size (2D)
};
static_assert(sizeof(ParamBlock) == 304, "std140 layout");

struct DrawCmd {
    uint64_t         pipeKey;
    uint32_t         firstVertex, vertexCount;
    CGraphicTexture *pTex;      // resolved to a GL texture in Flush (textures upload lazily)
    uint32_t         samplerKey;
    FSParams         fs;
    SDL_Rect         scissor;
    CGraphicMesh    *pMesh;
    uint32_t         meshParams;
    bool             pre;       // batched: firstVertex indexes pMesh->staging, copied to the stream in Flush
    bool             strip;     // batched strips: Impl::stripRuns[stripFirst, +strips), unrolled when copied
    uint32_t         stripFirst, strips;
};

// Batching: per-draw vertex-shader constants, 13 vec4 per draw (vs[13], the shader kind in the
// unused fog.w), in a uniform buffer; a batch binds a window of Impl::batchSlots draws (as many as
// the driver's uniform block size allows, at most 255) and each streamed vertex carries its draw's
// slot within the window.
constexpr uint32_t kSlotBytes = 13 * 16;

constexpr uint64_t kKeyMesh = 1ull << 24;
constexpr uint32_t kPaletteAtlasW = 4096;
constexpr uint32_t kPaletteAtlasH = CGraphic::kNoPalette / 16;

} // namespace

static uint32_t g_frameSerial;
static CGraphic::DebugStats g_stats;
static int g_cfgBatching = 1;      // graphics.ini "batching" (GL backend only)
// ARM Linux builds are for handhelds (Mali): speed-ups on unless graphics.ini says otherwise
#if defined(__linux__) && defined(__aarch64__)
#define HANDHELD_DEFAULT 1
#else
#define HANDHELD_DEFAULT 0
#endif
static int g_cfgFastAlpha = HANDHELD_DEFAULT;  // graphics.ini "fastalpha": skip blending/alpha test that cannot matter
static int g_cfgMipmaps = HANDHELD_DEFAULT;    // graphics.ini "mipmaps": mipmaps for fully opaque textures
static int g_cfgVsync = 1;         // graphics.ini "vsync": 0 = don't wait for the display (may tear)
static int g_cfgFps = 0;           // graphics.ini "fps": frames-per-second counter in the top-left corner
static int g_cfgStandIns = 1;      // graphics.ini "standins": 0 = model textures always wait for their upload
static int g_cfgTex16 = 0;         // graphics.ini "tex16": fully opaque textures as RGB565 (half the upload)
static int g_cfgThreaded = 1;      // graphics.ini "threaded": GL work on its own thread (see Job)
static int g_cfgTexArrays = 0;     // graphics.ini "texarrays": BGRA textures as layers of texture arrays (see TexArray)
static uint64_t g_traceDraws, g_traceMeshVerts, g_traceMeshDraws, g_traceBlended, g_traceAlphaTested;     // RECOMP_TRACE_TICKS
// instruction counts of the GL backend's own CPU work (macOS: rt_thread_instructions; 0 elsewhere)
extern "C" uint64_t rt_thread_instructions(void);
static uint64_t g_insTransform, g_insList, g_insUpload, g_insSubmit;
// RECOMP_TRACE_TICKS: render-thread time per phase (ns, summed over the 600-frame window)
static uint64_t g_nsTex, g_nsList, g_nsUpload, g_nsSubmit, g_nsPresent, g_nsSwapOnly;
// RECOMP_TRACE_TICKS: submission statistics (per 600 frames): items (draw calls) and binds by kind
static uint64_t g_cProg, g_cBlock, g_cState, g_cScissor, g_cTex, g_cSampler, g_cVao, g_cWindow, g_cItems2D, g_cItemsMesh;
// RECOMP_GL_EXP (GPU cost analysis): 1 trivial fragment shading (vertex colour), 2 scissor every
// draw to 1 pixel (vertex and tiling work only), 4 no present blit; leave out 8 2D draws, 16 blended,
// 32 alpha-tested, 64 opaque mesh draws, 128 full-screen 2D quads, 256 the other 2D draws
static int g_exp;
// RECOMP_TRACE_SPIKES: time in texture/mesh create/destroy/upload calls (outermost call only)
uint64_t g_gfxWaitNs;                     // game thread waiting for the render thread
uint64_t g_gfxResNs, g_gfxResCatNs[6];   // categories: create tex, destroy tex, unlock, load, create mesh, destroy mesh
namespace {
struct ResTimer {
    static inline int depth = 0;
    int cat;
    uint64_t t0;
    explicit ResTimer(int c) : cat(c), t0(depth++ ? 0 : SDL_GetTicksNS()) {}
    ~ResTimer() { if (--depth == 0) { uint64_t d = SDL_GetTicksNS() - t0; g_gfxResNs += d; g_gfxResCatNs[cat] += d; } }
};
}

// Texture arrays: BGRA textures (all power-of-two sized) are layers of GL_TEXTURE_2D_ARRAY objects,
// one set of arrays per size class (width, height, mip levels, RGB565). Draws that differ only in
// the texture then merge when the textures share an array: the layer is per draw (batched mesh
// draws: draw slot constants; 2D: a vertex attribute). Creating a texture takes a free layer instead
// of allocating GL storage (~0.9 ms per texture on the R36H's Mali driver). Render thread only.
struct TexArray {
    GLuint id = 0;
    uint64_t cls = 0;                               // ArrayClass
    uint32_t w = 0, h = 0, levels = 1, layers = 0;
    bool is565 = false;
    std::vector<uint16_t> freeLayers;
};

static uint64_t g_texUid;                   // CGraphicTexture::uid
struct CGraphicTexture {
    uint64_t             uid = ++g_texUid;  // unique per texture object (addresses get reused)
    uint32_t             contentGen = 0;    // bumped when the texels change (UnlockTexture)
    GLuint               id = 0;            // GL texture, created on upload (not arrayed)
    GLuint               fbo = 0;           // render targets
    uint32_t             nWidth = 0, nHeight = 0;
    uint32_t             nUVWidth = 0, nUVHeight = 0;
    TexFormat            nFormat = TEXFMT_BGRA8;
    bool                 bRenderTarget = false;
    bool                 bDirty = false;
    std::vector<uint8_t> staging;
    // BGRA textures: largest alpha below 255 (0 if none) and whether any texel is below 255 at all;
    // lets "fastalpha" drop blending / the alpha test where they cannot change a pixel
    uint8_t              maxPartialAlpha = 255;
    bool                 anyTransparent = true;
    bool                 mipmapped = false;
    // slots that refer to it, cleared when it is destroyed (scanning the 16K-entry tables for
    // each of the hundreds of textures freed at a scenery change stalled the game)
    int32_t              texSlot = -1;
    std::vector<uint32_t> managedSlots;
    bool                 pendingUpload = false;     // render thread: in Impl::pendingUploads
    bool                 neededNow = false;         // render thread: used by the job being executed
    bool                 dying = false;             // render thread: being freed
    bool                 is565 = false;             // render thread: GL storage is RGB565 ("tex16")
    uint8_t              levels = 1;                // render thread: mip levels allocated (glTexStorage2D)
    bool                 mipPending = false;        // render thread: in Impl::pendingMips
    bool                 inUse = false;             // render thread: drawn by the job being executed
    int8_t               rtIndex = -1;              // the two scene render targets: 0, 1 (see AcquireTarget)
    bool                 arrayed = false;           // a layer of a TexArray (BGRA, not a render target)
    TexArray            *arr = nullptr;             // render thread: its array once uploaded
    uint16_t             layer = 0;
    bool                 earlyCovered = false;      // game thread: its newest content has an UploadReq
    bool                 need2D = false;            // render thread: drawn in 2D by the job being executed
    bool                 upHeld = false;            // render thread: an UploadReq of it waits (ApplyUploads)
    GLuint               standIn = 0;               // render thread: 1x1 texture drawn until the upload (ApplyUploads)
};

// Batched vertices (Job::List::sverts, CGraphicMesh::unique): normals and UVs as the game's int16
// fixed point (value * 4096, see CVertex::BuildPacket), which the batch shader scales back; exact,
// and 24 bytes instead of MeshVertex's 32 to copy and upload per frame.
struct PreVertex { float x, y, z; int16_t nx, ny, nz, pad; int16_t u, v; };
static_assert(sizeof(PreVertex) == 24, "packed");

static inline int16_t Fixed12(float f)
{
    long i = lroundf(f * 4096.0f);
    return (int16_t)(i < -32768 ? -32768 : i > 32767 ? 32767 : i);
}

static inline PreVertex Pack(const MeshVertex &m)
{
    return PreVertex{m.x, m.y, m.z, Fixed12(m.nx), Fixed12(m.ny), Fixed12(m.nz), 0, Fixed12(m.u), Fixed12(m.v)};
}

struct CGraphicMesh {
    uint32_t                offset = 0;
    uint32_t                index = 0;      // in Impl::meshes
    std::vector<MeshVertex> staging;
    bool                    bDirty = true;
    // render thread (batching): staging without repeated vertices, and staging as indices into it
    // (models are unrolled triangle lists; drawn indexed, each vertex is shaded once)
    bool                    indexed = false;
    std::vector<PreVertex> unique;
    std::vector<uint16_t>   indices;
};

// A texture's new content (UnlockTexture, threaded): queued for the render thread at once, which
// uploads it while it waits for the next job (RenderMain), or else before the first job that draws
// with it (ApplyUploads). In both the job that follows the unlock (Job::uploadReqs) and the
// render thread's queue (Impl::upQueue), hence two references. Done in order per texture.
struct UploadReq {
    CGraphicTexture     *t = nullptr;
    bool                 anyTransparent = true;
    bool                 done = false;              // render thread: uploaded (or dropped: texture freed)
    int                  refs = 2;                  // render thread
    std::vector<uint8_t> data;
};

// A flush's work: everything recorded since the previous flush, the uploads it needs and the
// resources to free after it. Capture() takes it from the recording state (game thread, under
// g->lock); Execute() does the GL work (render thread). Without "threaded" both run back to back
// in Flush(); with it, jobs queue for the render thread, which owns the GL context, so the game
// thread does not wait for the driver or the GPU. Texture and palette data are copied for the
// queue (the game may rewrite them meanwhile); meshes are immutable and freed only after the jobs
// that may use them.
// A draw list entry: a 2D draw, an unbatched mesh draw or a batch of mesh draws
struct ListItem { uint32_t draw, block, first, count, window; uint8_t kind; uint32_t vfirst = 0, vlast = 0; };    // kind: 0 2D, 1 mesh, 2 batch
constexpr uint32_t kAnyBlock = ~1u;                 // the item's shaders read no parameter block

struct Job {
    std::vector<Vertex> verts;
    std::vector<DrawCmd> draws;
    std::vector<std::pair<MeshVSParams, MeshFSParams>> meshParams;
    std::vector<std::pair<uint32_t, uint32_t>> stripRuns;
    CGraphicTexture *target = nullptr;
    bool clearColor = false, clearDepth = false;
    float clearColorV[4] = {}, clearDepthV = 1.0f;
    struct TexUpload { CGraphicTexture *t; bool anyTransparent; std::vector<uint8_t> data; };   // data empty: t->staging
    std::vector<TexUpload> texUploads;
    std::vector<CGraphicMesh *> meshUploads;
    uint32_t newArenaCap = 0;                       // non-batched: reallocate the mesh buffer first
    std::vector<uint32_t> palIds;
    std::vector<uint8_t> palData;                   // 256 RGBA entries per id
    std::vector<CGraphicTexture *> deadTex;
    std::vector<CGraphicMesh *> deadMeshes;
    std::vector<UploadReq *> uploadReqs;           // issued since the previous flush, in order
    bool present = false, snapshot = false;
    CGraphicTexture *scene = nullptr;
    float gamma = 1.0f;
    // the draw list (BuildList); kept with the job so that its buffers keep their capacity
    struct List {
        std::vector<ListItem> items;
        std::vector<uint8_t> blockData;             // parameter blocks
        std::vector<PreVertex> sverts;              // batched vertices
        std::vector<uint8_t> sslots;                // their draw slot within the batch window
        std::vector<uint32_t> sindices;             // batched triangles (indices into sverts)
        std::vector<float> drawData;                // kSlotBytes per batched draw; only grows
        size_t drawUsed = 0;                        // floats of drawData in use
        uint64_t ns = 0, ins = 0, meshDraws = 0, meshVerts = 0;    // RECOMP_TRACE_TICKS
        bool built = false;
    } list;
    int listState = 0;                              // with the list thread: 0 queued, 1 being built, 2 built (queueLock)
};

// What presenting needs; per context (vertex arrays are not shared between contexts), so the
// present thread has its own.
struct PresentRes {
    GLuint prog = 0, vao = 0, fpsTex = 0, samplerLinear = 0, samplerNearest = 0;
    GLuint overlayTex = 0;                          // CGraphic::SetOverlay's image, at overlayVersion
    uint32_t overlayVersion = 0;
    GLint uPresent = -1, uScaleY = -1, uLutOn = -1, uClamp = -1;
    GLuint lut = 0;                                 // gamma table (256x1 R8), for lutGamma
    float lutGamma = 0;
};

// Recursive lock for the recording state. Taken thousands of times per frame (every model and
// 2D draw) and almost never contended (guest threads run one at a time), and the packet thread
// holds it across a frame's DMA chain (CGraphic::HoldRecordingLock): re-entry by the owner is a
// plain counter, std::recursive_mutex's lock/unlock cost ~9% of that thread on the R36H.
struct RecLock {
    std::mutex m;
    std::atomic<const void *> owner{nullptr};
    uint32_t depth = 0;
    static const void *Self() { static thread_local char me; return &me; }
    void lock()
    {
        const void *self = Self();
        if (owner.load(std::memory_order_relaxed) == self) { depth++; return; }
        m.lock();
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }
    void unlock()
    {
        if (--depth == 0) {
            owner.store(nullptr, std::memory_order_relaxed);
            m.unlock();
        }
    }
};

struct CGraphic::Impl {
    SDL_Window *pWindow = nullptr;
    SDL_GLContext ctx = nullptr;
    SDL_ThreadID renderThread = 0;
    bool glReady = false, glFailed = false, gles = true;

    std::string shaderHead;                         // #version and precision lines
    std::unordered_map<uint32_t, GLuint> programs;  // variant key (ProgramKey) -> program
    PresentRes inlinePresent;                       // presenting on the render thread (no asyncPresent)
    GLuint ubo = 0;
    uint32_t uboStride = 0;                         // sizeof(ParamBlock) rounded to the offset alignment
    std::unordered_map<uint32_t, GLuint> samplers;
    GLuint depthRB = 0, vao2D = 0, vaoMesh = 0, vb2D = 0, arenaVB = 0;
    // batching: runs of order-independent mesh draws are sorted by state and merged; their vertices
    // are copied into one stream per flush (one draw call per state instead of one per model)
    bool batching = true;
    GLuint vaoPre = 0, preVB = 0, slotVB = 0, uboDraws = 0;
    uint32_t batchSlots = 64, uboAlign = 16;
    GLuint indexVB = 0;
    uint64_t dedupVerts = 0, dedupUnique = 0;       // RECOMP_TRACE_TICKS: meshes indexed so far
    std::vector<CGraphicMesh *> deadMeshes;         // destroyed, freed after the next flush
    std::vector<std::pair<uint32_t, uint32_t>> stripRuns;  // batched strips: first vertex, count
    // the last batched strip draw's inputs, so that the model's following strips (same constants
    // and state; CVertex::Run draws a model strip by strip) are appended to it cheaply
    struct {
        CGraphicMesh *mesh = nullptr;
        uint32_t end = 0, cull = 0, palette = 0;
        int shader = -1, viewport = 0;
        bool textured = false;
        CGraphicTexture *tex = nullptr;
        State st;
        MeshConstants c;
    } lastStrip;
    uint32_t vbCapacity = 0, arenaCapSent = 0;     // arenaCapSent: mesh buffer size requested from GL
    GLuint paletteTex = 0;

    std::vector<uint8_t> paletteCPU;
    std::vector<uint32_t> dirtyPalettes;
    std::vector<bool> paletteDirty;
    std::vector<uint32_t> freePalettes;

    std::vector<CGraphicTexture *> tex;
    std::vector<uint32_t> texPalette;
    std::vector<CGraphicTexture *> managedTex;
    std::vector<uint32_t> managedPalette;
    std::vector<CGraphicTexture *> dirtyTex;
    std::vector<CGraphicTexture *> deadTex;         // deleted after the next flush
    std::vector<CGraphicMesh *> meshes, dirtyMeshes;
    uint32_t arenaCap = 0;
    std::map<uint32_t, uint32_t> arenaFree;
    std::vector<std::pair<uint32_t, uint32_t>> arenaPendingFree;
    std::vector<std::pair<MeshVSParams, MeshFSParams>> meshParams;
    TEXSIZE texSize = {16256, 256, 256};
    uint32_t maxTexW = 1024, maxTexH = 1024;
    CGraphicTexture *white = nullptr;

    std::vector<Vertex> verts;
    std::vector<DrawCmd> draws;
    bool pendingClearColor = false, pendingClearDepth = false;
    float clearColor[4] = {0, 0, 0, 1};
    float clearDepth = 1.0f;

    State st;
    bool textured = false;
    CGraphicTexture *pCurTex = nullptr;
    uint32_t curPalette = CGraphic::kNoPalette;
    int target = 0;
    TLVERTEX_TEX texVertex[4] = {};
    TLVERTEX vertex2D[4] = {};
    SDL_Rect viewport[2] = {};
    int nViewport = 0;
    SDL_Rect border[4] = {};
    float gamma = 1.0f;
    RecLock lock;                                   // recording state (game threads); see RecLock

    // "threaded": the render thread and its job queue (see Job)
    bool threaded = false;
    std::thread thread;
    bool listThreaded = false;                      // the list thread builds draw lists ahead (ListMain)
    std::thread listThread;
    std::mutex queueLock;                           // queue, freeJobs, setup and quit flags
    std::condition_variable queueCv;
    std::deque<Job *> queue;
    std::vector<Job *> freeJobs;
    bool setupDone = false, setupOk = false, quit = false, snapshotRequest = false;
    std::atomic<uint32_t> presentsQueued{0}, presentsDone{0};
    // when the frame in flight was handed over, and how long frames take from hand-over to
    // presented (moving average): the frame limiter starts the next frame a little before the
    // render thread is done instead of only after it (FrameBusyNs)
    // (per frame: hand-over time by present number; a frame starts when it was handed over or the
    // previous one was presented, whichever is later)
    std::atomic<uint64_t> handoverNs[4] = {}, lastPresentNs{0}, avgFrameNs{0};
    uint64_t flushNs = 0;                           // render thread: GL time of the frame so far
    // render thread: uploads not needed by the jobs so far (threaded only; see Execute)
    std::vector<Job::TexUpload> pendingUploads;
    std::vector<CGraphicTexture *> pendingMips;    // uploaded, mipmaps still to generate
    std::vector<GLuint> deleteIds;
    // freed GL textures kept for reuse, by size/format/levels (PoolKey): creating a texture costs
    // ~0.9 ms on the R36H's Mali driver, and a scenery change frees hundreds of textures of the
    // sizes it then creates. Capped by memory.
    std::unordered_map<uint64_t, std::vector<GLuint>> texPool;
    uint64_t texPoolBytes = 0;
    bool texArrays = false;                         // graphics.ini "texarrays"
    std::unordered_map<uint64_t, std::vector<TexArray *>> arrays;    // by ArrayClass; render thread
    uint32_t maxArrayLayers = 256, arrayCount = 0;
    uint64_t arrayBytes = 0;
    std::vector<uint8_t> mipScratch;

    // "Asynchronous present" (threaded, where a context can be current without a window surface):
    // the render thread draws with renderCtx and hands finished scenes to the present thread, which
    // owns the window surface (ctx) and is the one blocked in the swap (KMS waits for the GPU and
    // the next vblank there). The render thread meanwhile records the next frame. See PresentMain.
    SDL_GLContext renderCtx = nullptr;
    bool asyncPresent = false;
    std::thread presentThread;
    std::mutex presentLock;                         // presentReq, presentBusy, rtReleased, presentQuit
    std::condition_variable presentCv;
    struct PresentReq { GLuint tex = 0; uint32_t w = 0, h = 0; int rt = -1; float gamma = 1; GLsync ready = nullptr; bool valid = false; };
    PresentReq presentReq;                          // mailbox: the newest scene not taken yet
    int presentBusy = -1;                           // render target the present thread is blitting from
    GLsync rtReleased[2] = {};                      // after the last blit from render target 0 / 1
    bool presentQuit = false, presentSetupDone = false, presentSetupOk = false;
    GLsync frameFence = nullptr;                    // render thread: end of the previous frame (GPU backpressure)
    std::atomic<uint64_t> presentsShown{0}, presentsDropped{0}, presentSwapNs{0}, frameWaitNs{0}, presentWaitNs{0}, presentBlitNs{0};
    std::atomic<uint64_t> paceWaitNs{0};
    // render thread: per frame, the list thread's time and the wait for the GPU to finish the
    // previous frame (HandOff); frameLoadNs = the slowest of render thread + GPU wait (about
    // max(render thread, GPU)) and list thread for the last frame (FrameLoadNs)
    uint64_t listFrameNs = 0, gpuWaitFrameNs = 0;
    std::atomic<uint64_t> frameLoadNs{0};
    std::atomic<uint64_t> presentGapMaxNs{0}, presentHitches{0};   // longest time between two shown frames, gaps over 50 ms

    // shader variants compiled by either thread (render thread, warm-up thread), the warm-up
    // thread's context, and the variants being compiled
    SDL_GLContext warmCtx = nullptr;
    std::thread warmThread;
    std::mutex progLock;                            // sharedPrograms, compiling, the program cache file
    std::condition_variable progCv;
    std::unordered_map<uint32_t, GLuint> sharedPrograms;
    std::unordered_set<uint32_t> compiling;

    // early texture uploads (see UploadReq)
    bool earlyUploads = false;                      // threaded, not texArrays; RECOMP_GL_EARLY_UPLOADS=0 disables
    std::deque<UploadReq *> upQueue;                // under queueLock
    std::vector<UploadReq *> uploadReqs;            // game thread: issued since the last Capture
    std::vector<UploadReq *> heldReqs;              // render thread: not needed by the jobs so far
    std::vector<GLuint> standInPool;                // render thread: free 1x1 stand-in textures
    uint32_t standInsMade = 0;                      // render thread: made ahead in idle time, up to kStandIns
    uint64_t upFrameNs = 0;                         // render thread: ApplyUploads time in this frame
    uint64_t presentIntervalNs = 16666667;          // render thread: time between presents (moving average)
    uint64_t upBudgetNs = 3000000;                  // RECOMP_GL_UPLOAD_BUDGET (ms): see ApplyUploads
    uint64_t upIdleNs = 0, upIdleCount = 0, upJobNs = 0, upJobCount = 0;   // render thread, RECOMP_TRACE_TICKS
};

static CGraphic::Impl *g = nullptr;

GRAPHIC_CONFIG     CGraphic::m_Config;
SDL_GPUDevice     *CGraphic::m_pDevice = nullptr;
TexFormat          CGraphic::m_nTexFormat = TEXFMT_P8;
PrimitiveType      CGraphic::m_nPrimType = TRIANGLESTRIP;
char               CGraphic::m_szLastError[256] = "";

static bool Fail(char *buf, const char *what)
{
    SDL_snprintf(buf, 256, "%s: %s", what, SDL_GetError());
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "CGraphic: %s", buf);
    return false;
}
#define FAIL(what) Fail(CGraphic::m_szLastError, what)

// ---------------------------------------------------------------------------------------------
// Shaders (GLSL ES 3.00 / GLSL 3.30, chosen at run time). Same logic as GraphicShaders.h.

// Program variants (ProgramKey) are compiled per combination of MESH, FLAT, TEXMODE (0 none,
// 1 RGBA, 2 paletted), FOG, the batched mesh path, and for the fragment shader the texture stage's
// COLOROP and ALPHAOP, the alpha test's ATEST function (0: no test) and FLIPV: the fragment shader
// has no per-draw branches (Mali-G31 spent about half of its fragment time on the generic one).
// Fragment colour math is mediump (twice the rate on Mali's fp16 paths); texture coordinates stay
// highp (tiled textures repeat many times across a polygon). Only the mesh kind remains a
// (vertex shader) uniform branch.
static const char kParams[] = R"GLSL(
layout(std140) uniform Params {
    highp vec4 pVS[13];
    highp ivec4 pF0;      // mode, colorOp, alphaOp, alphaFunc
    highp ivec4 pF1;      // addrU, addrV, linear, flipV
    highp ivec4 pF2;      // texW, texH, palX, palY
    highp ivec4 pF3;      // alphaRef (0-255), mesh kind
    highp vec4 pFog;
    highp vec4 pTarget;
};
)GLSL";

static const char kCommonFS[] = R"GLSL(
uniform sampler2D uTex;
uniform sampler2D uPal;
flat in highp float vLayer;
out vec4 fragColor;
#if BATCHED
flat in mediump vec4 vFogRef;       // batched draws: fog colour and alpha reference from the draw slot
#define ALPHAREF floor(vFogRef.a * 255.0 + 0.5)
#define FOGCOLOR vFogRef.rgb
#else
#define ALPHAREF float(pF3.x)
#define FOGCOLOR pFog.rgb
#endif

#if TEXMODE == 2
// GLSL ES leaves % undefined for negative operands (llvmpipe and Metal do what C does, Mali need
// not), and tiled textures have negative texel coordinates: use floor division instead.
int floorMod(int c, int n) { return clamp(c - n * int(floor(float(c) / float(n))), 0, n - 1); }

int wrapCoord(int c, int size, int mode)
{
    if (mode == 3) return clamp(c, 0, size - 1);
    if (mode == 2) {
        int period = size * 2;
        int m = floorMod(c, period);
        return m < size ? m : period - 1 - m;
    }
    return floorMod(c, size);
}

vec4 paletteTexel(ivec2 c)
{
    int x = wrapCoord(c.x, pF2.x, pF1.x);
    int y = wrapCoord(c.y, pF2.y, pF1.y);
    int i = int(texelFetch(uTex, ivec2(x, y), 0).r * 255.0 + 0.5);
    return texelFetch(uPal, ivec2(pF2.z + i, pF2.w), 0);
}

vec4 sampleTexture(vec2 uv)
{
    vec2 t = uv * vec2(pF2.xy);
    if (pF1.z == 0) return paletteTexel(ivec2(floor(t)));
    t -= 0.5;
    ivec2 c = ivec2(floor(t));
    vec2 f = fract(t);
    vec4 a = paletteTexel(c), b = paletteTexel(c + ivec2(1, 0));
    vec4 d = paletteTexel(c + ivec2(0, 1)), e = paletteTexel(c + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}
#elif TEXMODE == 1
vec4 sampleTexture(highp vec2 uv) { return texture(uTex, uv); }
#elif TEXMODE == 3
uniform highp sampler2DArray uTexA;
vec4 sampleTexture(highp vec2 uv) { return texture(uTexA, vec3(uv, vLayer)); }
#endif

// op: COLOROP / ALPHAOP, constants of the variant (the compiler drops the other cases)
vec4 combine(int op, vec4 tex, vec4 diffuse)
{
    if (op == 4) return tex * diffuse;
    if (op == 5) return clamp(tex * diffuse * 2.0, 0.0, 1.0);
    if (op == 2) return tex;
    return diffuse;
}

bool alphaPasses(int func, float a, float ref)
{
    if (func == 1) return false;
    if (func == 2) return a < ref;
    if (func == 3) return a == ref;
    if (func == 4) return a <= ref;
    if (func == 5) return a > ref;
    if (func == 6) return a != ref;
    if (func == 7) return a >= ref;
    return true;
}

vec4 shade(vec4 color, highp vec2 uv)
{
    vec4 c = color;
#ifdef EXP_TRIVIAL
    return c;
#endif
#if TEXMODE != 0
#if FLIPV
    uv.y = 1.0 - uv.y;
#endif
    vec4 t = sampleTexture(uv);
#if COLOROP == ALPHAOP
    c = combine(COLOROP, t, color);
#else
    c = vec4(combine(COLOROP, t, color).rgb, combine(ALPHAOP, t, color).a);
#endif
#endif
#if ATEST
    // both sides as whole 0-255 values: an alpha equal to the reference must compare equal
    if (!alphaPasses(ATEST, floor(c.a * 255.0 + 0.5), ALPHAREF)) discard;
#endif
    return c;
}
)GLSL";

static const char kVS2D[] = R"GLSL(
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aLayer;
FLAT out vec4 vColor;
out vec2 vUV;
flat out highp float vLayer;
void main()
{
    vLayer = aLayer;
    float w = aPos.w != 0.0 ? 1.0 / aPos.w : 1.0;
    float x = (aPos.x + 0.5) / pTarget.x * 2.0 - 1.0;
    float y = 1.0 - (aPos.y + 0.5) / pTarget.y * 2.0;
    gl_Position = vec4(x * w, y * w, (aPos.z * 2.0 - 1.0) * w, w);
    gl_PointSize = 1.0;
    vColor = aColor.bgra;
    vUV = aUV;
}
)GLSL";

static const char kFS2D[] = R"GLSL(
FLAT in vec4 vColor;
in highp vec2 vUV;
void main() { fragColor = shade(vColor, vUV); }
)GLSL";

static const char kVSMesh[] = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
FLAT out vec4 vColor;
out vec2 vUV;
out float vFog;
flat out highp float vLayer;
void main()
{
    vLayer = pVS[8].w;
    int kind = pF3.y;
    vec4 p = aPos.x * pVS[4] + aPos.y * pVS[5] + aPos.z * pVS[6] + pVS[7];
    if (kind == 2 || kind == 3) p.z += pVS[12].z;
    vec4 d = vec4(max(vec3(dot(aNormal, pVS[8].xyz), dot(aNormal, pVS[9].xyz), dot(aNormal, pVS[10].xyz)), 0.0), 1.0);
    vec3 lit = vec3(dot(d, pVS[1]), dot(d, pVS[2]), dot(d, pVS[3]));
    bool lighting = kind == 0 || kind == 2;
    vColor = clamp(lighting ? vec4(lit, 1.0) * pVS[11] : pVS[11], 0.0, 1.0);
    if (kind == 2) vUV = aUV + pVS[12].xy;
    else if (kind == 3) vUV = min(vec2(pVS[12].x, dot(d, pVS[2])), 1.0);
    else vUV = aUV;
    vFog = kind == 2 ? 1.0 : clamp((pVS[0].y - p.w) * pVS[0].z, 0.0, 1.0);
    // no depth clamp in GL(ES): keep D3D depth within [0, w], then map it to GL's [-w, w]
    if (p.w > 0.0) p.z = clamp(p.z, 0.0, p.w);
    gl_Position = vec4(p.x, p.y, 2.0 * p.z - p.w, p.w);
}
)GLSL";

static const char kVSMeshBatch[] = R"GLSL(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aSlot;
layout(std140) uniform Draws { vec4 dV[DRAW_SLOTS * 13]; };
const float kFixed = 1.0 / 4096.0;  // normals and UVs arrive as int16 fixed point (PreVertex)
FLAT out vec4 vColor;
out vec2 vUV;
out float vFog;
flat out highp float vLayer;
flat out mediump vec4 vFogRef;      // fog colour, alpha reference (0-1)
void main()
{
    int b = int(aSlot) * 13;
    vLayer = dV[b + 8].w;
    uint fog = uint(dV[b + 9].w);
    vFogRef = vec4(float(fog & 255u), float((fog >> 8) & 255u), float(fog >> 16), dV[b + 10].w) / 255.0;
    int kind = int(dV[b].w);
    vec4 p = aPos.x * dV[b + 4] + aPos.y * dV[b + 5] + aPos.z * dV[b + 6] + dV[b + 7];
    if (kind == 2 || kind == 3) p.z += dV[b + 12].z;
    vec3 n = aNormal * kFixed;
    vec2 uv = aUV * kFixed;
    vec4 d = vec4(max(vec3(dot(n, dV[b + 8].xyz), dot(n, dV[b + 9].xyz), dot(n, dV[b + 10].xyz)), 0.0), 1.0);
    vec3 lit = vec3(dot(d, dV[b + 1]), dot(d, dV[b + 2]), dot(d, dV[b + 3]));
    bool lighting = kind == 0 || kind == 2;
    vColor = clamp(lighting ? vec4(lit, 1.0) * dV[b + 11] : dV[b + 11], 0.0, 1.0);
    if (kind == 2) vUV = uv + dV[b + 12].xy;
    else if (kind == 3) vUV = min(vec2(dV[b + 12].x, dot(d, dV[b + 2])), 1.0);
    else vUV = uv;
    vFog = kind == 2 ? 1.0 : clamp((dV[b].y - p.w) * dV[b].z, 0.0, 1.0);
    if (p.w > 0.0) p.z = clamp(p.z, 0.0, p.w);
    gl_Position = vec4(p.x, p.y, 2.0 * p.z - p.w, p.w);
}
)GLSL";

static const char kFSMesh[] = R"GLSL(
FLAT in vec4 vColor;
in highp vec2 vUV;
in float vFog;
void main()
{
    vec4 c = shade(vColor, vUV);
#if FOG
    c.rgb = mix(FOGCOLOR, c.rgb, clamp(vFog, 0.0, 1.0));
#endif
    fragColor = c;
}
)GLSL";

static const char kVSPresent[] = R"GLSL(
out vec2 vUV;
void main()
{
    vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    vUV = uv;
}
)GLSL";

static const char kFSPresent[] = R"GLSL(
in vec2 vUV;
uniform sampler2D uScene;
uniform sampler2D uLut;     // gamma table: 256 texels, texel i = (i/255)^invGamma
uniform vec4 uPresent;      // invGamma (unused), uvOffsetX, uvOffsetY (from the top), uvScaleX
uniform float uScaleY;
uniform float uLutOn;       // 0: no gamma (identity)
uniform vec4 uClamp;        // sampling bounds (from the top): half a texel inside the shown part
out vec4 fragColor;
void main()
{
    vec2 t = clamp(vec2(uPresent.y + vUV.x * uPresent.w, uPresent.z + vUV.y * uScaleY), uClamp.xy, uClamp.zw);
    vec2 uv = vec2(t.x, 1.0 - t.y);
    vec4 c = texture(uScene, uv);
    // a table lookup per channel instead of pow(): on the R36H's Mali-G31 the final pass took ~8%
    // of the GPU's time at 60 frames/s, the per-pixel pow ~3 points of it
    if (uLutOn != 0.0) {
        vec3 t = c.rgb * (255.0 / 256.0) + 0.5 / 256.0;
        c.rgb = vec3(texture(uLut, vec2(t.r, 0.5)).r, texture(uLut, vec2(t.g, 0.5)).r, texture(uLut, vec2(t.b, 0.5)).r);
    }
    fragColor = vec4(c.rgb, 1.0);
}
)GLSL";

static GLuint Compile(GLenum type, const std::string &src)
{
    GLuint s = gl.CreateShader(type);
    const char *p = src.c_str();
    gl.ShaderSource(s, 1, &p, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        gl.GetShaderInfoLog(s, sizeof(log), nullptr, log);
        SDL_Log("GraphicGL: shader compile failed:\n%s", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint Link(const std::string &vs, const std::string &fs)
{
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, v);
    gl.AttachShader(p, f);
    gl.LinkProgram(p);
    gl.DeleteShader(v);
    gl.DeleteShader(f);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        gl.GetProgramInfoLog(p, sizeof(log), nullptr, log);
        SDL_Log("GraphicGL: program link failed:\n%s", log);
        gl.DeleteProgram(p);
        return 0;
    }
    return p;
}

// ---------------------------------------------------------------------------------------------
// GL objects

static GLenum ToBlend(uint32_t b)
{
    switch (b) {
    case BLEND_ZERO:         return GL_ZERO;
    case BLEND_SRCCOLOR:     return GL_SRC_COLOR;
    case BLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case BLEND_SRCALPHA:     return GL_SRC_ALPHA;
    case BLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case BLEND_DESTALPHA:    return GL_DST_ALPHA;
    case BLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case BLEND_DESTCOLOR:    return GL_DST_COLOR;
    case BLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case BLEND_SRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
    default:                 return GL_ONE;
    }
}

static GLenum ToBlendOp(uint32_t op)
{
    switch (op) {
    case BLENDOP_SUBTRACT:    return GL_FUNC_SUBTRACT;
    case BLENDOP_REVSUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case BLENDOP_MIN:         return GL_MIN;
    case BLENDOP_MAX:         return GL_MAX;
    default:                  return GL_FUNC_ADD;
    }
}

static GLenum ToCompare(uint32_t c)
{
    switch (c) {
    case CMP_NEVER:        return GL_NEVER;
    case CMP_LESS:         return GL_LESS;
    case CMP_EQUAL:        return GL_EQUAL;
    case CMP_LESSEQUAL:    return GL_LEQUAL;
    case CMP_GREATER:      return GL_GREATER;
    case CMP_NOTEQUAL:     return GL_NOTEQUAL;
    case CMP_GREATEREQUAL: return GL_GEQUAL;
    default:               return GL_ALWAYS;
    }
}

static GLenum ToTopology(uint32_t t)
{
    switch (t) {
    case POINTLIST: return GL_POINTS;
    case LINELIST:  return GL_LINES;
    case LINESTRIP: return GL_LINE_STRIP;
    case TRIANGLELIST:
    case TRIANGLEFAN:  return GL_TRIANGLES;
    default:        return GL_TRIANGLE_STRIP;
    }
}

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
    k |= (uint64_t)(~s.colorWrite & 15) << 25;
    return k;
}

// Fixed-function state for a pipeline key (what SDL GPU bakes into a pipeline object). The GL
// state is shadowed so that only what differs from the previous draw is sent to the driver;
// InvalidateState() forgets the shadow after code that changes state directly (clears, present).
static struct {
    int blend, depthTest, depthMask, cull, colorMask;
    GLenum src, dst, eq, depthFunc, cullFace;
} g_gs;

static void InvalidateState() { memset(&g_gs, 0xff, sizeof(g_gs)); }

static void ApplyState(uint64_t key)
{
    // RECOMP_GL_DEBUG: bit 0 no culling, bit 1 no depth test, bit 2 opposite cull direction (bug hunts)
    static const int dbg = getenv("RECOMP_GL_DEBUG") ? atoi(getenv("RECOMP_GL_DEBUG")) : 0;
    if (dbg & 1) key &= ~(3ull << 21);
    if (dbg & 2) key &= ~(1ull << 15);
    int blend = (key >> 3) & 1;
    if (blend != g_gs.blend) { blend ? gl.Enable(GL_BLEND) : gl.Disable(GL_BLEND); g_gs.blend = blend; }
    if (blend) {
        GLenum src = ToBlend((key >> 4) & 15), dst = ToBlend((key >> 8) & 15), eq = ToBlendOp((key >> 12) & 7);
        if (src != g_gs.src || dst != g_gs.dst) { gl.BlendFunc(src, dst); g_gs.src = src; g_gs.dst = dst; }
        if (eq != g_gs.eq) { gl.BlendEquation(eq); g_gs.eq = eq; }
    }
    int depthTest = (key >> 15) & 1;   // GL also skips depth writes without the test, like D3D's ZENABLE off
    if (depthTest != g_gs.depthTest) { depthTest ? gl.Enable(GL_DEPTH_TEST) : gl.Disable(GL_DEPTH_TEST); g_gs.depthTest = depthTest; }
    if (depthTest) {
        GLenum func = ToCompare((key >> 17) & 15);
        int mask = (key >> 16) & 1;
        if (func != g_gs.depthFunc) { gl.DepthFunc(func); g_gs.depthFunc = func; }
        if (mask != g_gs.depthMask) { gl.DepthMask(mask); g_gs.depthMask = mask; }
    }
    uint32_t cullMode = (key >> 21) & 3;
    int cull = cullMode == CULL_CCW || cullMode == CULL_CW;
    if (cull != g_gs.cull) { cull ? gl.Enable(GL_CULL_FACE) : gl.Disable(GL_CULL_FACE); g_gs.cull = cull; }
    if (cull) {
        // D3DCULL_CCW is GL_FRONT with glFrontFace(GL_CCW) here: checked against the SDL GPU
        // renderer (the other way round, culled models such as buildings, platforms, ballast
        // and the sky vanished)
        GLenum face = (cullMode == CULL_CCW) != ((dbg & 4) != 0) ? GL_FRONT : GL_BACK;
        if (face != g_gs.cullFace) { gl.CullFace(face); g_gs.cullFace = face; }
    }
    int cw = (int)(~(uint32_t)(key >> 25) & 15);
    if (cw != g_gs.colorMask) { gl.ColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1); g_gs.colorMask = cw; }
}

static GLuint GetSampler(uint32_t key)
{
    auto it = g->samplers.find(key);
    if (it != g->samplers.end()) return it->second;
    auto mode = [](uint32_t a) {
        return a == TADDRESS_CLAMP ? GL_CLAMP_TO_EDGE : a == TADDRESS_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT;
    };
    GLuint s = 0;
    gl.GenSamplers(1, &s);
    bool linear = (key >> 4) & 1;
    // with mipmaps on, minification uses them (textures without mipmaps have MAX_LEVEL 0)
    GLenum minf = g_cfgMipmaps ? (linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST) : (linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minf);
    gl.SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_S, mode(key & 3));
    gl.SamplerParameteri(s, GL_TEXTURE_WRAP_T, mode((key >> 2) & 3));
    g->samplers[key] = s;
    return s;
}

static uint32_t SamplerKey(uint32_t addrU, uint32_t addrV, bool linear)
{
    return (addrU & 3) | (addrV & 3) << 2 | (linear ? 1u : 0u) << 4;
}

static uint32_t BytesPerTexel(TexFormat f) { return f == TEXFMT_P8 ? 1 : 4; }

static void MarkDirty(CGraphicTexture *t)
{
    if (!t->bDirty) {
        t->bDirty = true;
        g->dirtyTex.push_back(t);
    }
}

// GL errors go to the log (first 50), with what was being done: device-only problems (out of
// memory, driver limits) are otherwise invisible.
static void CheckGL(const char *what, int a = 0, int b = 0)
{
    static int reported;
    GLenum e;
    while ((e = gl.GetError()) != GL_NO_ERROR)
        if (reported < 50) { reported++; SDL_Log("GraphicGL: GL error 0x%04x after %s (%d, %d)", e, what, a, b); }
}

constexpr GLenum GL_RGB_ = 0x1907, GL_RGB565_ = 0x8D62, GL_UNSIGNED_SHORT_5_6_5_ = 0x8363;

// levels > 1: storage for mipmaps (allocated at once with glTexStorage2D where available; Mali
// drivers otherwise reallocate on glGenerateMipmap).
static uint32_t g_poolReused;                     // RECOMP_TRACE_SPIKES: textures taken from texPool

static uint64_t PoolKey(uint32_t w, uint32_t h, TexFormat f, bool rt, uint32_t levels, bool rgb565)
{
    uint64_t fmt = f == TEXFMT_P8 ? 0 : rt ? 1 : rgb565 ? 2 : 3;
    return (uint64_t)w | (uint64_t)h << 16 | (uint64_t)(levels & 0xff) << 32 | fmt << 40;
}

static uint64_t PoolKey(const CGraphicTexture *t, uint32_t levels, bool rgb565)
{
    return PoolKey(t->nWidth, t->nHeight, t->nFormat, t->bRenderTarget, levels, rgb565);
}

static uint64_t PoolBytes(uint64_t key)
{
    uint64_t w = key & 0xffff, h = key >> 16 & 0xffff, fmt = key >> 40;
    uint64_t texel = fmt == 0 ? 1 : fmt == 2 ? 2 : 4;
    return w * h * texel * ((key >> 32 & 0xff) > 1 ? 4 : 3) / 3;   // mipmaps add a third
}

// A pooled texture of that key (0 if none). Render thread.
static GLuint TakePooled(uint64_t key)
{
    auto it = g->texPool.find(key);
    if (it == g->texPool.end() || it->second.empty()) return 0;
    GLuint id = it->second.back();
    it->second.pop_back();
    g->texPoolBytes -= PoolBytes(key);
    return id;
}

// A GL texture for w x h texels of fmt, bound to GL_TEXTURE_2D with only level 0 enabled; levels:
// mip levels to allocate (255 without glTexStorage2D), rgb565: "tex16" storage for opaque
// textures. From texPool when it has one (`reused`). Render thread.
static GLuint AllocTexture(uint32_t w, uint32_t h, TexFormat fmt, bool rt, uint32_t &levels, bool rgb565, bool &reused)
{
    if (!glTexStorage2DPtr) levels = 255;           // see below: one key for every level count
    GLuint id = rt ? 0 : TakePooled(PoolKey(w, h, fmt, rt, levels, rgb565));
    reused = id != 0;
    if (id) {
        gl.BindTexture(GL_TEXTURE_2D, id);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        return id;
    }
    gl.GenTextures(1, &id);
    gl.BindTexture(GL_TEXTURE_2D, id);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum internal = fmt == TEXFMT_P8 ? GL_R8 : rgb565 ? GL_RGB565_ : GL_RGBA8;
    if (glTexStorage2DPtr) {
        glTexStorage2DPtr(GL_TEXTURE_2D, (GLsizei)levels, internal, w, h);
    } else {                                        // mip levels are allocated by glGenerateMipmap
        GLenum f = fmt == TEXFMT_P8 ? GL_RED : rgb565 ? GL_RGB_ : GL_RGBA;
        GLenum type = rgb565 ? GL_UNSIGNED_SHORT_5_6_5_ : GL_UNSIGNED_BYTE;
        gl.TexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, f, type, nullptr);
    }
    if (fmt != TEXFMT_P8 && !rt && !rgb565) {       // staging holds BGRA bytes
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, 0x1905 /* GL_BLUE */);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    return id;
}

static void CreateGLTexture(CGraphicTexture *t, uint32_t levels = 1, bool rgb565 = false)
{
    bool reused;
    t->id = AllocTexture(t->nWidth, t->nHeight, t->nFormat, t->bRenderTarget, levels, rgb565, reused);
    if (reused) g_poolReused++;
    t->levels = (uint8_t)levels;
    t->is565 = rgb565;
}

static bool MakeRenderTargetGL(CGraphicTexture *t)
{
    CreateGLTexture(t);
    gl.GenFramebuffers(1, &t->fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g->depthRB);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

// Variant key: bit 0 mesh, bit 1 flat shading, bits 2-3 texture mode, bit 5 fog, bit 6 batched mesh
// (constants per draw slot), bits 8-10 colour op, bits 11-13 alpha op (both 0 untextured), bits
// 14-16 alpha test function (0: no test), bit 17 v flipped (render target texture).
static uint32_t ProgramKey(bool mesh, bool flat, const FSParams &fs, bool fog, bool pre)
{
    uint32_t texMode = fs.mode & 3;
    uint32_t aFunc = fs.alphaTest && fs.alphaFunc != CMP_ALWAYS ? fs.alphaFunc & 7 : 0;
    uint32_t k = (mesh ? 1u : 0u) | (flat ? 2u : 0u) | texMode << 2 | (mesh && fog ? 32u : 0u) | (mesh && pre ? 64u : 0u) |
                 aFunc << 14;
    if (texMode) k |= (fs.colorOp & 7) << 8 | (fs.alphaOp & 7) << 11 | (fs.flipV ? 1u : 0u) << 17;
    return k;
}

// Program binaries (shaders.bin in the settings folder): a variant compiles in ~80 ms on the
// Mali-G31 and variants are made on first use, so each is compiled once per device and loaded
// from the cache afterwards. The file starts with a hash of the driver and the shader sources.
static void (*glGetProgramBinaryPtr)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
static void (*glProgramBinaryPtr)(GLuint, GLenum, const void *, GLsizei);
static void (*glProgramParameteriPtr)(GLuint, GLenum, GLint);
static std::string g_prefDir;                       // LoadConfig: settings folder (with a trailing /)
static uint64_t g_cacheHash;
static FILE *g_cacheFile;                           // open for appending once the cache is valid
static uint32_t g_cacheLoaded, g_cacheCompiled;

static uint64_t Fnv(uint64_t h, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= static_cast<const uint8_t *>(p)[i]; h *= 0x100000001b3ull; }
    return h;
}

static void SetupProgram(GLuint p)
{
    gl.UseProgram(p);
    gl.Uniform1i(gl.GetUniformLocation(p, "uTex"), 0);
    gl.Uniform1i(gl.GetUniformLocation(p, "uPal"), 1);
    gl.Uniform1i(gl.GetUniformLocation(p, "uTexA"), 0);
    GLuint pb = gl.GetUniformBlockIndex(p, "Params"), db = gl.GetUniformBlockIndex(p, "Draws");
    if (pb != 0xffffffffu) gl.UniformBlockBinding(p, pb, 0);
    if (db != 0xffffffffu) gl.UniformBlockBinding(p, db, 1);
}

static void CacheProgram(uint32_t key, GLuint p)
{
    if (!g_cacheFile || !glGetProgramBinaryPtr) return;
    GLint len = 0;
    gl.GetProgramiv(p, 0x8741 /* GL_PROGRAM_BINARY_LENGTH */, &len);
    if (len <= 0) return;
    std::vector<uint8_t> bin((size_t)len);
    GLenum fmt = 0;
    GLsizei got = 0;
    glGetProgramBinaryPtr(p, len, &got, &fmt, bin.data());
    if (got <= 0) return;
    uint32_t hdr[3] = {key, fmt, (uint32_t)got};
    fwrite(hdr, sizeof(hdr), 1, g_cacheFile);
    fwrite(bin.data(), 1, (size_t)got, g_cacheFile);
    fflush(g_cacheFile);
}

static std::string ShaderDefs(uint32_t key)
{
    char defs[256];
    SDL_snprintf(defs, sizeof(defs),
                 "#define FLAT %s\n#define TEXMODE %u\n#define FOG %u\n#define DRAW_SLOTS %u\n#define COLOROP %u\n"
                 "#define ALPHAOP %u\n#define ATEST %u\n#define FLIPV %u\n#define BATCHED %u\n",
                 (key & 2) ? "flat" : "", (key >> 2) & 3, (key >> 5) & 1, g->batchSlots, (key >> 8) & 7, (key >> 11) & 7,
                 (key >> 14) & 7, (key >> 17) & 1, (key >> 6) & 1);
    return defs;
}

// Compiles and sets up a variant in the current context (render thread or the warm-up thread).
static GLuint CompileProgram(uint32_t key)
{
    bool mesh = key & 1;
    std::string pre = g->shaderHead + ShaderDefs(key) + kParams;
    const char *vs = !mesh ? kVS2D : (key & 64) ? kVSMeshBatch : kVSMesh;
    std::string fpre = pre;
    if (g_exp & 1) fpre += "#define EXP_TRIVIAL 1\n";
    // fragment colour math in mediump (texture coordinates are declared highp)
    if (g->gles) fpre += "precision mediump float;\n";
    uint64_t c0 = SDL_GetTicksNS();
    GLuint p = Link(pre + vs, fpre + kCommonFS + (mesh ? kFSMesh : kFS2D));
    if (p) {
        SetupProgram(p);
        SDL_Log("GraphicGL: compiled shader variant %05x (%.0f ms%s)", key, (SDL_GetTicksNS() - c0) / 1e6,
                SDL_GetCurrentThreadID() == g->renderThread ? "" : ", warm-up");
    }
    return p;
}

// A variant's program, compiled on first use (render thread). Variants are shared with the warm-up
// thread (WarmMain): one being compiled there is waited for instead of compiled twice.
static GLuint GetProgram(uint32_t key)
{
    auto it = g->programs.find(key);
    if (it != g->programs.end()) return it->second;
    {
        std::unique_lock<std::mutex> lk(g->progLock);
        g->progCv.wait(lk, [key] { return !g->compiling.count(key); });
        auto sp = g->sharedPrograms.find(key);
        if (sp != g->sharedPrograms.end()) return g->programs[key] = sp->second;
        g->compiling.insert(key);
    }
    GLuint p = CompileProgram(key);                 // 0 on failure: the draws are skipped
    {
        std::lock_guard<std::mutex> lk(g->progLock);
        g->sharedPrograms[key] = p;
        g->compiling.erase(key);
        if (p) { CacheProgram(key, p); g_cacheCompiled++; }
    }
    g->progCv.notify_all();
    return g->programs[key] = p;
}

// Variants seen in play (menus, the HUD, driving: ProgramKey values with batching), compiled ahead
// of use by the warm-up thread unless the program cache has them.
static const uint32_t kWarmKeys[] = {0x00002, 0x02504, 0x02506, 0x22506, 0x02465, 0x02467, 0x1e465,
                                     0x1e467, 0x1e445, 0x1e447, 0x16445, 0x02445, 0x02447};

static bool MakeCurrentSurfaceless(SDL_GLContext ctx);

// The warm-up thread: its own context in the share group, current without a surface.
static void WarmMain()
{
    NameThread("gl-warm");
    if (!MakeCurrentSurfaceless(g->warmCtx)) return;
    uint64_t t0 = SDL_GetTicksNS();
    int n = 0;
    for (uint32_t key : kWarmKeys) {
        if (!g->batching && (key & 1)) key &= ~64u;
        {
            std::lock_guard<std::mutex> lk(g->progLock);
            if (g->sharedPrograms.count(key) || g->compiling.count(key)) continue;
            g->compiling.insert(key);
        }
        GLuint p = CompileProgram(key);
        gl.Finish();                                // complete before another context uses it
        {
            std::lock_guard<std::mutex> lk(g->progLock);
            g->sharedPrograms[key] = p;
            g->compiling.erase(key);
            if (p) { CacheProgram(key, p); g_cacheCompiled++; n++; }
        }
        g->progCv.notify_all();
    }
    if (n) SDL_Log("GraphicGL: warm-up compiled %d shader variants in %.0f ms", n, (SDL_GetTicksNS() - t0) / 1e6);
    SDL_GL_MakeCurrent(nullptr, nullptr);
}

// Loads the cached program binaries (SetupGL) and opens the cache for the variants still to come.
static void LoadProgramCache()
{
    glGetProgramBinaryPtr = reinterpret_cast<void (*)(GLuint, GLsizei, GLsizei *, GLenum *, void *)>(
        SDL_GL_GetProcAddress("glGetProgramBinary"));
    glProgramBinaryPtr = reinterpret_cast<void (*)(GLuint, GLenum, const void *, GLsizei)>(SDL_GL_GetProcAddress("glProgramBinary"));
    glProgramParameteriPtr = reinterpret_cast<void (*)(GLuint, GLenum, GLint)>(SDL_GL_GetProcAddress("glProgramParameteri"));
    GLint formats = 0;
    gl.GetIntegerv(0x87FE /* GL_NUM_PROGRAM_BINARY_FORMATS */, &formats);
    if (!glGetProgramBinaryPtr || !glProgramBinaryPtr || formats <= 0 || g_prefDir.empty() || getenv("RECOMP_GL_NO_SHADER_CACHE"))
        return;
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *src : {kParams, kCommonFS, kVS2D, kFS2D, kVSMesh, kVSMeshBatch, kFSMesh})
        h = Fnv(h, src, strlen(src));
    for (GLenum e : {GL_VERSION, GL_RENDERER}) { const char *v = (const char *)gl.GetString(e); if (v) h = Fnv(h, v, strlen(v)); }
    h = Fnv(h, g->shaderHead.data(), g->shaderHead.size());
    h = Fnv(h, &g->batchSlots, sizeof(g->batchSlots));
    h = Fnv(h, &g_exp, sizeof(g_exp));
    g_cacheHash = h;
    std::string path = g_prefDir + "shaders.bin";
    bool valid = false;
    if (FILE *f = fopen(path.c_str(), "rb")) {
        uint64_t fileHash = 0;
        valid = fread(&fileHash, 8, 1, f) == 1 && fileHash == h;
        uint32_t hdr[3];
        std::vector<uint8_t> bin;
        while (valid && fread(hdr, sizeof(hdr), 1, f) == 1) {
            bin.resize(hdr[2]);
            if (fread(bin.data(), 1, hdr[2], f) != hdr[2]) break;
            if (g->programs.count(hdr[0])) continue;
            GLuint p = gl.CreateProgram();
            glProgramBinaryPtr(p, hdr[1], bin.data(), (GLsizei)hdr[2]);
            GLint ok = 0;
            gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) { gl.DeleteProgram(p); continue; }   // compiled again on first use
            SetupProgram(p);
            g->programs[hdr[0]] = p;
            g->sharedPrograms[hdr[0]] = p;
            g_cacheLoaded++;
        }
        fclose(f);
    }
    g_cacheFile = fopen(path.c_str(), valid ? "ab" : "wb");
    if (g_cacheFile && !valid) { fwrite(&h, 8, 1, g_cacheFile); fflush(g_cacheFile); }
}

// vsync = 0 (graphics.ini) or RECOMP_VSYNC=0: don't wait for the display (may tear). With
// frameskip a slow device misses most refreshes anyway, and waiting for the next one stalls the game.
static int VsyncSetting()
{
    const char *vs = getenv("RECOMP_VSYNC");
    return vs ? *vs != '0' : g_cfgVsync != 0;
}

// The present shader and its objects, in the current context.
static bool CreatePresentRes(PresentRes &r)
{
    r.prog = Link(g->shaderHead + kVSPresent, g->shaderHead + kFSPresent);
    if (!r.prog) return false;
    gl.UseProgram(r.prog);
    gl.Uniform1i(gl.GetUniformLocation(r.prog, "uScene"), 0);
    gl.Uniform1i(gl.GetUniformLocation(r.prog, "uLut"), 2);   // unit 2: unused by the scene (1 is the palette)
    r.uPresent = gl.GetUniformLocation(r.prog, "uPresent");
    r.uScaleY = gl.GetUniformLocation(r.prog, "uScaleY");
    r.uLutOn = gl.GetUniformLocation(r.prog, "uLutOn");
    r.uClamp = gl.GetUniformLocation(r.prog, "uClamp");
    gl.GenTextures(1, &r.lut);
    gl.BindTexture(GL_TEXTURE_2D, r.lut);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    gl.GenVertexArrays(1, &r.vao);
    for (int i = 0; i < 2; i++) {
        GLuint &smp = i ? r.samplerLinear : r.samplerNearest;
        gl.GenSamplers(1, &smp);
        gl.SamplerParameteri(smp, GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR : GL_NEAREST);
        gl.SamplerParameteri(smp, GL_TEXTURE_MAG_FILTER, i ? GL_LINEAR : GL_NEAREST);
        gl.SamplerParameteri(smp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.SamplerParameteri(smp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return true;
}

static std::string ShaderHead()
{
    return g->gles ? "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n"
                   : "#version 330 core\n";
}

static void PresentMain();

// A context made current without a window surface: SDL reports success on some video drivers
// (offscreen) while leaving no context current, so ask GL.
static bool MakeCurrentSurfaceless(SDL_GLContext ctx)
{
    if (!SDL_GL_MakeCurrent(nullptr, ctx)) return false;
    if (gl.GetString(GL_VERSION)) return true;
    SDL_SetError("no context current after SDL_GL_MakeCurrent");
    SDL_GL_MakeCurrent(nullptr, nullptr);
    return false;
}

// Render thread: asyncPresent when the render context can be current without a surface and the
// present thread could make the window's context current. Before any GL object is created, so
// that a fallback to presenting here (the window's context) loses nothing.
static bool ChooseContexts()
{
    if (g->renderCtx && MakeCurrentSurfaceless(g->renderCtx)) {
        g->shaderHead = ShaderHead();
        g->presentThread = std::thread(PresentMain);
        std::unique_lock<std::mutex> lk(g->presentLock);
        g->presentCv.wait(lk, [] { return g->presentSetupDone; });
        if (g->presentSetupOk) { g->asyncPresent = true; return true; }
        lk.unlock();
        g->presentThread.join();
        SDL_Log("GraphicGL: present thread failed, presenting on the render thread");
    } else if (g->renderCtx) {
        SDL_Log("GraphicGL: no surfaceless context (%s), presenting on the render thread", SDL_GetError());
    }
    if (!SDL_GL_MakeCurrent(g->pWindow, g->ctx)) return FAIL("SDL_GL_MakeCurrent");
    SDL_GL_SetSwapInterval(VsyncSetting());
    return true;
}

// One-time GL setup on the render thread (first BeginFrame).
static bool SetupGL()
{
    if (g->glReady) return true;
    if (g->glFailed) return false;
    g->glFailed = true;                             // until everything below worked
    if (!LoadGL()) return FAIL("OpenGL entry points");
    if (!ChooseContexts()) return false;
    g->renderThread = SDL_GetCurrentThreadID();
    SDL_Log("GraphicGL: batching %d, fastalpha %d, mipmaps %d, vsync %d, threaded %d, async present %d, tex16 %d",
            g->batching ? 1 : 0, g_cfgFastAlpha, g_cfgMipmaps, VsyncSetting(), g->threaded ? 1 : 0, g->asyncPresent ? 1 : 0,
            g_cfgTex16);
    SDL_Log("GraphicGL: %s / %s", (const char *)gl.GetString(GL_VERSION), (const char *)gl.GetString(GL_RENDERER));

    g_exp = getenv("RECOMP_GL_EXP") ? atoi(getenv("RECOMP_GL_EXP")) : 0;
    if (g_exp) SDL_Log("GraphicGL: RECOMP_GL_EXP=%d", g_exp);
    g->shaderHead = ShaderHead();
    GLint maxBlock = 16384, uboAlign = 16;
    gl.GetIntegerv(0x8A30 /* GL_MAX_UNIFORM_BLOCK_SIZE */, &maxBlock);
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uboAlign);
    g->uboAlign = (uint32_t)SDL_max(uboAlign, 16);
    g->batchSlots = (uint32_t)SDL_clamp(maxBlock / (GLint)kSlotBytes, 16, 255);
    SDL_Log("GraphicGL: uniform blocks up to %d bytes: %u draws per batch", maxBlock, g->batchSlots);
    GLint maxLayers = 256;
    gl.GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
    g->maxArrayLayers = (uint32_t)SDL_max(maxLayers, 2);
    SDL_Log("GraphicGL: texture arrays %d (up to %d layers)", g->texArrays ? 1 : 0, maxLayers);
    // Shader variants: the ones used before come from the program binary cache; others are compiled
    // on first use (once per device: then they are cached too). The untextured 2D variant checks
    // that compiling works at all.
    uint64_t c0 = SDL_GetTicksNS();
    LoadProgramCache();
    if (g->warmCtx && CGraphic::m_nTexFormat != TEXFMT_P8) g->warmThread = std::thread(WarmMain);
    FSParams plain = {};
    if (!GetProgram(ProgramKey(false, false, plain, false, false))) return FAIL("shaders");
    SDL_Log("GraphicGL: %u shader variants from the cache, %u compiled, in %.0f ms", g_cacheLoaded, g_cacheCompiled,
            (SDL_GetTicksNS() - c0) / 1e6);
    if (!g->asyncPresent && !CreatePresentRes(g->inlinePresent)) return FAIL("present shader");

    GLint align = 16;
    gl.GetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &align);
    if (align < 16) align = 16;
    g->uboStride = (uint32_t)((sizeof(ParamBlock) + align - 1) / align * align);
    gl.GenBuffers(1, &g->ubo);

    uint32_t w = (uint32_t)CGraphic::m_Config.scrwidth, h = (uint32_t)CGraphic::m_Config.scrheight;
    gl.GenRenderbuffers(1, &g->depthRB);
    gl.BindRenderbuffer(GL_RENDERBUFFER, g->depthRB);
    gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    if (gl.GetError() != GL_NO_ERROR) gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    for (int i = 0; i < 2; i++)
        if (!MakeRenderTargetGL(g->tex[i])) return FAIL("render target");

    gl.GenTextures(1, &g->paletteTex);
    gl.BindTexture(GL_TEXTURE_2D, g->paletteTex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kPaletteAtlasW, kPaletteAtlasH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  g->paletteCPU.data());
    for (uint32_t id : g->dirtyPalettes) g->paletteDirty[id] = false;     // all uploaded just now
    g->dirtyPalettes.clear();

    gl.GenBuffers(1, &g->vb2D);
    gl.GenBuffers(1, &g->arenaVB);
    gl.GenVertexArrays(1, &g->vao2D);
    gl.BindVertexArray(g->vao2D);
    gl.BindBuffer(GL_ARRAY_BUFFER, g->vb2D);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, x));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(Vertex), (void *)offsetof(Vertex, color));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, u));
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(3, 1, GL_FLOAT, 0, sizeof(Vertex), (void *)offsetof(Vertex, layer));
    gl.GenVertexArrays(1, &g->vaoMesh);
    gl.BindVertexArray(g->vaoMesh);
    gl.BindBuffer(GL_ARRAY_BUFFER, g->arenaVB);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(MeshVertex), (void *)offsetof(MeshVertex, x));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 3, GL_FLOAT, 0, sizeof(MeshVertex), (void *)offsetof(MeshVertex, nx));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(MeshVertex), (void *)offsetof(MeshVertex, u));
    gl.GenBuffers(1, &g->preVB);
    gl.GenBuffers(1, &g->slotVB);
    gl.GenBuffers(1, &g->indexVB);
    gl.GenBuffers(1, &g->uboDraws);
    gl.GenVertexArrays(1, &g->vaoPre);
    gl.BindVertexArray(g->vaoPre);
    gl.BindBuffer(GL_ARRAY_BUFFER, g->preVB);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, 0, sizeof(PreVertex), (void *)offsetof(PreVertex, x));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 3, GL_SHORT, 0, sizeof(PreVertex), (void *)offsetof(PreVertex, nx));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 2, GL_SHORT, 0, sizeof(PreVertex), (void *)offsetof(PreVertex, u));
    gl.BindBuffer(GL_ARRAY_BUFFER, g->slotVB);
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(3, 1, GL_UNSIGNED_BYTE, 0, 1, nullptr);
    gl.BindVertexArray(0);

    gl.FrontFace(GL_CCW);                           // see ApplyState for the cull modes
    g->glReady = true;
    g->glFailed = false;
    return true;
}

static bool OnRenderThread()
{
    return g->glReady && SDL_GetCurrentThreadID() == g->renderThread;
}

// ---------------------------------------------------------------------------------------------
// Flush: uploads, then the recorded draws in order

static void ArenaRelease(uint32_t offset, uint32_t count);

static void FillBlockFS(ParamBlock &b, const FSParams &f)
{
    b.f0[0] = (int32_t)f.mode; b.f0[1] = (int32_t)f.colorOp; b.f0[2] = (int32_t)f.alphaOp; b.f0[3] = (int32_t)f.alphaFunc;
    b.f1[0] = (int32_t)f.addrU; b.f1[1] = (int32_t)f.addrV; b.f1[2] = (int32_t)f.linear; b.f1[3] = (int32_t)f.flipV;
    b.f2[0] = (int32_t)f.texW; b.f2[1] = (int32_t)f.texH; b.f2[2] = (int32_t)f.palX; b.f2[3] = (int32_t)f.palY;
    b.f3[0] = (int32_t)lroundf(f.alphaRef * 255.0f);
}

static ParamBlock *NewBlock(std::vector<uint8_t> &blockData, uint32_t &index)
{
    index = (uint32_t)(blockData.size() / g->uboStride);
    blockData.resize(blockData.size() + g->uboStride);
    auto *b = reinterpret_cast<ParamBlock *>(&blockData[(size_t)index * g->uboStride]);
    memset(b, 0, sizeof(ParamBlock));
    return b;
}

static void GLViewportRect(const SDL_Rect &r, uint32_t targetH, bool scissor)
{
    int y = (int)targetH - (r.y + r.h);
    if (scissor) gl.Scissor(r.x, y, r.w, r.h);
    else gl.Viewport(r.x, y, r.w, r.h);
}

static Job *AcquireJob()
{
    if (g->freeJobs.empty()) return new Job();
    Job *j = g->freeJobs.back();
    g->freeJobs.pop_back();
    return j;
}

static void ReleaseJob(Job *j)
{
    j->verts.clear(); j->draws.clear(); j->meshParams.clear(); j->stripRuns.clear();
    j->texUploads.clear(); j->meshUploads.clear(); j->palIds.clear(); j->palData.clear();
    j->deadTex.clear(); j->deadMeshes.clear(); j->uploadReqs.clear();
    j->newArenaCap = 0;
    j->present = j->snapshot = false;
    j->clearColor = j->clearDepth = false;
    j->list.built = false;
    j->listState = 0;
    std::lock_guard<std::mutex> lk(g->queueLock);   // freeJobs is shared with the render thread
    g->freeJobs.push_back(j);
}

// Takes the recorded work (nullptr if there is none and `always` is false). Game thread, g->lock held.
static Job *Capture(bool always)
{
    bool haveWork = !g->draws.empty() || g->pendingClearColor || g->pendingClearDepth;
    if (!always && !haveWork && g->dirtyTex.empty() && g->uploadReqs.empty() && g->dirtyMeshes.empty() && g->dirtyPalettes.empty() &&
        g->deadTex.empty() && g->deadMeshes.empty())
        return nullptr;
    Job *j;
    {
        std::lock_guard<std::mutex> lk(g->queueLock);
        j = AcquireJob();
    }
    g_stats.flushes++;
    g_stats.drawCmds += (uint32_t)g->draws.size();
    g_stats.vertexBytes += (uint32_t)(g->verts.size() * sizeof(Vertex));
    j->verts.swap(g->verts);
    j->draws.swap(g->draws);
    j->meshParams.swap(g->meshParams);
    j->stripRuns.swap(g->stripRuns);
    // the vectors come back from the job pool with any capacity: room for a frame like this one
    // (growing them while recording cost reallocations and copies every frame)
    g->verts.reserve(j->verts.size());
    g->draws.reserve(j->draws.size());
    g->meshParams.reserve(j->meshParams.size());
    g->stripRuns.reserve(j->stripRuns.size());
    g->lastStrip.mesh = nullptr;
    j->target = g->tex[g->target];
    j->clearColor = g->pendingClearColor;
    j->clearDepth = g->pendingClearDepth;
    memcpy(j->clearColorV, g->clearColor, sizeof(j->clearColorV));
    j->clearDepthV = g->clearDepth;
    g->pendingClearColor = g->pendingClearDepth = false;

    for (CGraphicTexture *t : g->dirtyTex) {
        if (t->earlyCovered) {                      // uploaded through uploadReqs (below)
            t->earlyCovered = t->bDirty = false;
            g_stats.texUploads++;
            g_stats.texUploadBytes += (uint32_t)t->staging.size();
            continue;
        }
        j->texUploads.push_back({t, t->anyTransparent, {}});
        if (g->threaded) j->texUploads.back().data = t->staging;
        t->bDirty = false;
        g_stats.texUploads++;
        g_stats.texUploadBytes += (uint32_t)t->staging.size();
    }
    g->dirtyTex.clear();
    j->uploadReqs.swap(g->uploadReqs);

    if (!g->batching && g->arenaCapSent != g->arenaCap) {
        j->newArenaCap = g->arenaCap;               // the new buffer starts empty: upload every mesh
        g->arenaCapSent = g->arenaCap;
        for (CGraphicMesh *m : g->meshes)
            if (!m->bDirty) { m->bDirty = true; g->dirtyMeshes.push_back(m); }
    }
    for (CGraphicMesh *m : g->dirtyMeshes) {
        m->bDirty = false;
        if (!g->batching) {                         // batched draws read the vertices on the CPU
            j->meshUploads.push_back(m);
            g_stats.meshUploadBytes += (uint32_t)(m->staging.size() * sizeof(MeshVertex));
        }
    }
    g->dirtyMeshes.clear();

    for (uint32_t id : g->dirtyPalettes) {
        uint32_t px = (id % 16) * 256, py = id / 16;
        const uint8_t *row = &g->paletteCPU[((size_t)py * kPaletteAtlasW + px) * 4];
        j->palIds.push_back(id);
        j->palData.insert(j->palData.end(), row, row + 256 * 4);
        g->paletteDirty[id] = false;
    }
    g->dirtyPalettes.clear();

    j->deadTex.swap(g->deadTex);
    j->deadMeshes.swap(g->deadMeshes);
    // the arena is CPU bookkeeping: a range reused by a new mesh is uploaded by a later job, after
    // this job's draws in GL's command order
    for (auto &f : g->arenaPendingFree) ArenaRelease(f.first, f.second);
    g->arenaPendingFree.clear();
    return j;
}

// A freed texture's GL object: into texPool for reuse (threaded, within 64 MB), else deleted later.
static void ReleaseGLTexture(CGraphicTexture *t)
{
    if (t->arr) {                                   // the layer is free for the next texture of its class
        t->arr->freeLayers.push_back(t->layer);
        t->arr = nullptr;
        return;
    }
    if (!t->id) return;
    uint64_t key = PoolKey(t, t->levels, t->is565);
    if (g->threaded && !t->bRenderTarget && g->texPoolBytes + PoolBytes(key) <= 64ull << 20) {
        g->texPool[key].push_back(t->id);
        g->texPoolBytes += PoolBytes(key);
    } else {
        g->deleteIds.push_back(t->id);
    }
    t->id = 0;
}

static void Present(CGraphicTexture &scene, float gamma);
static void HandOff(CGraphicTexture &scene, float gamma);
static void AcquireTarget(CGraphicTexture *t);
static bool SaveScene(CGraphicTexture &scene);
static void TraceFrame(uint64_t flushNs, uint64_t swapNs);
static uint64_t g_gpuSampleNs, g_gpuSamples;          // RECOMP_TRACE_TICKS: sampled GPU time per frame

static uint64_t g_upAllocNs, g_upDataNs, g_upMipNs;    // RECOMP_TRACE_SPIKES: upload time split
static std::vector<uint16_t> g_conv565;

static void ReleaseGLTexture(CGraphicTexture *t);

static uint64_t ArrayClass(uint32_t w, uint32_t h, uint32_t levels, bool rgb565)
{
    return (uint64_t)w | (uint64_t)h << 16 | (uint64_t)levels << 32 | (uint64_t)(rgb565 ? 1 : 0) << 40;
}

// A new array of the class. The first one is small; each further one is as large as the class's
// arrays so far together (so at most about half of a class's memory is unused), within 16 MB
// and 128 layers. GPU memory is the device's 1 GB of RAM.
static TexArray *NewArray(uint64_t cls, uint32_t w, uint32_t h, uint32_t levels, bool rgb565)
{
    uint64_t layerBytes = (uint64_t)w * h * (rgb565 ? 2 : 4) * (levels > 1 ? 4 : 3) / 3;
    uint64_t have = 0;
    for (TexArray *a : g->arrays[cls]) have += a->layers;
    uint64_t maxLayers = SDL_clamp((16ull << 20) / SDL_max(layerBytes, 1ull), 1ull, (uint64_t)SDL_min(128u, g->maxArrayLayers));
    uint64_t first = SDL_clamp((1ull << 20) / SDL_max(layerBytes, 1ull), 2ull, 16ull);
    uint32_t n = (uint32_t)SDL_min(SDL_max(have, first), maxLayers);
    auto *a = new TexArray();
    a->cls = cls; a->w = w; a->h = h; a->levels = levels; a->layers = n; a->is565 = rgb565;
    gl.GenTextures(1, &a->id);
    gl.BindTexture(GL_TEXTURE_2D_ARRAY, a->id);
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum internal = rgb565 ? GL_RGB565_ : GL_RGBA8;
    if (glTexStorage3DPtr) {
        glTexStorage3DPtr(GL_TEXTURE_2D_ARRAY, (GLsizei)levels, internal, w, h, n);
    } else {
        for (uint32_t l = 0; l < levels; l++)
            gl.TexImage3D(GL_TEXTURE_2D_ARRAY, (GLint)l, (GLint)internal, SDL_max(w >> l, 1u), SDL_max(h >> l, 1u), n, 0,
                          rgb565 ? GL_RGB_ : GL_RGBA, rgb565 ? GL_UNSIGNED_SHORT_5_6_5_ : GL_UNSIGNED_BYTE, nullptr);
    }
    if (!rgb565) {                                  // texels are BGRA bytes
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_R, 0x1905 /* GL_BLUE */);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_B, GL_RED);
    }
    for (uint32_t i = n; i-- > 0;) a->freeLayers.push_back((uint16_t)i);
    g->arrays[cls].push_back(a);
    g->arrayCount++;
    g->arrayBytes += layerBytes * n;
    if (CACHED_ENV("RECOMP_TRACE_TICKS"))
        SDL_Log("GraphicGL: texture array %ux%u, %u levels%s: %u layers (%.1f MB; now %u arrays, %.1f MB)", w, h, levels,
                rgb565 ? ", 565" : "", n, layerBytes * n / 1048576.0, g->arrayCount, g->arrayBytes / 1048576.0);
    return a;
}

// 2x2 box filter of BGRA texels (odd edges repeat the last row / column).
static void Downsample(const uint8_t *src, uint32_t sw, uint32_t sh, uint8_t *dst)
{
    uint32_t dw = SDL_max(sw >> 1, 1u), dh = SDL_max(sh >> 1, 1u);
    for (uint32_t y = 0; y < dh; y++) {
        const uint8_t *r0 = src + (size_t)SDL_min(y * 2, sh - 1) * sw * 4, *r1 = src + (size_t)SDL_min(y * 2 + 1, sh - 1) * sw * 4;
        uint8_t *o = dst + (size_t)y * dw * 4;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = SDL_min(x * 2, sw - 1) * 4, x1 = SDL_min(x * 2 + 1, sw - 1) * 4;
            for (int c = 0; c < 4; c++) o[x * 4 + c] = (uint8_t)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
        }
    }
}

static void UploadLevel(uint32_t level, uint32_t layer, uint32_t w, uint32_t h, const uint8_t *bgra, bool rgb565)
{
    if (rgb565) {
        size_t n = (size_t)w * h;
        g_conv565.resize(n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = bgra + i * 4;            // B, G, R, A
            g_conv565[i] = (uint16_t)((p[2] >> 3) << 11 | (p[1] >> 2) << 5 | p[0] >> 3);
        }
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
        gl.TexSubImage3D(GL_TEXTURE_2D_ARRAY, (GLint)level, 0, 0, (GLint)layer, w, h, 1, GL_RGB_, GL_UNSIGNED_SHORT_5_6_5_,
                         g_conv565.data());
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    } else {
        gl.TexSubImage3D(GL_TEXTURE_2D_ARRAY, (GLint)level, 0, 0, (GLint)layer, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, bgra);
    }
}

// An arrayed texture's upload: a layer of its class (moved when the class changed, e.g. the
// texture became transparent) and all its mip levels, filtered on the CPU (generating mipmaps on
// the GPU works on whole arrays).
static void UploadArrayTexture(CGraphicTexture *t, const uint8_t *data, bool anyTransparent)
{
    uint64_t c0 = SDL_GetTicksNS();
    bool opaque = !anyTransparent;
    bool mip = g_cfgMipmaps && opaque && (t->nWidth > 1 || t->nHeight > 1);
    bool rgb565 = g_cfgTex16 && opaque;
    uint32_t levels = 1;
    if (mip) for (uint32_t m = SDL_max(t->nWidth, t->nHeight); m > 1; m >>= 1) levels++;
    uint64_t cls = ArrayClass(t->nWidth, t->nHeight, levels, rgb565);
    if (t->arr && t->arr->cls != cls) ReleaseGLTexture(t);
    if (!t->arr) {
        TexArray *a = nullptr;
        for (TexArray *c : g->arrays[cls]) if (!c->freeLayers.empty()) { a = c; break; }
        if (!a) a = NewArray(cls, t->nWidth, t->nHeight, levels, rgb565);
        t->arr = a;
        t->layer = a->freeLayers.back();
        a->freeLayers.pop_back();
    }
    gl.BindTexture(GL_TEXTURE_2D_ARRAY, t->arr->id);
    uint64_t c1 = SDL_GetTicksNS();
    UploadLevel(0, t->layer, t->nWidth, t->nHeight, data, rgb565);
    uint64_t c2 = SDL_GetTicksNS(), mipNs = 0;
    if (levels > 1) {
        size_t total = 0;
        for (uint32_t l = 1; l < levels; l++) total += (size_t)SDL_max(t->nWidth >> l, 1u) * SDL_max(t->nHeight >> l, 1u) * 4;
        if (g->mipScratch.size() < total) g->mipScratch.resize(total);
        const uint8_t *src = data;
        uint8_t *dst = g->mipScratch.data();
        uint32_t w = t->nWidth, h = t->nHeight;
        for (uint32_t l = 1; l < levels; l++) {
            uint64_t m0 = SDL_GetTicksNS();
            Downsample(src, w, h, dst);
            mipNs += SDL_GetTicksNS() - m0;
            w = SDL_max(w >> 1, 1u); h = SDL_max(h >> 1, 1u);
            UploadLevel(l, t->layer, w, h, dst, rgb565);
            src = dst;
            dst += (size_t)w * h * 4;
        }
    }
    t->mipmapped = mip;
    t->is565 = rgb565;
    uint64_t c3 = SDL_GetTicksNS();
    g_upAllocNs += c1 - c0; g_upDataNs += c3 - c1 - mipNs; g_upMipNs += mipNs;
    (void)c2;
}

static void UploadTexture(CGraphicTexture *t, const uint8_t *data, bool anyTransparent, bool deferMip = false)
{
    if (t->arrayed) { UploadArrayTexture(t, data, anyTransparent); return; }
    uint64_t c0 = SDL_GetTicksNS();
    // mipmaps only for fully opaque textures: alpha-tested foliage would thin out at a distance
    bool opaque = t->nFormat == TEXFMT_BGRA8 && !anyTransparent && !t->bRenderTarget;
    bool mip = g_cfgMipmaps && opaque && (t->nWidth > 1 || t->nHeight > 1);
    bool rgb565 = g_cfgTex16 && opaque;
    uint32_t levels = 1;
    if (mip) for (uint32_t m = SDL_max(t->nWidth, t->nHeight); m > 1; m >>= 1) levels++;
    if (t->id && (t->is565 != rgb565 || t->levels < levels)) {     // the storage no longer fits
        gl.DeleteTextures(1, &t->id);
        t->id = 0;
        t->mipmapped = false;
    }
    if (!t->id) CreateGLTexture(t, levels, rgb565);
    else gl.BindTexture(GL_TEXTURE_2D, t->id);
    uint64_t c1 = SDL_GetTicksNS();
    if (rgb565) {
        size_t n = (size_t)t->nWidth * t->nHeight;
        g_conv565.resize(n);
        for (size_t i = 0; i < n; i++) {
            const uint8_t *p = data + i * 4;            // B, G, R, A
            g_conv565[i] = (uint16_t)((p[2] >> 3) << 11 | (p[1] >> 2) << 5 | p[0] >> 3);
        }
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 2);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->nWidth, t->nHeight, GL_RGB_, GL_UNSIGNED_SHORT_5_6_5_, g_conv565.data());
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    } else {
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->nWidth, t->nHeight, t->nFormat == TEXFMT_P8 ? GL_RED : GL_RGBA,
                         GL_UNSIGNED_BYTE, data);
    }
    uint64_t c2 = SDL_GetTicksNS();
    if (mip && deferMip) {                          // level 0 only until GenerateMipmaps() gets to it
        if (t->mipmapped) gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        t->mipmapped = false;
        if (!t->mipPending) { t->mipPending = true; g->pendingMips.push_back(t); }
    } else {
        if (mip) {
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
            gl.GenerateMipmap(GL_TEXTURE_2D);
        } else if (t->mipmapped) {
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }
        t->mipmapped = mip;
        t->mipPending = false;                      // a stale entry in pendingMips is skipped
    }
    uint64_t c3 = SDL_GetTicksNS();
    g_upAllocNs += c1 - c0; g_upDataNs += c2 - c1; g_upMipNs += c3 - c2;
}

// A scenery change brings a hundred or more textures at once, mostly for the track ahead: with the
// render thread, only those the job draws with are uploaded at once, the others a few per job
// (about 2 ms), so that the frame is not held up. Unthreaded, everything is uploaded at once
// (the uploads read t->staging, which the game may change after this flush).
static void UploadTextures(Job &j)
{
    if (!g->threaded) {
        for (Job::TexUpload &u : j.texUploads) UploadTexture(u.t, u.t->staging.data(), u.anyTransparent);
        return;
    }
    auto &pend = g->pendingUploads;
    for (Job::TexUpload &u : j.texUploads) {
        if (u.t->pendingUpload) {                   // superseded by this newer content
            for (size_t k = 0; k < pend.size(); k++)
                if (pend[k].t == u.t) { pend.erase(pend.begin() + (long)k); break; }
        }
        u.t->pendingUpload = true;
        u.t->neededNow = u.t == g->white;
        pend.push_back(std::move(u));
    }
    if (pend.empty()) return;
    for (const DrawCmd &d : j.draws)
        if (d.pTex && d.pTex->pendingUpload) d.pTex->neededNow = true;
    if (j.target && j.target->pendingUpload) j.target->neededNow = true;
    uint64_t t0 = SDL_GetTicksNS();
    size_t keep = 0;
    for (size_t k = 0; k < pend.size(); k++) {
        Job::TexUpload &u = pend[k];
        bool budget = SDL_GetTicksNS() - t0 < 2000000;
        if (!u.t->dying && (u.t->neededNow || budget)) {
            UploadTexture(u.t, u.data.data(), u.anyTransparent, true);
            u.t->pendingUpload = u.t->neededNow = false;
        } else {
            if (keep != k) pend[keep] = std::move(u);
            keep++;
        }
    }
    pend.resize(keep);
}

// Mipmaps for textures uploaded by UploadTextures (threaded): generating them is most of an upload's
// cost, and the frame can draw with level 0 meanwhile. About 2 ms per job.
static void GenerateMipmaps()
{
    auto &pm = g->pendingMips;
    if (pm.empty()) return;
    uint64_t t0 = SDL_GetTicksNS();
    size_t k = 0;
    for (; k < pm.size() && SDL_GetTicksNS() - t0 < 2000000; k++) {
        CGraphicTexture *t = pm[k];
        if (!t->mipPending) continue;
        t->mipPending = false;
        uint32_t levels = 1;
        for (uint32_t m = SDL_max(t->nWidth, t->nHeight); m > 1; m >>= 1) levels++;
        gl.BindTexture(GL_TEXTURE_2D, t->id);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
        gl.GenerateMipmap(GL_TEXTURE_2D);
        t->mipmapped = true;
    }
    pm.erase(pm.begin(), pm.begin() + (long)k);
    g_upMipNs += SDL_GetTicksNS() - t0;
}

// ---------------------------------------------------------------------------------------------
// Early texture uploads (see UploadReq)

// UnlockTexture (game thread, g->lock held): the texture's new content to the render thread.
static void IssueUpload(CGraphicTexture *t)
{
    auto *r = new UploadReq();
    r->t = t;
    r->anyTransparent = t->anyTransparent;
    r->data = t->staging;
    t->earlyCovered = true;
    g->uploadReqs.push_back(r);
    {
        std::lock_guard<std::mutex> lk(g->queueLock);
        g->upQueue.push_back(r);
    }
    g->queueCv.notify_all();
}

static void ReleaseStandIn(CGraphicTexture *t);

static void Unref(UploadReq *r)
{
    if (--r->refs == 0) delete r;
}

// Uploads a request's content (once). Render thread.
static void DoUpload(UploadReq *r, bool deferMip)
{
    if (r->done) return;
    r->done = true;
    CGraphicTexture *t = r->t;
    if (t->pendingUpload) {                         // older content (UploadTextures) never shows now
        auto &pend = g->pendingUploads;
        for (size_t k = 0; k < pend.size(); k++)
            if (pend[k].t == t) { pend.erase(pend.begin() + (long)k); break; }
        t->pendingUpload = t->neededNow = false;
    }
    UploadTexture(t, r->data.data(), r->anyTransparent, deferMip);
    ReleaseStandIn(t);
    std::vector<uint8_t>().swap(r->data);
}

static uint32_t g_upNow, g_upEarly, g_upHeld, g_upStandIn;     // RECOMP_TRACE_SPIKES: a job's requests

// A stand-in for a model texture whose upload waits, drawn for the few frames until the upload:
// an 8x8 thumbnail when the texture is opaque (a flat colour made textures flash), fully
// transparent when it has transparent texels (an averaged alpha drew cut-out signs and trees as
// half-transparent blocks; they now appear a few frames late instead).
// 8x8: on the R36H's Mali driver filling 168 stand-ins took 3 ms at 8x8 but 86 ms at 16x16 (the
// driver's tiled layout starts at 16x16)
constexpr uint32_t kStandInSize = 8;

static GLuint NewStandIn()
{
    GLuint id;
    gl.GenTextures(1, &id);
    gl.BindTexture(GL_TEXTURE_2D, id);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    // storage allocated and written once here, in idle time
    static const uint8_t zero[kStandInSize * kStandInSize * 4] = {};
    if (glTexStorage2DPtr) {
        glTexStorage2DPtr(GL_TEXTURE_2D, 1, GL_RGBA8, kStandInSize, kStandInSize);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kStandInSize, kStandInSize, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    } else {
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kStandInSize, kStandInSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    }
    return id;
}

// Stand-ins made ahead while the render thread waits (creating a texture costs ~0.45 ms on the
// R36H's Mali driver, even a small one; a scenery change needs about a hundred at once).
constexpr uint32_t kStandIns = 256;

static uint64_t g_standInCpuNs, g_standInGlNs;     // RECOMP_TRACE_SPIKES: making stand-ins

static void MakeStandIn(CGraphicTexture *t, const UploadReq *r)
{
    uint64_t c0 = SDL_GetTicksNS();
    constexpr uint32_t N = kStandInSize;
    uint8_t px[N * N * 4] = {};
    uint32_t w = t->nWidth, h = t->nHeight;
    for (uint32_t cy = 0; cy < N && !r->anyTransparent; cy++)
        for (uint32_t cx = 0; cx < N; cx++) {
            // the cell's texels, at most 4x4 of them (evenly spread)
            uint32_t x0 = cx * w / N, x1 = SDL_max((cx + 1) * w / N, x0 + 1), y0 = cy * h / N, y1 = SDL_max((cy + 1) * h / N, y0 + 1);
            uint32_t sx = SDL_max((x1 - x0) / 4, 1u), sy = SDL_max((y1 - y0) / 4, 1u), n = 0, sum[4] = {};
            for (uint32_t y = y0; y < y1 && y < h; y += sy)
                for (uint32_t x = x0; x < x1 && x < w; x += sx, n++) {
                    const uint8_t *p = &r->data[((size_t)y * w + x) * 4];          // B, G, R, A
                    sum[0] += p[2]; sum[1] += p[1]; sum[2] += p[0]; sum[3] += p[3];
                }
            uint8_t *o = &px[(cy * N + cx) * 4];
            for (int c = 0; c < 4; c++) o[c] = (uint8_t)(n ? sum[c] / n : 0);
        }
    uint64_t c1 = SDL_GetTicksNS();
    if (g->standInPool.empty()) {
        t->standIn = NewStandIn();
    } else {
        t->standIn = g->standInPool.back();
        g->standInPool.pop_back();
        gl.BindTexture(GL_TEXTURE_2D, t->standIn);
    }
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, N, N, GL_RGBA, GL_UNSIGNED_BYTE, px);
    g_standInCpuNs += c1 - c0;
    g_standInGlNs += SDL_GetTicksNS() - c1;
}

static void ReleaseStandIn(CGraphicTexture *t)
{
    if (t->standIn) { g->standInPool.push_back(t->standIn); t->standIn = 0; }
}

// Before a job's draws: the requests not uploaded yet for the textures it draws with; those of
// textures freed after it are dropped, the others wait (for idle time or a later job). Model
// textures that never had content are uploaded within upBudgetNs per 1/60 s of frame time (two
// budgets per frame at 30 frames/s); beyond it they are drawn with a stand-in (MakeStandIn) for
// the few frames until idle time or later jobs get to them, instead of the picture standing still
// while a scenery change's textures upload. Uploads made here get their mipmaps at once.
static void ApplyUploads(Job &j)
{
    g_upNow = g_upEarly = g_upStandIn = 0;
    g_standInCpuNs = g_standInGlNs = 0;
    auto &held = g->heldReqs;
    if (j.uploadReqs.empty() && held.empty()) { g_upHeld = 0; return; }
    for (UploadReq *r : j.uploadReqs) if (r->done) g_upEarly++;
    held.insert(held.end(), j.uploadReqs.begin(), j.uploadReqs.end());
    j.uploadReqs.clear();
    for (const DrawCmd &d : j.draws)
        if (d.pTex) { d.pTex->inUse = true; if (!d.pMesh) d.pTex->need2D = true; }
    uint64_t t0 = SDL_GetTicksNS();
    uint64_t budget = g->upBudgetNs * SDL_clamp(g->presentIntervalNs / 16666667.0, 1.0, 4.0);
    size_t keep = 0;
    for (UploadReq *r : held) {
        CGraphicTexture *t = r->t;
        if (!r->done) {
            if (t->dying && !t->inUse) {
                r->done = true;
            } else if (t->upHeld) {                 // behind an earlier request of the texture
                held[keep++] = r;
                continue;
            } else if (t->inUse) {
                bool standIn = !t->need2D && !t->dying && (!t->id || t->standIn) && t->nFormat == TEXFMT_BGRA8 && budget &&
                               g_cfgStandIns && g->upFrameNs + (SDL_GetTicksNS() - t0) >= budget;
                if (standIn) {
                    if (!t->standIn) { MakeStandIn(t, r); g_upStandIn++; }
                    t->upHeld = true;
                    held[keep++] = r;
                    continue;
                }
                DoUpload(r, false);
                g_upNow++;
            } else {
                t->upHeld = true;
                held[keep++] = r;
                continue;
            }
        }
        Unref(r);
    }
    held.resize(keep);
    for (UploadReq *r : held) r->t->upHeld = false;
    g_upHeld = (uint32_t)keep;
    uint64_t ns = SDL_GetTicksNS() - t0;
    g->upFrameNs += ns;
    g->upJobNs += ns;
    g->upJobCount += g_upNow;
    for (const DrawCmd &d : j.draws) if (d.pTex) d.pTex->inUse = d.pTex->need2D = false;
}

// What a draw binds as its texture: arrayed textures of one array are the same (the layer is per draw).
static uintptr_t TexKey(const CGraphicTexture *t)
{
    return !t ? 0 : t->arr ? (uintptr_t)t->arr : (uintptr_t)t;
}

// Removes repeated vertices of a mesh (bitwise equal: the strips the models came from share them).
// Render thread, on the mesh's first batched draws (Execute spreads the work over jobs); the mesh
// does not change afterwards.
static void BuildIndexed(CGraphicMesh *m)
{
    m->indexed = true;
    size_t n = m->staging.size();
    if (n > 65535 * 3) return;                      // unique vertices could exceed 16-bit indices: not indexed
    size_t cap = 64;
    while (cap < n * 2) cap <<= 1;
    static std::vector<int32_t> table;              // open addressing: index into unique
    if (table.size() < cap) table.resize(cap);
    std::fill(table.begin(), table.begin() + (long)cap, -1);
    m->unique.reserve(n / 2 + 4);
    m->indices.resize(n);
    static_assert(sizeof(MeshVertex) == 32, "hashed as four words");
    for (size_t i = 0; i < n; i++) {
        const MeshVertex &v = m->staging[i];
        uint64_t w[4];
        memcpy(w, &v, sizeof(w));
        uint64_t h = (w[0] * 0x9e3779b97f4a7c15ull) ^ (w[1] * 0xc2b2ae3d27d4eb4full) ^ (w[2] * 0x165667b19e3779f9ull) ^
                     (w[3] * 0xd6e8feb86659fd93ull);
        size_t k = (size_t)(h ^ h >> 32) & (cap - 1);
        for (;; k = (k + 1) & (cap - 1)) {
            int32_t u = table[k];
            if (u < 0) {
                if (m->unique.size() >= 65535) { m->unique.clear(); m->indices.clear(); return; }
                table[k] = (int32_t)m->unique.size();
                m->indices[i] = (uint16_t)m->unique.size();
                m->unique.push_back(Pack(v));
                break;
            }
            PreVertex pv = Pack(v);
            if (!memcmp(&m->unique[(size_t)u], &pv, sizeof(pv))) { m->indices[i] = (uint16_t)u; break; }
        }
    }
    m->unique.shrink_to_fit();
    g->dedupVerts += n;
    g->dedupUnique += m->unique.size();
}

// The draw list of a job (Job::List): parameter blocks, batched vertices, indices and draw slots,
// and the items Execute draws. Uses nothing the render thread changes (texture arrays aside, whose
// layers are assigned by uploads), so the list thread can build it ahead (see ListMain).
static void BuildList(Job &j)
{
    Job::List &L = j.list;
    CGraphicTexture &rt = *j.target;
    // The draw list. Mesh draws get a parameter block per parameter set. Batched mesh draws are
    // copied into a vertex stream in drawing order; runs of order-independent draws
    // ("sortable": depth test and writes on, opaque or alpha-tested over-blending) are stably
    // sorted by everything that affects their pixels, and equal neighbours share one draw call
    // (up to kBatchSlots draws, whose vertex-shader constants are in the Draws uniform buffer).
    uint64_t ins0 = rt_thread_instructions(), tl0 = SDL_GetTicksNS();
    using Item = ListItem;
    std::vector<Item> &items = L.items;
    items.clear();
    items.reserve(j.draws.size());
    L.blockData.clear();
    L.sverts.clear();
    L.sslots.clear();
    L.sindices.clear();
    L.drawUsed = 0;
    L.meshDraws = L.meshVerts = 0;
    auto drawRoom = [&](size_t floats) {        // capacity without zero-filling each append
        if (L.drawUsed + floats > L.drawData.size())
            L.drawData.resize(SDL_max(L.drawUsed + floats, L.drawData.size() * 2 + 4096));
    };
    std::vector<uint32_t> meshBlock(j.meshParams.size(), ~0u);
    auto meshBlockFor = [&](const DrawCmd &d) {
        // unbatched draws of arrayed textures carry the layer in the block (lightLocal[0].w):
        // a block per draw then, as parameter sets do not include the texture
        uint32_t own = ~0u;
        bool layered = !d.pre && d.pTex && d.pTex->arr;
        uint32_t &mb = layered ? own : meshBlock[d.meshParams];
        if (mb == ~0u) {
            const auto &mp = j.meshParams[d.meshParams];
            ParamBlock *b = NewBlock(L.blockData, mb);
            memcpy(b->vs, mp.first.v, sizeof(b->vs));
            FillBlockFS(*b, mp.second.p);
            b->f3[1] = mp.first.kind;
            b->fog[0] = mp.second.fogR; b->fog[1] = mp.second.fogG; b->fog[2] = mp.second.fogB;
            if (layered) b->vs[8][3] = d.pTex->layer;
        }
        return mb;
    };
    auto sortable = [&](const DrawCmd &c) {
        uint64_t k = c.pipeKey;
        if (!c.pre || !(k & (1ull << 15)) || !(k & (1ull << 16)) || (k >> 25 & 15)) return false;
        uint32_t zf = (k >> 17) & 15;
        if (zf != CMP_LESS && zf != CMP_LESSEQUAL) return false;
        if (!(k & (1ull << 3))) return true;
        const FSParams &f = j.meshParams[c.meshParams].second.p;
        return (k >> 4 & 15) == BLEND_SRCALPHA && (k >> 8 & 15) == BLEND_INVSRCALPHA && f.alphaTest &&
               f.alphaFunc == CMP_GREATEREQUAL && f.alphaRef >= 0.5f;
    };
    auto cmpKey = [&](uint32_t a, uint32_t b) -> int {
        const DrawCmd &x = j.draws[a], &y = j.draws[b];
        if (x.pipeKey != y.pipeKey) return x.pipeKey < y.pipeKey ? -1 : 1;
        if (TexKey(x.pTex) != TexKey(y.pTex)) return TexKey(x.pTex) < TexKey(y.pTex) ? -1 : 1;
        if (x.samplerKey != y.samplerKey) return x.samplerKey < y.samplerKey ? -1 : 1;
        if (int c = memcmp(&x.scissor, &y.scissor, sizeof(SDL_Rect))) return c;
        return memcmp(&j.meshParams[x.meshParams].second, &j.meshParams[y.meshParams].second, sizeof(MeshFSParams));
    };
    // appends draw r to the stream (and its constants to a new slot); continues the last item
    // when `merge` and the slot still fits its window
    // Item::window is the window's byte offset in drawData (aligned for glBindBufferRange)
    // Draw slots are allocated in windows of batchSlots; consecutive items share a window (one
    // BindBufferRange per window, not per item). Everything a batched draw's shaders read
    // is in its slot (only the paletted path still reads its parameter block), so the items
    // of a window rebind nothing but program, state and texture.
    size_t curWindow = ~(size_t)0;
    uint64_t indexNs = 0;                       // time spent in BuildIndexed by this job
    auto emitBatched = [&](uint32_t di, bool merge) {
        const DrawCmd &r = j.draws[di];
        size_t at = L.drawUsed;
        if (curWindow == ~(size_t)0 || (at * 4 - curWindow) / kSlotBytes >= g->batchSlots) {
            size_t aligned = (at * 4 + g->uboAlign - 1) / g->uboAlign * g->uboAlign;
            drawRoom(aligned / 4 - at);
            L.drawUsed = at = aligned / 4;
            curWindow = aligned;
            merge = false;                      // an item draws from one window
        }
        if (!merge || items.empty() || items.back().kind != 2) {
            uint32_t block = j.meshParams[r.meshParams].second.p.mode == 2 ? meshBlockFor(r) : kAnyBlock;
            items.push_back({di, block, (uint32_t)L.sindices.size(), 0, (uint32_t)curWindow, 2, (uint32_t)L.sverts.size(), 0});
        }
        Item &it = items.back();
        uint32_t slot = (uint32_t)((at * 4 - it.window) / kSlotBytes);
        const auto &mp = j.meshParams[r.meshParams];
        drawRoom(kSlotBytes / 4);
        L.drawUsed = at + kSlotBytes / 4;
        memcpy(&L.drawData[at], mp.first.v, sizeof(mp.first.v));
        L.drawData[at + 3] = (float)mp.first.kind;        // fog.w
        L.drawData[at + 8 * 4 + 3] = r.pTex && r.pTex->arr ? (float)r.pTex->layer : 0.0f;   // lightLocal[0].w
        // fragment parameters (lightLocal[1].w, [2].w): fog colour as a 24-bit integer, alpha reference
        const MeshFSParams &mf = mp.second;
        uint32_t fog24 = (uint32_t)lroundf(mf.fogR * 255.0f) | (uint32_t)lroundf(mf.fogG * 255.0f) << 8 |
                         (uint32_t)lroundf(mf.fogB * 255.0f) << 16;
        L.drawData[at + 9 * 4 + 3] = (float)fog24;
        L.drawData[at + 10 * 4 + 3] = (float)lroundf(mf.p.alphaRef * 255.0f);
        size_t vat = L.sverts.size(), iat = L.sindices.size();
        CGraphicMesh *m = r.pMesh;
        // meshes not indexed yet: a few ms per job (a scenery change brings thousands of
        // models; until indexed they are drawn as they are)
        if (!r.strip && !m->indexed && r.firstVertex == 0 && r.vertexCount == m->staging.size() && indexNs < 2000000) {
            uint64_t x0 = SDL_GetTicksNS();
            BuildIndexed(m);
            indexNs += SDL_GetTicksNS() - x0;
        }
        if (!r.strip && r.firstVertex == 0 && r.vertexCount == m->staging.size() && !m->unique.empty()) {
            // the whole model (CVertex draws whole models): its unique vertices, indexed
            L.sverts.insert(L.sverts.end(), m->unique.begin(), m->unique.end());
            L.sindices.resize(iat + m->indices.size());
            uint32_t *dst = &L.sindices[iat];
            uint32_t base = (uint32_t)vat;
            for (uint16_t ix : m->indices) *dst++ = base + ix;
        } else if (!r.strip) {
            const MeshVertex *src = &r.pMesh->staging[r.firstVertex];
            L.sverts.resize(vat + r.vertexCount);
            for (uint32_t k = 0; k < r.vertexCount; k++) L.sverts[vat + k] = Pack(src[k]);
        } else {
            size_t total = 0;
            for (uint32_t k = 0; k < r.strips; k++) total += (size_t)(j.stripRuns[r.stripFirst + k].second - 2) * 3;
            L.sverts.resize(vat + total);
            PreVertex *dst = &L.sverts[vat];
            for (uint32_t k = 0; k < r.strips; k++) {
                const auto &run = j.stripRuns[r.stripFirst + k];
                const MeshVertex *src = &r.pMesh->staging[run.first];
                for (uint32_t i = 0; i + 2 < run.second; i++) {     // keep winding and the last (provoking) vertex
                    *dst++ = Pack(src[(i & 1) ? i + 1 : i]);
                    *dst++ = Pack(src[(i & 1) ? i : i + 1]);
                    *dst++ = Pack(src[i + 2]);
                }
            }
        }
        size_t added = L.sverts.size() - vat;
        L.sslots.resize(vat + added, (uint8_t)slot);
        if (L.sindices.size() == iat) {            // not indexed above: the vertices in order
            L.sindices.resize(iat + added);
            for (size_t k = 0; k < added; k++) L.sindices[iat + k] = (uint32_t)(vat + k);
        }
        it.count += (uint32_t)(L.sindices.size() - iat);
        it.vlast = (uint32_t)L.sverts.size() - 1;  // an item's vertices are contiguous
    };
    const FSParams *last2D = nullptr;
    uint32_t last2DBlock = 0;
    std::vector<uint32_t> run;
    struct SortKey { uint64_t k1, k2; uint32_t idx; };
    std::vector<SortKey> sortKeys;
    size_t lastBatched = ~(size_t)0;   // draw index emitted last, when it was batched (in-order merging)
    for (size_t i = 0; i < j.draws.size();) {
        const DrawCmd &d = j.draws[i];
        if (d.pMesh && d.pre && sortable(d)) {
            run.clear();
            size_t e = i;
            while (e < j.draws.size() && j.draws[e].pMesh && sortable(j.draws[e])) run.push_back((uint32_t)e++);
            // sort by a precomputed key (program and pipeline key, then a hash of the rest); ties keep the
            // recorded order. Merging below still compares exactly, so a hash collision only
            // costs a merge.
            sortKeys.clear();
            for (uint32_t di : run) {
                const DrawCmd &r = j.draws[di];
                uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t)TexKey(r.pTex);
                auto mix = [&](uint64_t v) { h ^= v; h *= 0xff51afd7ed558ccdull; h ^= h >> 32; };
                mix(r.samplerKey);
                uint64_t sc[2]; memcpy(sc, &r.scissor, sizeof(sc)); mix(sc[0]); mix(sc[1]);
                const uint8_t *fp = reinterpret_cast<const uint8_t *>(&j.meshParams[r.meshParams].second);
                for (size_t o = 0; o + 8 <= sizeof(MeshFSParams); o += 8) { uint64_t w; memcpy(&w, fp + o, 8); mix(w); }
                // program first (the most expensive switch), then the fixed-function state
                const MeshFSParams &mf = j.meshParams[r.meshParams].second;
                uint64_t prog = ProgramKey(true, (r.pipeKey >> 23) & 1, mf.p, mf.fogOn, true);
                sortKeys.push_back({prog << 32 | (r.pipeKey & 0xffffffffull), h, di});
            }
            std::sort(sortKeys.begin(), sortKeys.end(), [](const SortKey &a, const SortKey &b) {
                return a.k1 != b.k1 ? a.k1 < b.k1 : a.k2 != b.k2 ? a.k2 < b.k2 : a.idx < b.idx;
            });
            for (size_t k = 0; k < run.size(); k++) run[k] = sortKeys[k].idx;
            for (size_t k = 0; k < run.size(); k++) {
                const DrawCmd &r = j.draws[run[k]];
                L.meshDraws++; L.meshVerts += r.vertexCount;
                emitBatched(run[k], k > 0 && cmpKey(run[k - 1], run[k]) == 0);
            }
            lastBatched = ~(size_t)0;
            i = e;
            continue;
        }
        if (d.pMesh) { L.meshDraws++; L.meshVerts += d.vertexCount; }
        if (d.pMesh && d.pre) {
            emitBatched((uint32_t)i, lastBatched + 1 == i && cmpKey((uint32_t)lastBatched, (uint32_t)i) == 0);
            lastBatched = i;
            i++;
            continue;
        }
        lastBatched = ~(size_t)0;
        if (d.pMesh) {
            items.push_back({(uint32_t)i, meshBlockFor(d), d.firstVertex, d.vertexCount, 0, 1});
        } else {
            if (!last2D || memcmp(last2D, &d.fs, sizeof(FSParams))) {
                ParamBlock *b = NewBlock(L.blockData, last2DBlock);
                FillBlockFS(*b, d.fs);
                b->target[0] = (float)rt.nWidth;
                b->target[1] = (float)rt.nHeight;
                last2D = &d.fs;
            }
            // consecutive triangle lists that differ only in arrayed textures of one array
            // (the layer is per vertex) continue the previous item
            if (!items.empty() && items.back().kind == 0 && (d.pipeKey & 7) == TRIANGLELIST) {
                Item &pi = items.back();
                const DrawCmd &pd = j.draws[pi.draw];
                if (pi.block == last2DBlock && pd.pipeKey == d.pipeKey && TexKey(pd.pTex) == TexKey(d.pTex) &&
                    pd.samplerKey == d.samplerKey && pi.first + pi.count == d.firstVertex &&
                    !memcmp(&pd.scissor, &d.scissor, sizeof(SDL_Rect))) {
                    pi.count += d.vertexCount;
                    i++;
                    continue;
                }
            }
            items.push_back({(uint32_t)i, last2DBlock, d.firstVertex, d.vertexCount, 0, 0});
        }
        i++;
    }
    if (L.blockData.empty()) { uint32_t idx; NewBlock(L.blockData, idx); }    // something to bind for kAnyBlock
    if (!L.sverts.empty()) drawRoom(g->batchSlots * kSlotBytes / 4);     // the last window's full binding range
    L.ins = rt_thread_instructions() - ins0;
    L.ns = SDL_GetTicksNS() - tl0;
    L.built = true;
}

// The GL work of a job: uploads, then the recorded draws in order. Render thread.
static void Execute(Job &j)
{
    uint64_t t0 = SDL_GetTicksNS();
    uint32_t vbytes = (uint32_t)(j.verts.size() * sizeof(Vertex));
    bool haveWork = !j.draws.empty() || j.clearColor || j.clearDepth;

    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // freed textures this job does not draw with go to the pool before the uploads, which can then
    // reuse them (a scenery change frees the old scenery's textures and uploads the new one's)
    if (g->threaded && !j.deadTex.empty()) {
        for (CGraphicTexture *t : j.deadTex) t->dying = true;        // their pending uploads are dropped
        for (const DrawCmd &d : j.draws) if (d.pTex) d.pTex->inUse = true;
        if (j.target) j.target->inUse = true;
        for (CGraphicTexture *t : j.deadTex) if (!t->inUse && !t->bRenderTarget) ReleaseGLTexture(t);
        for (const DrawCmd &d : j.draws) if (d.pTex) d.pTex->inUse = false;
        if (j.target) j.target->inUse = false;
    }
    uint64_t tUp0 = SDL_GetTicksNS();
    g_upAllocNs = g_upDataNs = g_upMipNs = 0;
    g_poolReused = 0;
    ApplyUploads(j);
    UploadTextures(j);
    uint64_t upNs = SDL_GetTicksNS() - tUp0;
    g_nsTex += upNs;
    gl.BindVertexArray(g->vaoMesh);
    gl.BindBuffer(GL_ARRAY_BUFFER, g->arenaVB);
    if (j.newArenaCap)
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)j.newArenaCap * sizeof(MeshVertex), nullptr, GL_STATIC_DRAW);
    for (CGraphicMesh *m : j.meshUploads)
        gl.BufferSubData(GL_ARRAY_BUFFER, (GLintptr)m->offset * sizeof(MeshVertex),
                         (GLsizeiptr)(m->staging.size() * sizeof(MeshVertex)), m->staging.data());

    if (!j.palIds.empty()) {
        gl.BindTexture(GL_TEXTURE_2D, g->paletteTex);
        for (size_t k = 0; k < j.palIds.size(); k++) {
            uint32_t id = j.palIds[k], px = (id % 16) * 256, py = id / 16;
            gl.TexSubImage2D(GL_TEXTURE_2D, 0, px, py, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, &j.palData[k * 256 * 4]);
        }
    }

    if (haveWork) {
        CGraphicTexture &rt = *j.target;
        AcquireTarget(&rt);
        gl.BindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
        gl.Viewport(0, 0, rt.nWidth, rt.nHeight);
        if (j.clearColor || j.clearDepth) {
            gl.Disable(GL_SCISSOR_TEST);
            gl.ColorMask(1, 1, 1, 1);
            gl.DepthMask(1);
            GLbitfield bits = 0;
            if (j.clearColor) {
                gl.ClearColor(j.clearColorV[0], j.clearColorV[1], j.clearColorV[2], j.clearColorV[3]);
                bits |= GL_COLOR_BUFFER_BIT;
            }
            if (j.clearDepth) { gl.ClearDepthf(j.clearDepthV); bits |= GL_DEPTH_BUFFER_BIT; }
            gl.Clear(bits);
        }
        InvalidateState();                          // clears and Present changed state directly
        if (vbytes) {
            // 2D draws with arrayed textures: their layer (known now that the textures are uploaded)
            for (const DrawCmd &d : j.draws) {
                if (d.pMesh || !d.pTex || !d.pTex->arr) continue;
                float layer = d.pTex->layer;
                for (uint32_t k = 0; k < d.vertexCount; k++) j.verts[d.firstVertex + k].layer = layer;
            }
            gl.BindVertexArray(g->vao2D);
            gl.BindBuffer(GL_ARRAY_BUFFER, g->vb2D);
            if (vbytes > g->vbCapacity) {
                g->vbCapacity = SDL_max(vbytes, g->vbCapacity * 2);
                gl.BufferData(GL_ARRAY_BUFFER, g->vbCapacity, nullptr, GL_STREAM_DRAW);
            }
            gl.BufferSubData(GL_ARRAY_BUFFER, 0, vbytes, j.verts.data());
        }
        // the draw list: built by the list thread already when it runs (see ListMain)
        Job::List &L = j.list;
        if (!L.built) BuildList(j);
        g_insList += L.ins; g_nsList += L.ns;
        g->listFrameNs += L.ns;
        g_traceMeshDraws += L.meshDraws; g_traceMeshVerts += L.meshVerts;
        const std::vector<ListItem> &items = L.items;
        uint64_t ins1 = rt_thread_instructions(), tl1 = SDL_GetTicksNS();
        gl.BindBuffer(GL_UNIFORM_BUFFER, g->ubo);
        gl.BufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)L.blockData.size(), L.blockData.data(), GL_STREAM_DRAW);
        if (!L.sverts.empty()) {
            size_t pad = g->batchSlots * kSlotBytes / 4;    // the last window's full binding range (room made by BuildList)
            gl.BindBuffer(GL_UNIFORM_BUFFER, g->uboDraws);
            gl.BufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)((L.drawUsed + pad) * 4), L.drawData.data(), GL_STREAM_DRAW);
            gl.BindBuffer(GL_ARRAY_BUFFER, g->preVB);
            gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(L.sverts.size() * sizeof(PreVertex)), L.sverts.data(), GL_STREAM_DRAW);
            gl.BindBuffer(GL_ARRAY_BUFFER, g->slotVB);
            gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)L.sslots.size(), L.sslots.data(), GL_STREAM_DRAW);
            gl.BindVertexArray(g->vaoPre);              // the element buffer binding is vertex array state
            gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->indexVB);
            gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(L.sindices.size() * 4), L.sindices.data(), GL_STREAM_DRAW);
        }
        uint64_t ins2 = rt_thread_instructions(), tl2 = SDL_GetTicksNS();
        g_insUpload += ins2 - ins1;
        g_nsUpload += tl2 - tl1;
        gl.Enable(GL_SCISSOR_TEST);
        gl.ActiveTexture(GL_TEXTURE0 + 1);
        gl.BindTexture(GL_TEXTURE_2D, g->paletteTex);
        gl.BindSampler(1, GetSampler(SamplerKey(TADDRESS_CLAMP, TADDRESS_CLAMP, false)));
        gl.ActiveTexture(GL_TEXTURE0);

        GLuint boundProg = ~0u;
        uint64_t lastKey = ~0ull;
        GLuint lastTex = ~0u, lastTexArray = ~0u, lastSampler = ~0u;
        uint32_t lastBlock = ~0u;
        bool haveScissor = false, haveMeshVp = false;
        SDL_Rect lastScissor = {}, lastMeshVp = {};
        int lastKind = -1;
        uint32_t lastWindow = ~0u;
        for (const ListItem &it : items) {
            const DrawCmd &d = j.draws[it.draw];
            bool mesh = it.kind != 0;
            const FSParams &fs = mesh ? j.meshParams[d.meshParams].second.p : d.fs;
            bool fog = mesh && j.meshParams[d.meshParams].second.fogOn;
            if (g_exp & 0x78) {                         // RECOMP_GL_EXP: leave out a class of draws (GPU cost split)
                bool blended = (d.pipeKey >> 3) & 1, tested = fs.alphaTest && fs.alphaFunc != CMP_ALWAYS;
                if (((g_exp & 8) && !mesh) || ((g_exp & 16) && mesh && blended) || ((g_exp & 32) && mesh && tested) ||
                    ((g_exp & 64) && mesh && !blended && !tested)) continue;
            }
            if ((g_exp & 0x180) && !mesh) {             // 128: full-screen 2D quads only, 256: the other 2D draws
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (uint32_t k = 0; k < (uint32_t)it.count; k++) {
                    const Vertex &w = j.verts[it.first + k];
                    x0 = SDL_min(x0, w.x); y0 = SDL_min(y0, w.y); x1 = SDL_max(x1, w.x); y1 = SDL_max(y1, w.y);
                }
                bool full = (x1 - x0) * (y1 - y0) >= 0.9f * rt.nWidth * rt.nHeight;
                if (((g_exp & 128) && full) || ((g_exp & 256) && !full)) continue;
            }
            GLuint prog = GetProgram(ProgramKey(mesh, (d.pipeKey >> 23) & 1, fs, fog, it.kind == 2));
            if (!prog || CACHED_ENV("RECOMP_GL_NOSUBMIT")) continue;   // diagnostics: skip GL draw work
            if (prog != boundProg) { gl.UseProgram(prog); boundProg = prog; g_cProg++; }
            if (mesh) g_cItemsMesh++; else g_cItems2D++;
            if (it.block != lastBlock && (it.block != kAnyBlock || lastBlock == ~0u)) {
                g_cBlock++;
                lastBlock = it.block == kAnyBlock ? 0 : it.block;
                gl.BindBufferRange(GL_UNIFORM_BUFFER, 0, g->ubo, (GLintptr)lastBlock * g->uboStride, sizeof(ParamBlock));
            }
            if (d.pipeKey != lastKey) { ApplyState(d.pipeKey); lastKey = d.pipeKey; g_stats.pipelineBinds++; g_cState++; }
            if (!haveScissor || memcmp(&d.scissor, &lastScissor, sizeof(SDL_Rect))) {
                g_cScissor++;
                if (g_exp & 2) gl.Scissor(0, 0, 1, 1); else
                GLViewportRect(d.scissor, rt.nHeight, true);
                lastScissor = d.scissor;
                haveScissor = true;
            }
            if (fs.mode != 0) {                         // untextured variants sample nothing
                if (fs.mode == 3) {
                    if (!d.pTex || !d.pTex->arr) continue;     // never uploaded
                    if (d.pTex->arr->id != lastTexArray) {
                        gl.BindTexture(GL_TEXTURE_2D_ARRAY, d.pTex->arr->id);
                        lastTexArray = d.pTex->arr->id;
                        g_cTex++;
                    }
                } else {
                    GLuint texId = !d.pTex ? g->white->id : d.pTex->id ? d.pTex->id : d.pTex->standIn;
                    if (texId != lastTex) { gl.BindTexture(GL_TEXTURE_2D, texId); lastTex = texId; g_cTex++; }
                }
                GLuint smp = GetSampler(d.samplerKey);
                if (smp != lastSampler) { gl.BindSampler(0, smp); lastSampler = smp; g_cSampler++; }
            }
            if (it.kind != lastKind) {
                g_cVao++;
                gl.BindVertexArray(it.kind == 0 ? g->vao2D : it.kind == 1 ? g->vaoMesh : g->vaoPre);
                if (!mesh) gl.Viewport(0, 0, rt.nWidth, rt.nHeight);
                haveMeshVp = false;
                lastKind = it.kind;
            }
            if (mesh && (!haveMeshVp || memcmp(&d.scissor, &lastMeshVp, sizeof(SDL_Rect)))) {
                GLViewportRect(d.scissor, rt.nHeight, false);       // D3D viewport = the current viewport
                lastMeshVp = d.scissor;
                haveMeshVp = true;
            }
            if (it.kind == 2) {
                if (it.window != lastWindow) {
                    g_cWindow++;
                    lastWindow = it.window;
                    gl.BindBufferRange(GL_UNIFORM_BUFFER, 1, g->uboDraws, (GLintptr)it.window, g->batchSlots * kSlotBytes);
                }
                gl.DrawRangeElements(GL_TRIANGLES, it.vfirst, it.vlast, it.count, GL_UNSIGNED_INT, (const void *)((uintptr_t)it.first * 4));
            } else {
                gl.DrawArrays(ToTopology(d.pipeKey & 7), it.first, it.count);
            }
            g_traceDraws++;
            if (d.pipeKey & (1ull << 3)) g_traceBlended++;
            if (fs.alphaTest && fs.alphaFunc != CMP_ALWAYS) g_traceAlphaTested++;
        }
        g_insSubmit += rt_thread_instructions() - ins2;
        g_nsSubmit += SDL_GetTicksNS() - tl2;
        gl.Disable(GL_SCISSOR_TEST);
    }

    // dead textures leave the mipmap queue first
    for (CGraphicTexture *t : j.deadTex) if (t->mipPending) t->mipPending = false;
    if (!j.deadTex.empty() && !g->pendingMips.empty()) {
        auto &pm = g->pendingMips;
        for (CGraphicTexture *t : j.deadTex) t->dying = true;
        pm.erase(std::remove_if(pm.begin(), pm.end(), [](CGraphicTexture *t) { return t->dying; }), pm.end());
    }
    if (g->threaded) GenerateMipmaps();
    uint64_t tFree0 = SDL_GetTicksNS();
    for (CGraphicMesh *m : j.deadMeshes) delete m;
    bool dropPending = false;
    for (CGraphicTexture *t : j.deadTex) {
        t->dying = true;
        dropPending |= t->pendingUpload;
        ReleaseGLTexture(t);
        ReleaseStandIn(t);
        if (t->fbo) gl.DeleteFramebuffers(1, &t->fbo);
    }
    if (dropPending) {                              // uploads that will never be needed
        auto &pend = g->pendingUploads;
        pend.erase(std::remove_if(pend.begin(), pend.end(), [](const Job::TexUpload &u) { return u.t->dying; }), pend.end());
    }
    for (CGraphicTexture *t : j.deadTex) delete t;
    // GL texture deletes are spread out like uploads (hundreds at a scenery change cost tens of
    // ms on Mali): at most 64 per job, at once unthreaded
    size_t ndel = g->threaded ? SDL_min(g->deleteIds.size(), (size_t)64) : g->deleteIds.size();
    if (ndel) {
        gl.DeleteTextures((GLsizei)ndel, g->deleteIds.data());
        g->deleteIds.erase(g->deleteIds.begin(), g->deleteIds.begin() + (long)ndel);
    }
    uint64_t freeNs = SDL_GetTicksNS() - tFree0;
    // RECOMP_TRACE_SPIKES=<ms>: GL jobs that take longer (uploads at scenery changes)
    static const double spikeMs = getenv("RECOMP_TRACE_SPIKES") ? atof(getenv("RECOMP_TRACE_SPIKES")) : 0;
    if (spikeMs > 0) {
        double ms = (SDL_GetTicksNS() - t0) / 1e6;
        if (ms > spikeMs)
            SDL_Log("GraphicGL: slow GL job %.1f ms: %zu draws, %zu texture uploads (%.1f ms: create %.1f with %u reused, "
                    "data %.1f, mipmaps %.1f; unlocked since the last job: %u uploaded now, %u before, %u waiting, %u new "
                    "stand-ins: %.1f ms thumbnails, %.1f ms GL, %zu in the pool), %zu textures and %zu models freed (%.1f ms)", ms, j.draws.size(), j.texUploads.size() + g_upNow,
                    upNs / 1e6, g_upAllocNs / 1e6, g_poolReused, g_upDataNs / 1e6, g_upMipNs / 1e6, g_upNow, g_upEarly, g_upHeld,
                    g_upStandIn, g_standInCpuNs / 1e6, g_standInGlNs / 1e6, g->standInPool.size(),
                    j.deadTex.size(), j.deadMeshes.size(), freeNs / 1e6);
    }
    if (j.present) {
        g->upFrameNs = 0;
        static uint64_t lastPresent;
        uint64_t now = SDL_GetTicksNS();
        if (lastPresent && now - lastPresent < 200000000)
            g->presentIntervalNs = (g->presentIntervalNs * 7 + (now - lastPresent)) / 8;
        lastPresent = now;
        if (j.snapshot) SaveScene(*j.scene);
        CheckGL("drawing a frame", (int)g_frameSerial);
        uint64_t t1 = SDL_GetTicksNS();
        if (g->asyncPresent) HandOff(*j.scene, j.gamma);
        else Present(*j.scene, j.gamma);
        g->flushNs += t1 - t0;
        TraceFrame(g->flushNs, SDL_GetTicksNS() - t1);
        g->frameLoadNs = SDL_max(g->flushNs + g->gpuWaitFrameNs, g->listFrameNs);
        g->flushNs = g->listFrameNs = g->gpuWaitFrameNs = 0;
    } else {
        g->flushNs += SDL_GetTicksNS() - t0;
    }
}

static void Enqueue(Job *j)
{
    {
        std::lock_guard<std::mutex> lk(g->queueLock);
        g->queue.push_back(j);
        if (j->present) { g->handoverNs[g->presentsQueued % 4] = SDL_GetTicksNS(); g->presentsQueued++; }
    }
    g->queueCv.notify_all();
}

// The list thread: builds the draw lists of queued jobs (BuildList), in order, while the render
// thread is still drawing the previous one. Per frame on the R36H at draw distance 1.00 the render
// thread needed ~8 ms for the list, ~2 for uploads and ~9 for submitting (mostly in the driver,
// ~20 us per draw call): over a 60 Hz frame together, not apart. Not with texture arrays (their
// layers are assigned by the render thread's uploads). RECOMP_GL_LISTTHREAD=0 turns it off.
static void ListMain()
{
    NameThread("gl-list");
    std::unique_lock<std::mutex> lk(g->queueLock);
    for (;;) {
        Job *j = nullptr;
        g->queueCv.wait(lk, [&] {
            for (Job *q : g->queue) if (q->listState == 0) { j = q; return true; }
            return g->quit;
        });
        if (!j) break;
        j->listState = 1;
        lk.unlock();
        if (!j->draws.empty() || j->clearColor || j->clearDepth) BuildList(*j);
        lk.lock();
        j->listState = 2;
        g->queueCv.notify_all();
    }
}

// The render thread: GL setup, then jobs in order until Term().
static void RenderMain()
{
    NameThread("gl-render");
    bool ok = SetupGL();
    {
        std::lock_guard<std::mutex> lk(g->queueLock);
        g->setupDone = true;
        g->setupOk = ok;
        // started before any job is taken: jobs are built by one thread only (BuildIndexed)
        const char *lt = getenv("RECOMP_GL_LISTTHREAD");
        g->listThreaded = ok && !g->texArrays && !(lt && atoi(lt) == 0);
        if (g->listThreaded) g->listThread = std::thread(ListMain);
    }
    g->queueCv.notify_all();
    for (;;) {
        Job *j = nullptr;
        UploadReq *r = nullptr;
        bool makeStandIn = false;
        {
            auto standIns = [ok] { return ok && g->earlyUploads && g->standInsMade < kStandIns; };
            std::unique_lock<std::mutex> lk(g->queueLock);
            // with the list thread, the next job is taken once its draw list is built
            auto jobReady = [] { return !g->queue.empty() && (!g->listThreaded || g->queue.front()->listState == 2); };
            g->queueCv.wait(lk, [&] { return jobReady() || !g->upQueue.empty() || (g->quit && g->queue.empty()) || standIns(); });
            if (jobReady()) {
                j = g->queue.front();
                g->queue.pop_front();
            } else if (g->quit && g->queue.empty()) {
                break;
            } else if (!g->upQueue.empty()) {       // no job waiting: a texture upload
                r = g->upQueue.front();
                g->upQueue.pop_front();
            } else {
                makeStandIn = true;
            }
        }
        if (makeStandIn) {
            g->standInPool.push_back(NewStandIn());
            g->standInsMade++;
            continue;
        }
        if (r) {
            if (ok && !r->done) {
                uint64_t u0 = SDL_GetTicksNS();
                DoUpload(r, false);
                g->upIdleNs += SDL_GetTicksNS() - u0;
                g->upIdleCount++;
            }
            Unref(r);
            continue;
        }
        if (ok) {
            Execute(*j);
        } else {                                    // no GL: only free what the job would have
            for (CGraphicMesh *m : j->deadMeshes) delete m;
            for (CGraphicTexture *t : j->deadTex) delete t;
        }
        bool present = j->present;
        // test hook: RECOMP_GL_SLOW=<ms> per frame imitates a slow GPU (frame skipping on the desktop)
        static const int slow = getenv("RECOMP_GL_SLOW") ? atoi(getenv("RECOMP_GL_SLOW")) : 0;
        if (present && slow > 0) SDL_Delay((uint32_t)slow);
        // test hook: RECOMP_GL_SLOW_ONCE=<s>:<ms> delays one frame after s seconds (a scenery change)
        static const char *once = getenv("RECOMP_GL_SLOW_ONCE");
        static const uint64_t startNs = SDL_GetTicksNS();
        if (present && once && (SDL_GetTicksNS() - startNs) / 1e9 >= atof(once)) {
            SDL_Log("GraphicGL: RECOMP_GL_SLOW_ONCE: delaying a frame by %d ms", atoi(strchr(once, ':') + 1));
            SDL_Delay((uint32_t)atoi(strchr(once, ':') + 1));
            once = nullptr;
        }
        ReleaseJob(j);
        if (present) {
            uint64_t now = SDL_GetTicksNS(), start = SDL_max((uint64_t)g->handoverNs[g->presentsDone % 4], (uint64_t)g->lastPresentNs);
            uint64_t d = now - start, a = g->avgFrameNs;
            g->avgFrameNs = a ? (a * 7 + d) / 8 : d;
            g->lastPresentNs = now;
            g->presentsDone++;
        }
        g->queueCv.notify_all();
    }
    if (g->warmThread.joinable()) g->warmThread.join();
    if (g->presentThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(g->presentLock);
            g->presentQuit = true;
        }
        g->presentCv.notify_all();
        g->presentThread.join();
    }
    SDL_GL_MakeCurrent(g->asyncPresent ? nullptr : g->pWindow, nullptr);
}

static bool Flush()
{
    std::lock_guard<RecLock> lock(g->lock);
    if (g->threaded) {
        if (Job *j = Capture(false)) Enqueue(j);
        return true;
    }
    if (!OnRenderThread()) return true;             // GL work waits for the render thread
    if (Job *j = Capture(false)) {
        Execute(*j);
        ReleaseJob(j);
    }
    return true;
}

static void FillTexture(DrawCmd &d)
{
    State &s = g->st;
    d.fs.alphaTest = s.alphaTest;
    d.fs.alphaFunc = s.alphaFunc;
    d.fs.alphaRef = (s.alphaRef & 0xff) / 255.0f;
    d.fs.colorOp = s.colorOp;
    d.fs.alphaOp = s.alphaOp;
    if (g->textured && g->pCurTex) {
        CGraphicTexture &t = *g->pCurTex;
        bool linear = s.magFilter == TEXF_LINEAR || s.minFilter == TEXF_LINEAR;
        d.pTex = &t;
        d.samplerKey = SamplerKey(s.addrU, s.addrV, linear);
        d.fs.mode = t.nFormat == TEXFMT_P8 ? 2 : t.arrayed ? 3 : 1;
        d.fs.addrU = s.addrU;
        d.fs.addrV = s.addrV;
        d.fs.linear = linear;
        d.fs.texW = t.arrayed ? 0 : t.nWidth;      // (the paletted path's texel addressing only)
        d.fs.texH = t.arrayed ? 0 : t.nHeight;
        d.fs.flipV = t.bRenderTarget;
        uint32_t pal = g->curPalette < CGraphic::kNoPalette ? g->curPalette : 0;
        d.fs.palX = (pal % 16) * 256;
        d.fs.palY = pal / 16;
    } else {
        d.pTex = nullptr;
        d.samplerKey = SamplerKey(TADDRESS_CLAMP, TADDRESS_CLAMP, false);
        d.fs.mode = 0;
    }
}

// HUD sprites: about 3 screens of textured, blended 2D quads per frame while driving, 61% of
// their texels fully transparent (Osaka Loop). A quad whose transparent texels cannot change a
// pixel (source weighted by alpha, alpha taken from the texture) is cropped to the box of the
// texels with alpha > 0 plus one texel (bilinear filtering), its texture coordinates mapped as
// before, so the pixels it still covers get the same values. Boxes are cached per texture region
// and texture version. Returns false when nothing in the region is visible (draw dropped).
// RECOMP_GL_CROPQUADS=0 disables it.
static bool CropQuad(Vertex *v, CGraphicTexture &t)
{
    float x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0;
    for (int i = 1; i < 6; i++) {
        if (v[i].z != v[0].z || v[i].rhw != v[0].rhw || v[i].color != v[0].color) return true;
        x0 = SDL_min(x0, v[i].x); x1 = SDL_max(x1, v[i].x); y0 = SDL_min(y0, v[i].y); y1 = SDL_max(y1, v[i].y);
    }
    if (x1 - x0 < 8 || y1 - y0 < 8) return true;            // small sprites: not worth it
    // an axis-aligned rectangle whose u depends only on x and v only on y
    float uX[2] = {NAN, NAN}, vY[2] = {NAN, NAN};
    for (int i = 0; i < 6; i++) {
        int ix = v[i].x == x0 ? 0 : v[i].x == x1 ? 1 : -1, iy = v[i].y == y0 ? 0 : v[i].y == y1 ? 1 : -1;
        if (ix < 0 || iy < 0) return true;
        if (std::isnan(uX[ix])) uX[ix] = v[i].u; else if (uX[ix] != v[i].u) return true;
        if (std::isnan(vY[iy])) vY[iy] = v[i].v; else if (vY[iy] != v[i].v) return true;
    }
    if (std::isnan(uX[0]) || std::isnan(uX[1]) || std::isnan(vY[0]) || std::isnan(vY[1]) || uX[0] == uX[1] || vY[0] == vY[1]) return true;
    float tu0 = SDL_min(uX[0], uX[1]) * t.nUVWidth, tu1 = SDL_max(uX[0], uX[1]) * t.nUVWidth;
    float tv0 = SDL_min(vY[0], vY[1]) * t.nUVHeight, tv1 = SDL_max(vY[0], vY[1]) * t.nUVHeight;
    if (tu0 < 0 || tv0 < 0 || tu1 > t.nWidth || tv1 > t.nHeight) return true;   // repeats or beyond the texels
    int ix0 = (int)floorf(tu0), ix1 = (int)ceilf(tu1), iy0 = (int)floorf(tv0), iy1 = (int)ceilf(tv1);
    struct Box { int x0, y0, x1, y1; };
    static std::map<std::tuple<uint64_t, uint32_t, int, int, int, int>, Box> cache;
    auto key = std::make_tuple(t.uid, t.contentGen, ix0, iy0, ix1, iy1);
    auto it = cache.find(key);
    Box b;
    if (it != cache.end()) {
        b = it->second;
    } else {
        b = {1 << 30, 1 << 30, -1, -1};
        for (int y = iy0; y < iy1; y++) {
            const uint8_t *row = &t.staging[((size_t)y * t.nWidth) * 4 + 3];
            for (int x = ix0; x < ix1; x++)
                if (row[(size_t)x * 4]) { b.x0 = SDL_min(b.x0, x); b.x1 = SDL_max(b.x1, x + 1); b.y0 = SDL_min(b.y0, y); b.y1 = SDL_max(b.y1, y + 1); }
        }
        if (cache.size() > 8192) cache.clear();
        cache[key] = b;
    }
    if (b.x1 < 0) return false;                                // all transparent
    float nu0 = SDL_max(tu0, b.x0 - 1.0f), nu1 = SDL_min(tu1, b.x1 + 1.0f);
    float nv0 = SDL_max(tv0, b.y0 - 1.0f), nv1 = SDL_min(tv1, b.y1 + 1.0f);
    if ((nu1 - nu0) * (nv1 - nv0) > 0.9f * (tu1 - tu0) * (tv1 - tv0)) return true;
    // texel coordinate -> screen coordinate along each axis (u may run against x)
    auto xOfU = [&](float tu) { return x0 + (tu / t.nUVWidth - uX[0]) / (uX[1] - uX[0]) * (x1 - x0); };
    auto yOfV = [&](float tv) { return y0 + (tv / t.nUVHeight - vY[0]) / (vY[1] - vY[0]) * (y1 - y0); };
    float nx0 = SDL_min(xOfU(nu0), xOfU(nu1)), nx1 = SDL_max(xOfU(nu0), xOfU(nu1));
    float ny0 = SDL_min(yOfV(nv0), yOfV(nv1)), ny1 = SDL_max(yOfV(nv0), yOfV(nv1));
    nx0 = SDL_max(nx0, x0); nx1 = SDL_min(nx1, x1); ny0 = SDL_max(ny0, y0); ny1 = SDL_min(ny1, y1);
    auto uOfX = [&](float x) { return uX[0] + (x - x0) / (x1 - x0) * (uX[1] - uX[0]); };
    auto vOfY = [&](float y) { return vY[0] + (y - y0) / (y1 - y0) * (vY[1] - vY[0]); };
    for (int i = 0; i < 6; i++) {
        float nx = v[i].x == x0 ? nx0 : nx1, ny = v[i].y == y0 ? ny0 : ny1;
        v[i].x = nx; v[i].u = uOfX(nx);
        v[i].y = ny; v[i].v = vOfY(ny);
    }
    return true;
}

static bool Record(const Vertex *v, uint32_t n)
{
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
    static const bool cropQuads = !getenv("RECOMP_GL_CROPQUADS") || atoi(getenv("RECOMP_GL_CROPQUADS")) != 0;
    if (cropQuads && d.pTex && topo == TRIANGLELIST && d.vertexCount == 6 && s.blendEnable && s.srcBlend == BLEND_SRCALPHA &&
        (s.dstBlend == BLEND_INVSRCALPHA || s.dstBlend == BLEND_ONE) &&
        (s.alphaOp == TOP_SELECTARG1 || s.alphaOp == TOP_MODULATE || s.alphaOp == TOP_MODULATE2X) &&
        !d.pTex->bRenderTarget && !d.pTex->arrayed && d.pTex->nFormat == TEXFMT_BGRA8 &&
        d.pTex->staging.size() >= (size_t)d.pTex->nWidth * d.pTex->nHeight * 4) {
        if (!CropQuad(&g->verts[d.firstVertex], *d.pTex)) {      // nothing visible
            g->verts.resize(d.firstVertex);
            return true;
        }
    }
    if (topo == TRIANGLELIST && !g->draws.empty()) {
        DrawCmd &last = g->draws.back();
        if (!last.pMesh && last.pipeKey == d.pipeKey && last.pTex == d.pTex && last.samplerKey == d.samplerKey &&
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

static CGraphicTexture *NewRenderTarget(uint32_t w, uint32_t h)
{
    auto *t = new CGraphicTexture();
    t->nWidth = t->nUVWidth = w;
    t->nHeight = t->nUVHeight = h;
    t->nFormat = TEXFMT_BGRA8;
    t->bRenderTarget = true;
    return t;
}

// ---------------------------------------------------------------------------------------------
// Setup / teardown


bool CGraphic::LoadConfig(int argc, char **argv)
{
    const char *override = getenv("RECOMP_PREF_DIR");
    char *pref = override ? SDL_strdup((std::string(override) + "/").c_str()) : SDL_GetPrefPath("TAITO", "DengoFinal");
    if (pref) {
        std::string path = std::string(pref) + "graphics.ini";
        g_prefDir = pref;
        SDL_free(pref);
        if (FILE *f = fopen(path.c_str(), "r")) {
            struct { const char *name; int *value; } keys[] = {
                {"windowed", &m_Config.windowed}, {"gpu", &m_Config.gpu}, {"scrwidth", &m_Config.scrwidth},
                {"scrheight", &m_Config.scrheight}, {"bbcount", &m_Config.bbcount}, {"scrformat", &m_Config.scrformat},
                {"refrate", &m_Config.refrate}, {"zformat", &m_Config.zformat}, {"texformat", &m_Config.texformat},
                {"texmodulate", &m_Config.texmodulate}, {"fog", &m_Config.fog}, {"texsize", &m_Config.texsize},
                {"screffect", &m_Config.screffect}, {"snapshot", &m_Config.snapshot},
                {"crop", &m_Config.crop}, {"batching", &g_cfgBatching}, {"fastalpha", &g_cfgFastAlpha},
                {"mipmaps", &g_cfgMipmaps}, {"vsync", &g_cfgVsync}, {"threaded", &g_cfgThreaded}, {"tex16", &g_cfgTex16}, {"fps", &g_cfgFps}, {"standins", &g_cfgStandIns},
                {"texarrays", &g_cfgTexArrays},
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

// The window needs SDL_WINDOW_OPENGL and the context attributes set before it is created (host/native.cpp).
bool CGraphic::Init(SDL_Window *window)
{
    g = new Impl();
    g->pWindow = window;
    g->batching = g_cfgBatching != 0 && !getenv("RECOMP_NO_BATCHING");
    g->texArrays = g_cfgTexArrays != 0 && !getenv("RECOMP_GL_NO_TEXARRAYS");
    const char *th = getenv("RECOMP_GL_THREADED");                     // overrides graphics.ini
    g->threaded = th ? *th != '0' : g_cfgThreaded != 0;
    const char *eu = getenv("RECOMP_GL_EARLY_UPLOADS");
    g->earlyUploads = g->threaded && !g->texArrays && (!eu || *eu != '0');
    if (const char *ub = getenv("RECOMP_GL_UPLOAD_BUDGET")) g->upBudgetNs = (uint64_t)(atof(ub) * 1e6);
    g->tex.assign(kMaxTextures, nullptr);
    g->texPalette.assign(kMaxTextures, kNoPalette);
    g->managedTex.assign(kMaxTextures, nullptr);
    g->managedPalette.assign(kMaxTextures, kNoPalette);
    g->paletteCPU.assign((size_t)kPaletteAtlasW * kPaletteAtlasH * 4, 0);
    g->paletteDirty.assign(kNoPalette, false);
    for (uint32_t i = kNoPalette; i-- > 0;) g->freePalettes.push_back(i);

    int profile = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
    g->gles = profile == SDL_GL_CONTEXT_PROFILE_ES;
    g->ctx = SDL_GL_CreateContext(window);
    if (!g->ctx) return FAIL("SDL_GL_CreateContext");
    // threaded: a second context sharing objects with the first, for the render thread when the
    // present thread owns the window's context (asyncPresent; RECOMP_GL_ASYNC_PRESENT=0 disables)
    const char *ap = getenv("RECOMP_GL_ASYNC_PRESENT");
    if (g->threaded && (!ap || *ap != '0')) {
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
        g->renderCtx = SDL_GL_CreateContext(window);
        if (!g->renderCtx) SDL_Log("GraphicGL: no shared context: %s", SDL_GetError());
        else g->warmCtx = SDL_GL_CreateContext(window);     // shader warm-up (same share group)
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    }
    SDL_GL_MakeCurrent(window, nullptr);            // made current on the render/present thread later
    if (!m_Config.windowed) SDL_SetWindowFullscreen(window, true);

    m_nTexFormat = m_Config.texformat == 0 ? TEXFMT_P8 : TEXFMT_BGRA8;
    static const uint32_t sizes[4] = {256, 512, 1024, 1024};
    g->maxTexW = g->maxTexH = sizes[SDL_clamp(m_Config.texsize, 0, 3)];
    g->st.colorOp = GetTextureModulate();

    uint32_t w = (uint32_t)m_Config.scrwidth, h = (uint32_t)m_Config.scrheight;
    g->tex[0] = NewRenderTarget(w, h);
    g->tex[1] = NewRenderTarget(w, h);
    g->tex[0]->rtIndex = 0;
    g->tex[1]->rtIndex = 1;
    g->white = CreateTexture(1, 1, TEXFMT_BGRA8);
    g->white->staging = {0xff, 0xff, 0xff, 0xff};
    g->pendingClearColor = g->pendingClearDepth = true;

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
    if (!g) return;
    if (g->thread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(g->queueLock);
            g->quit = true;
        }
        g->queueCv.notify_all();
        g->thread.join();
        if (g->listThread.joinable()) g->listThread.join();
    }
    if (g->warmCtx) SDL_GL_DestroyContext(g->warmCtx);
    if (g->renderCtx) SDL_GL_DestroyContext(g->renderCtx);
    if (g->ctx) SDL_GL_DestroyContext(g->ctx);
    delete g;
    g = nullptr;
}

bool CGraphic::Reset() { return true; }

// ---------------------------------------------------------------------------------------------
// Frame

CGraphic::DebugStats &CGraphic::GetDebugStats() { g_stats.pipelines = 0; return g_stats; }
uint32_t CGraphic::GetFrameSerial() { return g_frameSerial; }

void CGraphic::HoldRecordingLock(bool hold)
{
    if (!g) return;
    if (hold) g->lock.lock();
    else g->lock.unlock();
}

bool CGraphic::BeginFrame()
{
    g_frameSerial++;
    if (g->threaded) {
        if (!g->thread.joinable()) {
            g->thread = std::thread(RenderMain);
            std::unique_lock<std::mutex> lk(g->queueLock);
            g->queueCv.wait(lk, [] { return g->setupDone; });
        }
        if (!g->setupOk) return false;
    } else {
        if (!SetupGL()) return false;
        if (!OnRenderThread()) {
            static bool warned;
            if (!warned) { warned = true; SDL_Log("GraphicGL: frame begun on another thread than the first one"); }
        }
    }
    if (!SetRenderTargetTexture(0)) return false;
    // a real depth clear (tiled GPUs then need not load the old depth; it is discarded at present)
    g->pendingClearDepth = true;
    g->clearDepth = 1.0f;
    if (!ClearBorders()) return false;
    return SetViewport(2, true);
}

// "fps = 1": frames presented per second (over half-second windows), as white 3x5-pixel digits on
// a black box in the top-left corner, drawn with the present shader from a small texture.
static void DrawFpsCounter(PresentRes &r, int sh)
{
    static const uint16_t kGlyphs[13] = {      // 0-9, F, P, S: 5 rows of 3 bits, top row first
        0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF, 0x79E4, 0x7BE4, 0x79CF};
    constexpr int kW = 32, kH = 7;                  // texture: 1-pixel border, up to 7 glyphs of 4 pixels
    static uint64_t windowStart, frames;
    static int shown = -1, usedW = kW;
    uint64_t now = SDL_GetTicksNS();
    if (!windowStart) windowStart = now;
    frames++;
    int fps = shown;
    if (now - windowStart >= 500000000ull) {
        fps = (int)(frames * 1e9 / (double)(now - windowStart) + 0.5);
        windowStart = now;
        frames = 0;
    }
    if (!r.fpsTex) {
        gl.GenTextures(1, &r.fpsTex);
        gl.BindTexture(GL_TEXTURE_2D, r.fpsTex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        fps = SDL_max(fps, 0);
        shown = -1;
    }
    gl.BindTexture(GL_TEXTURE_2D, r.fpsTex);
    if (fps != shown) {
        shown = fps;
        char text[8];
        int len = SDL_snprintf(text, sizeof(text), "%dFPS", SDL_clamp(fps, 0, 999));
        usedW = 1 + SDL_min(len, 7) * 4;
        uint8_t px[kH][kW][4];
        memset(px, 0, sizeof(px));
        for (int y = 0; y < kH; y++) for (int x = 0; x < kW; x++) px[y][x][3] = 255;
        for (int c = 0; text[c] && c < 7; c++) {
            int gi = text[c] >= '0' && text[c] <= '9' ? text[c] - '0' : text[c] == 'F' ? 10 : text[c] == 'P' ? 11 : 12;
            for (int row = 0; row < 5; row++) for (int b = 0; b < 3; b++)
                if (kGlyphs[gi] >> (14 - row * 3 - b) & 1)
                    memset(px[kH - 2 - row][1 + c * 4 + b], 255, 4);    // GL rows bottom-up
        }
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    int scale = SDL_max(2, sh / 160);               // 3 at 480 lines
    gl.Viewport(scale * 2, sh - (kH + 2) * scale, usedW * scale, kH * scale);
    float pp[4] = {1.0f, 0.0f, 0.0f, (float)usedW / kW};
    gl.Uniform4fv(r.uPresent, 1, pp);
    gl.Uniform1f(r.uScaleY, 1.0f);
    gl.Uniform1f(r.uLutOn, 0.0f);
    float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    gl.Uniform4fv(r.uClamp, 1, full);
    gl.BindSampler(0, r.samplerNearest);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

// CGraphic::SetOverlay: the image (rows top-down) and its version, drawn by DrawOverlay
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

bool CGraphic::HasFpsCounter() { return true; }
bool CGraphic::FpsCounter() { return g_cfgFps != 0; }
void CGraphic::SetFpsCounter(bool bOn) { g_cfgFps = bOn; }

// The overlay, centred at the largest integer scale that leaves a margin, nearest-sampled.
static void DrawOverlay(PresentRes &r, int sw, int sh)
{
    std::vector<uint8_t> px;
    int w, h;
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_overlay.lock);
        if (!g_overlay.w) return;
        w = g_overlay.w;
        h = g_overlay.h;
        changed = !r.overlayTex || r.overlayVersion != g_overlay.version;
        if (changed) px = g_overlay.px;
        r.overlayVersion = g_overlay.version;
    }
    if (!r.overlayTex) {
        gl.GenTextures(1, &r.overlayTex);
        gl.BindTexture(GL_TEXTURE_2D, r.overlayTex);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    gl.BindTexture(GL_TEXTURE_2D, r.overlayTex);
    if (changed) {                                  // GL rows bottom-up
        std::vector<uint8_t> flipped(px.size());
        for (int y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    }
    int scale = SDL_max(1, SDL_min(sw * 15 / 16 / w, sh * 15 / 16 / h));
    gl.Viewport((sw - w * scale) / 2, (sh - h * scale) / 2, w * scale, h * scale);
    float pp[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    gl.Uniform4fv(r.uPresent, 1, pp);
    gl.Uniform1f(r.uScaleY, 1.0f);
    gl.Uniform1f(r.uLutOn, 0.0f);
    float full[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    gl.Uniform4fv(r.uClamp, 1, full);
    gl.BindSampler(0, r.samplerNearest);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

// Draws the scene texture into the window (scaled, letterboxed, gamma) in the current context,
// whose default framebuffer is the window. The caller swaps.
static void BlitToWindow(PresentRes &r, GLuint sceneTex, uint32_t sceneW, uint32_t sceneH, float gamma)
{
    int sw = 0, sh = 0;
    SDL_GetWindowSizeInPixels(g->pWindow, &sw, &sh);
    float w = (float)sceneW, h = (float)sceneH;
    float u0 = 0, v0 = 0, us = 1, vs = 1, aspect = w / h;
    if (CGraphic::m_Config.crop) { u0 = 20.0f / 640.0f; v0 = 30.0f / 480.0f; us = 600.0f / 640.0f; vs = 420.0f / 480.0f; }
    float vw = SDL_min((float)sw, (float)sh * aspect), vh = vw / aspect;
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Disable(GL_SCISSOR_TEST);
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.ColorMask(1, 1, 1, 1);
    gl.Viewport(0, 0, sw, sh);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.Viewport((int)((sw - vw) * 0.5f), (int)((sh - vh) * 0.5f), (int)vw, (int)vh);
    gl.UseProgram(r.prog);
    float pp[4] = {1.0f / (gamma > 0 ? gamma : 1.0f), u0, v0, us};
    gl.Uniform4fv(r.uPresent, 1, pp);
    gl.Uniform1f(r.uScaleY, vs);
    // sampling stays half a texel inside the shown part: bilinear filtering at the screen edge
    // would otherwise mix in the overscan margin (stripes at the edges with crop)
    float hu = 0.5f / w, hv = 0.5f / h, clampUV[4] = {u0 + hu, v0 + hv, u0 + us - hu, v0 + vs - hv};
    gl.Uniform4fv(r.uClamp, 1, clampUV);
    bool lutOn = gamma > 0 && gamma != 1.0f;
    gl.Uniform1f(r.uLutOn, lutOn ? 1.0f : 0.0f);
    gl.ActiveTexture(GL_TEXTURE0 + 2);
    gl.BindTexture(GL_TEXTURE_2D, r.lut);
    gl.BindSampler(2, r.samplerLinear);
    if (lutOn && gamma != r.lutGamma) {             // (re)build the gamma table
        uint8_t t[256];
        for (int i = 0; i < 256; i++) t[i] = (uint8_t)SDL_clamp(lroundf(255.0f * powf(i / 255.0f, 1.0f / gamma)), 0, 255);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RED, GL_UNSIGNED_BYTE, t);
        r.lutGamma = gamma;
    }
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, sceneTex);
    gl.BindSampler(0, r.samplerLinear);
    gl.BindVertexArray(r.vao);
    if (!(g_exp & 4)) gl.DrawArrays(GL_TRIANGLES, 0, 3);
    if (g_cfgFps) DrawFpsCounter(r, sh);
    DrawOverlay(r, sw, sh);
    // test hook: RECOMP_SHOT_WINDOW=<seconds> saves the final window image once (snapshot/window.bmp)
    static const char *shotWin = getenv("RECOMP_SHOT_WINDOW");
    static uint64_t start = SDL_GetTicksNS();
    if (shotWin && (SDL_GetTicksNS() - start) / 1e9 >= atof(shotWin)) {
        shotWin = nullptr;
        std::vector<uint8_t> px((size_t)sw * sh * 4), flipped(px.size());
        gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gl.ReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < sh; y++) memcpy(&flipped[(size_t)y * sw * 4], &px[(size_t)(sh - 1 - y) * sw * 4], (size_t)sw * 4);
        if (SDL_Surface *ws = SDL_CreateSurfaceFrom(sw, sh, SDL_PIXELFORMAT_RGBX32, flipped.data(), sw * 4)) {
            SDL_CreateDirectory("snapshot");
            SDL_SaveBMP(ws, "snapshot/window.bmp");
            SDL_DestroySurface(ws);
        }
    }
}

static void InvalidateDepth(CGraphicTexture &scene)
{
    if (glInvalidateFramebufferPtr) {               // the scene's depth is not needed any more
        const GLenum depth = GL_DEPTH_ATTACHMENT;
        gl.BindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
        glInvalidateFramebufferPtr(GL_FRAMEBUFFER, 1, &depth);
    }
}

// Presenting on the render thread (no asyncPresent): blit and swap here.
static void Present(CGraphicTexture &scene, float gamma)
{
    uint64_t tp0 = SDL_GetTicksNS();
    InvalidateDepth(scene);
    BlitToWindow(g->inlinePresent, scene.id, scene.nWidth, scene.nHeight, gamma);
    // RECOMP_TRACE_TICKS: every 30th frame, wait for the GPU before the swap, which splits "swap"
    // into GPU time and waiting for the display
    static const bool trace = getenv("RECOMP_TRACE_TICKS") != nullptr;
    static uint32_t presents;
    if (trace && ++presents % 30 == 0) {
        uint64_t f0 = SDL_GetTicksNS();
        gl.Finish();
        g_gpuSampleNs += SDL_GetTicksNS() - f0;
        g_gpuSamples++;
    }
    uint64_t tp1 = SDL_GetTicksNS();
    SDL_GL_SwapWindow(g->pWindow);
    g_nsPresent += tp1 - tp0;
    g_nsSwapOnly += SDL_GetTicksNS() - tp1;
    g->presentsShown++;
}

// asyncPresent: the scene is complete in GL's command stream. Hand it to the present thread
// (replacing a scene it has not taken yet: only the newest is worth showing) and keep the render
// thread at most one frame ahead of the GPU.
static void HandOff(CGraphicTexture &scene, float gamma)
{
    uint64_t tp0 = SDL_GetTicksNS();
    InvalidateDepth(scene);
    // Display pacing: until the present thread has taken the previous scene (and issued its blit),
    // this one stays unflushed in the driver. Otherwise this scene's GPU work goes ahead of the
    // previous scene's blit, the swap misses its vblank and finished scenes are dropped.
    {
        std::unique_lock<std::mutex> lk(g->presentLock);
        g->presentCv.wait_for(lk, std::chrono::milliseconds(100), [] { return !g->presentReq.valid && g->presentBusy < 0; });
    }
    uint64_t tp1 = SDL_GetTicksNS();
    g->paceWaitNs += tp1 - tp0;
    GLsync ready = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLsync done = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.Flush();                                     // the fences reach the GPU (and the other context sees them)
    {
        std::lock_guard<std::mutex> lk(g->presentLock);
        if (g->presentReq.valid) { gl.DeleteSync(g->presentReq.ready); g->presentsDropped++; }
        g->presentReq = {scene.id, scene.nWidth, scene.nHeight, scene.rtIndex, gamma, ready, true};
    }
    g->presentCv.notify_all();
    // GPU backpressure: wait until the GPU has finished the previous frame. Without it the render
    // thread runs ahead of a slower GPU and the driver queues frames (latency), or blocks somewhere
    // less predictable.
    uint64_t w0 = SDL_GetTicksNS();
    if (g->frameFence) {
        gl.ClientWaitSync(g->frameFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        gl.DeleteSync(g->frameFence);
    }
    g->frameFence = done;
    uint64_t w1 = SDL_GetTicksNS();
    g->frameWaitNs += w1 - w0;
    g->gpuWaitFrameNs = w1 - w0;
    g_nsPresent += w0 - tp1;
}

// Before the render thread draws into scene render target t: a scene in it that the present thread
// has not taken yet is dropped (it is being overwritten), one being blitted is waited for, and the
// GPU waits for the last blit from it.
static void AcquireTarget(CGraphicTexture *t)
{
    if (!g->asyncPresent || !t || t->rtIndex < 0) return;
    GLsync rel;
    {
        std::unique_lock<std::mutex> lk(g->presentLock);
        if (g->presentReq.valid && g->presentReq.rt == t->rtIndex) {
            gl.DeleteSync(g->presentReq.ready);
            g->presentReq.valid = false;
            g->presentsDropped++;
        }
        g->presentCv.wait(lk, [t] { return g->presentBusy != t->rtIndex; });
        rel = g->rtReleased[t->rtIndex];
        g->rtReleased[t->rtIndex] = nullptr;
    }
    if (rel) {
        gl.WaitSync(rel, 0, GL_TIMEOUT_IGNORED);
        gl.DeleteSync(rel);
    }
}

// The present thread (asyncPresent): owns the window's context; blits the newest finished scene
// and swaps. The swap blocks (KMS: until the GPU is done and the next vblank) here instead of on the
// render thread.
static void PresentMain()
{
    NameThread("gl-present");
    PresentRes res;
    bool ok = SDL_GL_MakeCurrent(g->pWindow, g->ctx);
    if (!ok) SDL_Log("GraphicGL: present thread: SDL_GL_MakeCurrent: %s", SDL_GetError());
    if (ok) {
        SDL_GL_SetSwapInterval(VsyncSetting());
        ok = CreatePresentRes(res);
        auto queryString = reinterpret_cast<const char *(*)(void *, int)>(SDL_EGL_GetProcAddress("eglQueryString"));
        if (SDL_EGLDisplay dpy = SDL_EGL_GetCurrentDisplay(); dpy && queryString)
            SDL_Log("GraphicGL: EGL extensions: %s", queryString(dpy, 0x3055 /* EGL_EXTENSIONS */));
    }
    {
        std::lock_guard<std::mutex> lk(g->presentLock);
        g->presentSetupDone = true;
        g->presentSetupOk = ok;
    }
    g->presentCv.notify_all();
    if (!ok) { SDL_GL_MakeCurrent(g->pWindow, nullptr); return; }
    for (;;) {
        CGraphic::Impl::PresentReq r;
        {
            std::unique_lock<std::mutex> lk(g->presentLock);
            g->presentCv.wait(lk, [] { return g->presentReq.valid || g->presentQuit; });
            if (!g->presentReq.valid) break;
            r = g->presentReq;
            g->presentReq.valid = false;
            g->presentBusy = r.rt;
        }
        uint64_t t0 = SDL_GetTicksNS();
        // the GPU runs the blit after the scene; the blit is queued at once, so that the render
        // thread can submit the next scene behind it (HandOff)
        gl.WaitSync(r.ready, 0, GL_TIMEOUT_IGNORED);
        gl.DeleteSync(r.ready);
        uint64_t t1 = SDL_GetTicksNS();
        BlitToWindow(res, r.tex, r.w, r.h, r.gamma);
        GLsync rel = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        gl.Flush();
        {
            std::lock_guard<std::mutex> lk(g->presentLock);
            if (r.rt >= 0) {
                if (g->rtReleased[r.rt]) gl.DeleteSync(g->rtReleased[r.rt]);
                g->rtReleased[r.rt] = rel;
            } else {
                gl.DeleteSync(rel);
            }
            g->presentBusy = -1;
        }
        g->presentCv.notify_all();
        uint64_t t2 = SDL_GetTicksNS();
        SDL_GL_SwapWindow(g->pWindow);
        uint64_t t3 = SDL_GetTicksNS();
        g->presentSwapNs += t3 - t0;
        g->presentWaitNs += t1 - t0;
        g->presentBlitNs += t2 - t1;
        g->presentsShown++;
        // RECOMP_TRACE_TICKS / RECOMP_TRACE_SPIKES: the picture standing still (scenery changes)
        static uint64_t lastShown;
        static const double spikeMs = getenv("RECOMP_TRACE_SPIKES") ? atof(getenv("RECOMP_TRACE_SPIKES")) : 0;
        if (lastShown) {
            uint64_t gap = t3 - lastShown;
            if (gap > g->presentGapMaxNs) g->presentGapMaxNs = gap;
            if (gap > 50000000) g->presentHitches++;
            if (spikeMs > 0 && gap > spikeMs * 1e6) SDL_Log("GraphicGL: no new frame for %.1f ms", gap / 1e6);
        }
        lastShown = t3;
    }
    SDL_GL_MakeCurrent(g->pWindow, nullptr);
}

// RECOMP_TRACE_TICKS: every 600 frames, where the render thread's time goes. "flush" is issuing
// GL commands (includes driver CPU work), "swap" mostly waits for the GPU and the display.
static void TraceFrame(uint64_t flushNs, uint64_t swapNs)
{
    static const bool on = getenv("RECOMP_TRACE_TICKS") != nullptr;
    static uint64_t n, fl, sw, swMax, last;
    if (!on) return;
    n++; fl += flushNs; sw += swapNs; if (swapNs > swMax) swMax = swapNs;
    if (n < 600) return;
    uint64_t now = SDL_GetTicksNS();
    SDL_Log("gl: %.1f fps | per frame: flush %.2f ms, swap %.2f ms (max %.1f), of it GPU %.2f ms (sampled) | draw calls %.0f, mesh draws %.0f "
            "with %.0f vertices, blended %.0f, alpha-tested %.0f | Minstr/frame: transform %.2f, list %.2f, upload %.2f, submit %.2f",
            last ? 600e9 / (double)(now - last) : 0.0, fl / 600 / 1e6, sw / 600 / 1e6, swMax / 1e6,
            g_gpuSamples ? g_gpuSampleNs / (double)g_gpuSamples / 1e6 : 0.0,
            g_traceDraws / 600.0, g_traceMeshDraws / 600.0, g_traceMeshVerts / 600.0, g_traceBlended / 600.0,
            g_traceAlphaTested / 600.0, g_insTransform / 600e6, g_insList / 600e6, g_insUpload / 600e6, g_insSubmit / 600e6);
    uint64_t shown = g->presentsShown.exchange(0), dropped = g->presentsDropped.exchange(0);
    uint64_t pswap = g->presentSwapNs.exchange(0), fwait = g->frameWaitNs.exchange(0);
    uint64_t pwait = g->presentWaitNs.exchange(0), pblit = g->presentBlitNs.exchange(0), pace = g->paceWaitNs.exchange(0);
    uint64_t gapMax = g->presentGapMaxNs.exchange(0), hitches = g->presentHitches.exchange(0);
    double ps = shown ? (double)shown * 1e6 : 1.0;
    SDL_Log("gl: ms/frame: textures %.2f, list %.2f, upload %.2f, submit %.2f, present %.2f, swap call %.2f, display wait %.2f, "
            "gpu wait %.2f | shown %.1f/s (%llu dropped, longest gap %.0f ms, %llu over 50 ms), present thread ms/present: "
            "%.2f = sync %.2f, blit %.2f, swap %.2f",
            g_nsTex / 600e6, g_nsList / 600e6, g_nsUpload / 600e6, g_nsSubmit / 600e6, g_nsPresent / 600e6, g_nsSwapOnly / 600e6,
            pace / 600e6, fwait / 600e6, last ? shown * 1e9 / (double)(now - last) : 0.0, (unsigned long long)dropped,
            gapMax / 1e6, (unsigned long long)hitches, pswap / ps, pwait / ps, pblit / ps, (pswap - pwait - pblit) / ps);
    g_nsTex = g_nsList = g_nsUpload = g_nsSubmit = g_nsPresent = g_nsSwapOnly = 0;
    SDL_Log("gl: per frame: items 2D %.0f mesh %.0f | binds: program %.0f, params %.0f, state %.0f, scissor %.0f, texture %.0f, "
            "sampler %.0f, vao %.0f, draw window %.0f",
            g_cItems2D / 600.0, g_cItemsMesh / 600.0, g_cProg / 600.0, g_cBlock / 600.0, g_cState / 600.0, g_cScissor / 600.0,
            g_cTex / 600.0, g_cSampler / 600.0, g_cVao / 600.0, g_cWindow / 600.0);
    SDL_Log("gl: indexed meshes: %llu vertices, %.0f%% unique", (unsigned long long)g->dedupVerts,
            g->dedupVerts ? 100.0 * g->dedupUnique / g->dedupVerts : 0.0);
    if (g->texArrays) SDL_Log("gl: texture arrays: %u (%.1f MB)", g->arrayCount, g->arrayBytes / 1048576.0);
    if (g->earlyUploads) {
        SDL_Log("gl: texture uploads: %llu while idle (%.1f ms), %llu when drawn (%.1f ms)", (unsigned long long)g->upIdleCount,
                g->upIdleNs / 1e6, (unsigned long long)g->upJobCount, g->upJobNs / 1e6);
        g->upIdleCount = g->upIdleNs = g->upJobCount = g->upJobNs = 0;
    }
    g_cProg = g_cBlock = g_cState = g_cScissor = g_cTex = g_cSampler = g_cVao = g_cWindow = g_cItems2D = g_cItemsMesh = 0;
    g_traceDraws = g_traceMeshVerts = g_traceMeshDraws = g_traceBlended = g_traceAlphaTested = 0;
    g_gpuSampleNs = g_gpuSamples = 0;
    g_insTransform = g_insList = g_insUpload = g_insSubmit = 0;
    last = now;
    n = fl = sw = swMax = 0;
}

bool CGraphic::EndFrame(bool bSnapshotKey)
{
    std::lock_guard<RecLock> lock(g->lock);
    if (!g->threaded && !OnRenderThread()) return false;
    Job *j = Capture(true);
    j->present = true;
    j->scene = g->tex[0];
    j->gamma = g->gamma;
    j->snapshot = (m_Config.snapshot && bSnapshotKey) || g->snapshotRequest;
    g->snapshotRequest = false;
    if (g->threaded) {
        uint64_t w0 = SDL_GetTicksNS();
        std::unique_lock<std::mutex> lk(g->queueLock);
        // up to two frames in flight: the render thread draws one while the next waits, so that a
        // frame started shortly before the render thread is done (see FrameBusyNs) does not block
        g->queueCv.wait(lk, [] { return g->presentsQueued - g->presentsDone < 2; });
        lk.unlock();
        g_gfxWaitNs += SDL_GetTicksNS() - w0;
        Enqueue(j);
    } else {
        Execute(*j);
        ReleaseJob(j);
    }
    // the scene targets alternate with the screen effect (it reads the previous frame), and with
    // asyncPresent (the present thread reads the previous frame while the next one is drawn)
    if (m_Config.screffect || g->asyncPresent) std::swap(g->tex[0], g->tex[1]);
    return true;
}

bool CGraphic::Threaded() { return g && g->threaded; }

bool CGraphic::FrameInFlight()
{
    return g && g->threaded && g->presentsDone != g->presentsQueued;
}

uint64_t CGraphic::FrameLoadNs() { return g ? (uint64_t)g->frameLoadNs : 0; }

uint64_t CGraphic::FrameBusyNs()
{
    if (!FrameInFlight()) return 0;
    if (g->presentsQueued - g->presentsDone >= 2) return ~0ull >> 1;   // one already waits
    uint64_t start = SDL_max((uint64_t)g->handoverNs[g->presentsDone % 4], (uint64_t)g->lastPresentNs);
    uint64_t end = start + g->avgFrameNs, now = SDL_GetTicksNS();
    if (end > now) return end - now;
    // overdue: a few ms are normal variation; much longer means a slow frame (a scenery change's
    // uploads) whose end cannot be estimated
    return now - end < 10000000 ? 1 : ~0ull >> 1;
}

void CGraphic::SetDeferredPresent(bool) {}         // GL presents on the render thread
bool CGraphic::PresentPending() { return false; }

// ---------------------------------------------------------------------------------------------
// Textures and palettes

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
    ResTimer rt_(0);
    std::lock_guard<RecLock> lock(g->lock);
    auto *t = new CGraphicTexture();
    t->nFormat = fmt;
    t->arrayed = g->texArrays && fmt == TEXFMT_BGRA8;
    t->nWidth = t->nUVWidth = SDL_max(nWidth, 1u);
    t->nHeight = t->nUVHeight = SDL_max(nHeight, 1u);
    t->staging.assign((size_t)t->nWidth * t->nHeight * BytesPerTexel(fmt), 0);
    MarkDirty(t);
    g_stats.liveTextures++; g_stats.texCreates++;
    return t;
}

void CGraphic::DestroyTexture(CGraphicTexture *pTex)
{
    ResTimer rt_(1);
    if (!pTex) return;
    std::lock_guard<RecLock> lock(g->lock);
    if (pTex->bDirty)
        g->dirtyTex.erase(std::remove(g->dirtyTex.begin(), g->dirtyTex.end(), pTex), g->dirtyTex.end());
    if (g->pCurTex == pTex) g->pCurTex = nullptr;
    for (uint32_t h : pTex->managedSlots) if (g->managedTex[h] == pTex) g->managedTex[h] = nullptr;
    if (pTex->texSlot >= 0 && g->tex[pTex->texSlot] == pTex) g->tex[pTex->texSlot] = nullptr;
    for (int i = 0; i < 2; i++) if (g->tex[i] == pTex) g->tex[i] = nullptr;
    // freed after the draws recorded so far (they go to GL in the same flush, before it)
    g->deadTex.push_back(pTex);
    g_stats.liveTextures--; g_stats.texDestroys++;
}

void *CGraphic::LockTexture(CGraphicTexture *pTex, uint32_t *pPitch)
{
    if (!pTex || pTex->bRenderTarget) return nullptr;
    if (pPitch) *pPitch = pTex->nWidth * BytesPerTexel(pTex->nFormat);
    return pTex->staging.data();
}

static void AnalyzeAlpha(CGraphicTexture *t)
{
    if (t->nFormat != TEXFMT_BGRA8 || t->bRenderTarget) { t->maxPartialAlpha = 255; t->anyTransparent = true; return; }
    uint8_t maxPartial = 0;
    bool any = false;
    const uint8_t *p = t->staging.data(), *end = p + t->staging.size();
    for (p += 3; p < end; p += 4) {
        uint8_t a = *p;
        if (a != 255) { any = true; if (a > maxPartial) maxPartial = a; }
    }
    t->maxPartialAlpha = maxPartial;
    t->anyTransparent = any;
}

void CGraphic::UnlockTexture(CGraphicTexture *pTex)
{
    ResTimer rt_(2);
    if (!pTex || pTex->bRenderTarget) return;
    std::lock_guard<RecLock> lock(g->lock);
    if (!pTex->bDirty) Flush();
    AnalyzeAlpha(pTex);
    pTex->contentGen++;
    MarkDirty(pTex);
    if (g->earlyUploads && !pTex->arrayed) IssueUpload(pTex);
}

void CGraphic::SetTextureUVSize(CGraphicTexture *pTex, uint32_t nWidth, uint32_t nHeight)
{
    if (pTex) { pTex->nUVWidth = nWidth; pTex->nUVHeight = nHeight; }
}

uint32_t CGraphic::AllocPalette()
{
    std::lock_guard<RecLock> lock(g->lock);
    if (g->freePalettes.empty()) return kNoPalette;
    uint32_t id = g->freePalettes.back();
    g->freePalettes.pop_back();
    return id;
}

void CGraphic::FreePalette(uint32_t nPalID)
{
    std::lock_guard<RecLock> lock(g->lock);
    if (nPalID < kNoPalette) g->freePalettes.push_back(nPalID);
}

bool CGraphic::UploadPalette(uint32_t nPalID, const uint8_t *pRGBA, uint32_t nCount)
{
    if (nPalID >= kNoPalette) return false;
    std::lock_guard<RecLock> lock(g->lock);
    if (!g->paletteDirty[nPalID]) Flush();
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
    if (nHandle >= kMaxTextures) return;
    CGraphicTexture *&slot = g->managedTex[nHandle];
    if (slot == pTex) return;
    if (slot) {
        auto &v = slot->managedSlots;
        v.erase(std::remove(v.begin(), v.end(), nHandle), v.end());
    }
    slot = pTex;
    if (pTex) pTex->managedSlots.push_back(nHandle);
}

void CGraphic::SetManagedPalette(uint32_t nHandle, uint32_t nPalID)
{
    if (nHandle < kMaxTextures) g->managedPalette[nHandle] = nPalID;
}

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
    if (nID < 2 || nID >= kMaxTextures) return false;
    CGraphicTexture *&t = g->tex[nID];
    if (TEXSIZE *ts = FindTexSize((int)nID)) {
        if (t) return true;
        nWidth = ts->nWidth;
        nHeight = ts->nHeight;
    }
    nWidth = SDL_min(PowerOfTwo(nWidth), g->maxTexW);
    nHeight = SDL_min(PowerOfTwo(nHeight), g->maxTexH);
    if (t && t->nWidth == nWidth && t->nHeight == nHeight && t->nFormat == m_nTexFormat) return true;
    DestroyTexture(t);
    t = CreateTexture(nWidth, nHeight, m_nTexFormat);
    if (t) t->texSlot = (int32_t)nID;
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
    ResTimer rt_(3);
    if (nID < 2 || nID >= kMaxTextures || w <= 0 || h <= 0) return false;
    if (!LoadTexture(nID, (uint32_t)w, (uint32_t)h)) return false;
    CGraphicTexture *t = g->tex[nID];
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
    g->textured = nID < kMaxTextures;
    if (!g->textured) { g->pCurTex = nullptr; return true; }
    bool managed = nGroup == kGroupManaged;
    g->pCurTex = managed ? g->managedTex[nID] : g->tex[nID];
    g->curPalette = nPalID >= kMaxTextures ? kNoPalette
                  : managed ? g->managedPalette[nPalID] : g->texPalette[nPalID];
    return g->pCurTex != nullptr;
}

// ---------------------------------------------------------------------------------------------
// Immediate-mode 2D drawing (as Graphic.cpp)

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
    if (!g->textured) { g->vertex2D[i] = {pos[0], pos[1], pos[2], 1.0f, color}; return true; }
    if (!g->pCurTex) return false;
    g->texVertex[i] = {pos[0], pos[1], pos[2], 1.0f, color, u, v};
    return true;
}

bool CGraphic::AddStripVertex(int i, D3DCOLOR color, const float *pos, float u, float v)
{
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
    (void)nPrimCount;
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
    std::lock_guard<RecLock> lock(g->lock);
    return Record(v, nVertexCount);
}

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
    case RS_FOGENABLE:        s.fogEnable = value; break;
    case RS_FOGCOLOR:         s.fogColor = value; break;
    case RS_DITHERENABLE: case RS_EDGEANTIALIAS: break;
    }
}

void CGraphic::SetTextureStageState(uint32_t stage, TextureStageState type, uint32_t value)
{
    if (stage != 0) return;
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
    // A full-target quad with the depth test set to always, as in Graphic.cpp.
    if (!(flags & (CLEAR_TARGET | CLEAR_ZBUFFER))) return true;
    std::lock_guard<RecLock> lock(g->lock);
    State saved = g->st;
    bool savedTextured = g->textured;
    int savedVp = g->nViewport;
    PrimitiveType savedPrim = m_nPrimType;
    g->st.blendEnable = 0; g->st.alphaTest = 0; g->st.cull = CULL_NONE; g->st.shade = SHADE_GOURAUD;
    g->st.fogEnable = 0;
    g->st.zEnable = 1; g->st.zFunc = CMP_ALWAYS; g->st.zWrite = (flags & CLEAR_ZBUFFER) ? 1 : 0;
    g->st.colorWrite = (flags & CLEAR_TARGET) ? 15 : 0;
    g->textured = false;
    g->nViewport = -1;
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

bool CGraphic::ClearBorders()
{
    std::lock_guard<RecLock> lock(g->lock);
    State saved = g->st;
    bool savedTextured = g->textured;
    int savedVp = g->nViewport;
    PrimitiveType savedPrim = m_nPrimType;
    g->st.blendEnable = 0; g->st.zEnable = 0; g->st.alphaTest = 0; g->st.cull = CULL_NONE; g->st.shade = SHADE_GOURAUD;
    g->st.colorWrite = 15;
    g->textured = false;
    g->nViewport = -1;
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

// a screenshot of the next presented frame (written on the render thread)
bool CGraphic::PrintScreen()
{
    g->snapshotRequest = true;
    return true;
}

static bool SaveScene(CGraphicTexture &scene)
{
    uint32_t w = scene.nWidth, h = scene.nHeight;
    std::vector<uint8_t> px((size_t)w * h * 4), flipped((size_t)w * h * 4);
    gl.BindFramebuffer(GL_FRAMEBUFFER, scene.fbo);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (uint32_t y = 0; y < h; y++) memcpy(&flipped[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], w * 4);
    bool ok = false;
    if (SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_RGBX32, flipped.data(), (int)w * 4)) {
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
    return ok || FAIL("PrintScreen");
}

// ---------------------------------------------------------------------------------------------
// 3D models (CPU side as Graphic.cpp; the GL buffer follows arenaCap in Flush)

static void ArenaRelease(uint32_t offset, uint32_t count)
{
    auto &fl = g->arenaFree;
    auto next = fl.lower_bound(offset);
    if (next != fl.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) { offset = prev->first; count += prev->second; fl.erase(prev); }
    }
    if (next != fl.end() && offset + count == next->first) { count += next->second; fl.erase(next); }
    fl[offset] = count;
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
        Flush();                                    // recorded draws use the current buffer size
        uint32_t cap = SDL_max(g->arenaCap ? g->arenaCap * 2 : 1u << 18, g->arenaCap + count);
        ArenaRelease(g->arenaCap, cap - g->arenaCap);
        g->arenaCap = cap;
    }
    return false;
}

CGraphicMesh *CGraphic::CreateMesh(const MeshVertex *pVertices, uint32_t nCount)
{
    ResTimer rt_(4);
    if (!nCount) return nullptr;
    std::lock_guard<RecLock> lock(g->lock);
    auto *m = new CGraphicMesh();
    if (!ArenaAlloc(nCount, m->offset)) { delete m; FAIL("mesh arena"); return nullptr; }
    m->staging.assign(pVertices, pVertices + nCount);
    m->index = (uint32_t)g->meshes.size();
    g->meshes.push_back(m);
    g->dirtyMeshes.push_back(m);
    g_stats.liveMeshes++; g_stats.meshCreates++;
    return m;
}

void CGraphic::DestroyMesh(CGraphicMesh *pMesh)
{
    ResTimer rt_(5);
    if (!pMesh) return;
    std::lock_guard<RecLock> lock(g->lock);
    if (pMesh->bDirty)
        g->dirtyMeshes.erase(std::remove(g->dirtyMeshes.begin(), g->dirtyMeshes.end(), pMesh), g->dirtyMeshes.end());
    CGraphicMesh *last = g->meshes.back();          // order does not matter: swap with the last
    g->meshes[pMesh->index] = last;
    last->index = pMesh->index;
    g->meshes.pop_back();
    g->arenaPendingFree.push_back({pMesh->offset, (uint32_t)pMesh->staging.size()});
    g->deadMeshes.push_back(pMesh);                 // recorded or queued work may still read it
    g_stats.liveMeshes--; g_stats.meshDestroys++;
}

// Equality of plain data without a library call (memcmp is not inlined for these sizes; ~2500
// calls per frame on the game thread). `n` is a multiple of 4.
static inline bool SameBytes(const void *a, const void *b, size_t n)
{
    const uint8_t *x = static_cast<const uint8_t *>(a), *y = static_cast<const uint8_t *>(b);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t u, v;
        memcpy(&u, x + i, 8); memcpy(&v, y + i, 8);
        if (u != v) return false;
    }
    for (; i < n; i += 4) {
        uint32_t u, v;
        memcpy(&u, x + i, 4); memcpy(&v, y + i, 4);
        if (u != v) return false;
    }
    return true;
}

// MeshConstants: the model's matrix first (differs from the previous model's almost always)
static inline bool SameConstants(const MeshConstants &a, const MeshConstants &b)
{
    return SameBytes(a.localToScreen, b.localToScreen, sizeof(a.localToScreen)) && SameBytes(&a, &b, sizeof(MeshConstants));
}

// DrawMesh with g->lock held. `sameCall`: an earlier strip of the same DrawMeshStrips call came
// before, so state, texture and constants are known to equal lastStrip's (no comparison needed).
// `trusted`: the range is known to fit the mesh (DrawMeshStrips: CVertex built the strips from it),
// so the mesh itself is not read (a cache miss per model on the R36H).
static bool DrawMeshLocked(CGraphicMesh *pMesh, MeshShader shader, PrimitiveType type, uint32_t nFirst,
                           uint32_t nPrimCount, uint32_t nCull, const MeshConstants &c, bool sameCall,
                           bool trusted = false);

bool CGraphic::DrawMesh(CGraphicMesh *pMesh, MeshShader shader, PrimitiveType type, uint32_t nFirst,
                        uint32_t nPrimCount, uint32_t nCull, const MeshConstants &c)
{
    std::lock_guard<RecLock> lock(g->lock);
    return DrawMeshLocked(pMesh, shader, type, nFirst, nPrimCount, nCull, c, false);
}

// The strips of one model: one lock and one state comparison instead of one per strip (a model
// has a few to a few dozen strips, and a frame thousands of models).
void CGraphic::DrawMeshStrips(CGraphicMesh *pMesh, MeshShader shader, const uint32_t *prims, const uint32_t *culls,
                              size_t n, const MeshConstants &c)
{
    std::lock_guard<RecLock> lock(g->lock);
    uint32_t start = 0;
    bool same = false;
    for (size_t i = 0; i < n; i++) {
        // a strip under 3 vertices records nothing, so lastStrip may not describe this call yet
        bool drawn = DrawMeshLocked(pMesh, shader, TRIANGLESTRIP, start, prims[i], culls[i], c, same, g->batching);
        same = same || (drawn && g->batching && prims[i] + 2 >= 3 && g->lastStrip.mesh == pMesh);
        start += prims[i] + 2;
    }
}

static bool DrawMeshLocked(CGraphicMesh *pMesh, MeshShader shader, PrimitiveType type, uint32_t nFirst,
                           uint32_t nPrimCount, uint32_t nCull, const MeshConstants &c, bool sameCall, bool trusted)
{
    if (!pMesh || !nPrimCount) return false;
    uint32_t count = type == TRIANGLESTRIP ? nPrimCount + 2 : nPrimCount * 3;
    if (!trusted && nFirst + count > pMesh->staging.size()) return false;
    auto &ls = g->lastStrip;
    if (g->batching && type == TRIANGLESTRIP && !g->draws.empty() && g->draws.back().strip &&
        ls.mesh == pMesh && ls.end == nFirst && ls.cull == nCull &&
        (sameCall || (ls.shader == (int)shader && ls.viewport == g->nViewport && ls.textured == g->textured &&
                      ls.tex == g->pCurTex && ls.palette == g->curPalette && SameConstants(ls.c, c) &&
                      SameBytes(&ls.st, &g->st, sizeof(State))))) {
        DrawCmd &last = g->draws.back();
        if (count >= 3) { g->stripRuns.push_back({nFirst, count}); last.strips++; last.vertexCount += count; }
        ls.end = nFirst + count;
        g_stats.meshDraws++;
        g_stats.mergedDraws++;
        return true;
    }

    DrawCmd d = {};
    d.pMesh = pMesh;
    d.firstVertex = g->batching ? nFirst : pMesh->offset + nFirst;   // batching: into pMesh->staging (below)
    d.vertexCount = count;
    int vpIndex = g->nViewport >= 0 && g->nViewport < 2 ? g->nViewport : 0;
    d.scissor = g->viewport[vpIndex];
    State s = g->st;
    s.cull = nCull;
    PrimitiveType topo = type;
    if (g->batching) {                  // vertices are copied from pMesh->staging in Flush
        d.pre = true;
        d.strip = type == TRIANGLESTRIP;
        d.firstVertex = nFirst;
        topo = TRIANGLELIST;            // strips are unrolled
        if (d.strip && count < 3) return true;
        if (d.strip) {
            d.stripFirst = (uint32_t)g->stripRuns.size();
            d.strips = 1;
            g->stripRuns.push_back({nFirst, count});
            ls.mesh = pMesh; ls.end = nFirst + count; ls.shader = (int)shader; ls.cull = nCull;
            ls.viewport = g->nViewport; ls.textured = g->textured; ls.tex = g->pCurTex; ls.palette = g->curPalette;
            ls.st = g->st; ls.c = c;
        }
    }
    FillTexture(d);
    // fastalpha: over-blending with an alpha test at >= ref changes nothing when every texel that
    // passes is opaque (and the vertex alpha is 1): drop the blend, and the test for opaque textures.
    // Mali GPUs then reject hidden pixels early. (Filtered edges of cut-out textures get harder.)
    if (g_cfgFastAlpha && d.pTex && !d.pTex->bRenderTarget && d.fs.mode == 1 && s.blendEnable &&
        s.srcBlend == BLEND_SRCALPHA && s.dstBlend == BLEND_INVSRCALPHA && s.blendOp == BLENDOP_ADD &&
        d.fs.alphaTest && d.fs.alphaFunc == CMP_GREATEREQUAL && (d.fs.alphaOp == TOP_MODULATE || d.fs.alphaOp == TOP_SELECTARG1) &&
        c.baseColor[3] >= 1.0f && d.pTex->maxPartialAlpha < (uint32_t)lroundf(d.fs.alphaRef * 255.0f)) {
        s.blendEnable = 0;
        if (!d.pTex->anyTransparent) d.fs.alphaTest = 0;
    }
    d.pipeKey = PipeKey(topo, s) | kKeyMesh;

    // the parameter set is written in place and dropped again when it repeats the previous one
    auto &mps = g->meshParams;
    mps.emplace_back();
    MeshVSParams &vs = mps.back().first;
    memcpy(vs.v[0], c.fog, 16);
    memcpy(vs.v[1], c.lightColor, 48);
    memcpy(vs.v[4], c.localToScreen, 64);
    memcpy(vs.v[8], c.lightLocal, 48);
    memcpy(vs.v[11], c.baseColor, 16);
    memcpy(vs.v[12], c.stzOffset, 16);
    vs.kind = (int32_t)shader;
    MeshFSParams &fs = mps.back().second;
    fs.p = d.fs;
    fs.fogOn = s.fogEnable && CGraphic::m_Config.fog ? 1 : 0;
    fs.fogR = ((s.fogColor >> 16) & 0xff) / 255.0f;
    fs.fogG = ((s.fogColor >> 8) & 0xff) / 255.0f;
    fs.fogB = (s.fogColor & 0xff) / 255.0f;
    size_t np = mps.size();
    if (np >= 2 && SameBytes(mps[np - 2].first.v[4], vs.v[4], 64) && SameBytes(&mps[np - 2].first, &vs, sizeof(vs)) &&
        SameBytes(&mps[np - 2].second, &fs, sizeof(fs))) {
        mps.pop_back();
        d.meshParams = (uint32_t)np - 2;
    } else {
        d.meshParams = (uint32_t)np - 1;
        g_stats.meshParamBlocks++;
    }
    g_stats.meshDraws++;
    // continue the previous draw when it is the same in everything but the (adjacent) vertex range
    if (topo == TRIANGLELIST && !g->draws.empty()) {
        DrawCmd &last = g->draws.back();
        if (last.pMesh && last.pre == d.pre && !last.strip && !d.strip && (!d.pre || last.pMesh == d.pMesh) &&
            last.meshParams == d.meshParams && last.pipeKey == d.pipeKey && last.pTex == d.pTex &&
            last.samplerKey == d.samplerKey && last.firstVertex + last.vertexCount == d.firstVertex &&
            !memcmp(&last.scissor, &d.scissor, sizeof(SDL_Rect))) {
            last.vertexCount += d.vertexCount;
            g_stats.mergedDraws++;
            return true;
        }
    }
    g->draws.push_back(d);
    return true;
}

int CGraphic::DebugCompareManagedTexture(uint32_t, const char *) { return -1; }
