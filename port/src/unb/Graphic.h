// Graphic.h - CGraphic on SDL3 GPU (Metal / Vulkan / D3D12)
//
// Replacement for unb/Graphic.cpp in Densha de GO! FINAL (PC). The original drove Direct3D 8;
// addresses in comments refer to perfect.exe.
//
// The public interface follows the original so the rest of the unb layer (the PS2 GS emulator
// at 0x40d1b0, CTexture, the frame begin/end hooks) can be ported with small changes:
//   - Draw calls are recorded and submitted in batches instead of issued immediately.
//   - The GS emulator's direct device calls go through SetRenderState/SetTextureStageState,
//     which keep the Direct3D 8 numbering so the ported code can pass the same values.
//   - CTexture writes texels through LockTexture/UnlockTexture instead of IDirect3DTexture8::LockRect.
//   - BeginScene/EndScene/Present (0x40f030/0x40f090 in the original) become BeginFrame/EndFrame.
//   - The scene is always rendered off screen at scrwidth x scrheight and scaled to the window,
//     which replaces the original m_bScaleScreen handling and makes PrintScreen and gamma simple.

#pragma once
#include <SDL3/SDL.h>
#include <cstdint>

// Direct3D 8 values still used by callers. Only the subset the game uses is listed.
namespace d3d {
enum PrimitiveType : uint32_t { POINTLIST = 1, LINELIST = 2, LINESTRIP = 3, TRIANGLELIST = 4, TRIANGLESTRIP = 5, TRIANGLEFAN = 6 };

enum RenderState : uint32_t {
    RS_ZENABLE = 7, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19,
    RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25,
    RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_FOGCOLOR = 34, RS_EDGEANTIALIAS = 40,
    RS_COLORWRITEENABLE = 168,     // bit mask: 1 red, 2 green, 4 blue, 8 alpha
    RS_BLENDOP = 171,
};
enum ShadeMode : uint32_t { SHADE_FLAT = 1, SHADE_GOURAUD = 2 };
enum Cull : uint32_t { CULL_NONE = 1, CULL_CW = 2, CULL_CCW = 3 };
enum Cmp : uint32_t { CMP_NEVER = 1, CMP_LESS, CMP_EQUAL, CMP_LESSEQUAL, CMP_GREATER, CMP_NOTEQUAL, CMP_GREATEREQUAL, CMP_ALWAYS };
enum Blend : uint32_t {
    BLEND_ZERO = 1, BLEND_ONE, BLEND_SRCCOLOR, BLEND_INVSRCCOLOR, BLEND_SRCALPHA, BLEND_INVSRCALPHA,
    BLEND_DESTALPHA, BLEND_INVDESTALPHA, BLEND_DESTCOLOR, BLEND_INVDESTCOLOR, BLEND_SRCALPHASAT,
};
enum BlendOp : uint32_t { BLENDOP_ADD = 1, BLENDOP_SUBTRACT, BLENDOP_REVSUBTRACT, BLENDOP_MIN, BLENDOP_MAX };

enum TextureStageState : uint32_t { TSS_COLOROP = 1, TSS_ALPHAOP = 4, TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_MAGFILTER = 16, TSS_MINFILTER = 17 };
enum TextureOp : uint32_t { TOP_DISABLE = 1, TOP_SELECTARG1 = 2, TOP_SELECTARG2 = 3, TOP_MODULATE = 4, TOP_MODULATE2X = 5 };
enum TextureAddress : uint32_t { TADDRESS_WRAP = 1, TADDRESS_MIRROR = 2, TADDRESS_CLAMP = 3 };
enum TextureFilter : uint32_t { TEXF_POINT = 1, TEXF_LINEAR = 2 };

enum ClearFlags : uint32_t { CLEAR_TARGET = 1, CLEAR_ZBUFFER = 2 };
}

typedef uint32_t D3DCOLOR;     // 0xAARRGGBB, as in the original vertex data

// Texture slot formats. P8 is kept from the PS2 data path: 8-bit indices plus a 256-entry palette,
// looked up in the fragment shader because modern GPUs have no paletted textures.
enum TexFormat : uint32_t { TEXFMT_P8 = 0, TEXFMT_BGRA8 = 1 };

struct TEXSIZE { int nID; uint16_t nWidth; uint16_t nHeight; };

struct CGraphicTexture;     // a GPU texture owned by CGraphic (opaque to callers)
struct CGraphicMesh;        // a GPU vertex buffer of MeshVertex (opaque to callers)

// Model vertex as built by CVertex (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1, 32 bytes).
struct MeshVertex { float x, y, z, nx, ny, nz, u, v; };

// The original's vertex-shader constants c1-c13 (see cddata/vsh), in the port's layout.
struct MeshConstants {
    float fog[4];               // c1: fog start, fog end, 1 / (end - start)
    float lightColor[3][4];     // c2-c4: rows = R, G, B weights of lights 0-2 and ambient
    float localToScreen[4][4];  // c5-c8 before transposing: row-vector matrix, pos * M
    float lightLocal[3][4];     // c9-c11: light directions in model space
    float baseColor[4];         // c12
    float stzOffset[4];         // c13: S, T, Z offsets
};

// The four vertex shaders of the original (cddata/vsh), in the order Vertex.cpp created them.
enum MeshShader : uint32_t { MESH_LIGHT = 0, MESH_DIFFUSE = 1, MESH_STZOFS = 2, MESH_SPECULAR = 3 };

struct TLVERTEX_TEX { float x, y, z, rhw; D3DCOLOR color; float u, v; };   // FVF 0x144 in the original
struct TLVERTEX     { float x, y, z, rhw; D3DCOLOR color; };               // FVF 0x044

// Same fields and defaults as the original registry settings (HKCU\Software\TAITO\...).
struct GRAPHIC_CONFIG {
    int windowed = 0;
    int gpu = 1;            // kept for compatibility; vertex processing is always on the GPU now
    int scrwidth = 640;
    int scrheight = 480;
    int bbcount = 0;        // ignored: the swapchain is managed by SDL
    int scrformat = 22;     // ignored: the swapchain format is chosen by SDL
    int refrate = 0;        // ignored: vsync is used instead
    int zformat = 1;
    int texformat = 0;      // 0 = P8 (default), anything else = BGRA8
    int texmodulate = 1;    // 1 = MODULATE2X for PS2 vertex colours (0x80 = 1.0)
    int fog = 1;
    int texsize = 2;        // max texture size: 0 = 256, 1 = 512, 2-3 = 1024
    int screffect = 7;      // non-zero keeps the previous frame available as texture slot 1
    int snapshot = 0;       // allow PrintScreen
    int crop = 1;           // new: show only the game's 600x420 canvas, at 4:3 (0 = whole 640x480 target)
};

class CGraphic {
public:
    static constexpr uint32_t kMaxTextures = 0x4000;    // IDs >= this mean "untextured"
    static constexpr uint32_t kNoPalette = 0x4000;

    // Setup / teardown
    static bool LoadConfig(int argc = 0, char **argv = nullptr);  // graphics.ini in the pref path; "W"/"w" forces windowed
    static bool Init(SDL_Window *window);
    static void Term();
    static bool Reset();                    // device loss does not exist with SDL GPU; kept for callers

    // Frame (BeginScene/EndScene/Present in the original, 0x40f030/0x40f090)
    static bool BeginFrame();
    // Keeps the recording lock across a frame's packet processing (CDma::Send), so the many calls
    // inside re-enter it cheaply. Nothing that waits for another guest thread may run while held.
    static void HoldRecordingLock(bool hold);
    static uint32_t GetFrameSerial();                // frames begun so far (debugging)
    // Diagnostics: live objects and work counters (the counters are reset by the caller).
    struct DebugStats { uint32_t liveTextures, liveMeshes, pipelines, flushes, texCreates, texDestroys,
                        meshCreates, meshDestroys, uploadBytes, texUploads, texUploadBytes,
                        meshUploadBytes, vertexBytes, meshDraws, meshParamBlocks, drawCmds, pipelineBinds, mergedDraws; };
    static DebugStats &GetDebugStats();
    // Debugging: read a managed texture back from the GPU, compare it with the CPU copy and
    // write both as BMP files (<prefix>_gpu.bmp, <prefix>_cpu.bmp). Returns differing texels.
    static int DebugCompareManagedTexture(uint32_t nHandle, const char *prefix);
    static bool EndFrame(bool bSnapshotKey = false);
    // When rendering runs on another thread than the window (the recompiled game renders on its
    // game thread), EndFrame only hands the finished frame over and the window thread calls
    // PresentPending() to show it. SDL allows swapchain access only on the window's thread.
    static void SetDeferredPresent(bool bDefer);
    static bool PresentPending();
    // GL renderer with a render thread: the previous frame is still being drawn (the frame
    // limiter then skips drawing instead of waiting). Not defined by the SDL GPU renderer.
    static bool FrameInFlight();
    static uint64_t FrameBusyNs();      // estimated time until the frame in flight is presented

    // Textures and palettes
    static bool SetGamma(float fGamma, bool bStore);
    static TEXSIZE *FindTexSize(int nID);
    static bool LoadTexture(uint32_t nID, uint32_t nWidth, uint32_t nHeight);  // create/resize slot nID
    // GS image upload into direct slot nID (original signature, 0x403660): (x, y, w, h) in
    // texels, nBits 4/8/32. Creates or resizes the slot like the original.
    static bool LoadTexture(uint32_t nID, const void *pSrc, int x, int y, int w, int h, int nBits);
    static void *LockTexture(uint32_t nID, uint32_t *pPitch);                 // CPU texels for CTexture to fill
    static void UnlockTexture(uint32_t nID);
    // PS2 CLUT for direct slot nID (16 or 256 entries, alpha 0x80 = opaque), converted like the
    // original (0x403840). Ignored unless textures are P8.
    static bool SetPalette(uint32_t nID, const void *pPs2Clut, uint32_t nCount);
    static uint32_t GetTextureModulate();           // m_nD3DTOPModulate: TOP_MODULATE2X by default
    static bool SetTexture(uint32_t nID, uint8_t nGroup, uint32_t nPalID);
    static TexFormat GetTextureFormat() { return m_nTexFormat; }
    static uint32_t GetMaxTextureWidth();
    static uint32_t GetMaxTextureHeight();
    static uint32_t PowerOfTwo(uint32_t v);         // D3DX rounded texture sizes up like this

    // Handle-based textures and palettes for CTexture. The original kept CTexture's textures in
    // a separate table that SetTexture reached with nGroup == 0x0F; SetManagedTexture and
    // SetManagedPalette fill that table here.
    static constexpr uint8_t kGroupManaged = 0x0f;
    static CGraphicTexture *CreateTexture(uint32_t nWidth, uint32_t nHeight, TexFormat fmt);
    static void DestroyTexture(CGraphicTexture *pTex);
    static void *LockTexture(CGraphicTexture *pTex, uint32_t *pPitch);
    static void UnlockTexture(CGraphicTexture *pTex);
    static void SetTextureUVSize(CGraphicTexture *pTex, uint32_t nWidth, uint32_t nHeight);  // for NormalizeUV
    static uint32_t AllocPalette();                 // kNoPalette when the atlas is full
    static void FreePalette(uint32_t nPalID);
    static bool UploadPalette(uint32_t nPalID, const uint8_t *pRGBA, uint32_t nCount);
    static void SetManagedTexture(uint32_t nHandle, CGraphicTexture *pTex);
    static void SetManagedPalette(uint32_t nHandle, uint32_t nPalID);

    // Immediate-mode 2D drawing with pre-transformed vertices (same data flow as the original)
    static bool NormalizeUV(float *pu, float *pv);
    static bool SetVertex(int i, D3DCOLOR color, const float *pos, float u, float v);
    static bool AddStripVertex(int i, D3DCOLOR color, const float *pos, float u, float v);
    static bool SetSpriteRect(D3DCOLOR color, const float *pos, float u, float v);
    static bool DrawPrimitive(uint32_t nVertexCount, uint32_t nPrimCount);
    static void SetPrimitiveType(d3d::PrimitiveType t) { m_nPrimType = t; }

    // Render state, replacing the GS emulator's direct IDirect3DDevice8 calls
    static void SetRenderState(d3d::RenderState state, uint32_t value);
    static void SetTextureStageState(uint32_t stage, d3d::TextureStageState type, uint32_t value);
    static bool Clear(uint32_t flags, D3DCOLOR color, float z);

    // Screen, viewport and render targets
    static bool ClearBorders();
    static bool SetViewport(int nType, bool bApply);
    static bool PrintScreen();
    static bool SetRenderTargetTexture(int nType);

    // 3D models (CVertex). DrawMesh uses the current render state and texture like the 2D path,
    // plus the D3D viewport transform of the current viewport, as the original did.
    static CGraphicMesh *CreateMesh(const MeshVertex *pVertices, uint32_t nCount);
    static void DestroyMesh(CGraphicMesh *pMesh);
    static bool DrawMesh(CGraphicMesh *pMesh, MeshShader shader, d3d::PrimitiveType type, uint32_t nFirst,
                         uint32_t nPrimCount, uint32_t nCull, const MeshConstants &c);
    // A strip model's strips (as DrawMesh with TRIANGLESTRIP, one after the other from vertex 0;
    // strip i has prims[i] triangles and cull mode culls[i]) in one call.
    static void DrawMeshStrips(CGraphicMesh *pMesh, MeshShader shader, const uint32_t *prims, const uint32_t *culls,
                               size_t n, const MeshConstants &c);

    static SDL_GPUDevice *GetDevice() { return m_pDevice; }

    static const char *GetLastError() { return m_szLastError; }

    // Static members (public like the originals, which other unb files read directly)
    struct Impl;                            // backend state, defined in Graphic.cpp
    static GRAPHIC_CONFIG m_Config;
    static SDL_GPUDevice *m_pDevice;
    static TexFormat m_nTexFormat;
    static d3d::PrimitiveType m_nPrimType;
    static char m_szLastError[256];
};
