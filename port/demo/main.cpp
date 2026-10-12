// Exercises the SDL3 GPU CGraphic the way the game's GS emulator (0x40d1b0) drives it:
// SetTexture -> SetVertex/SetSpriteRect or AddStripVertex -> DrawPrimitive.
//
//   ./graphic_demo [W] [--frames N] [--snapshot]
//   W            windowed (same switch as the original game)
//   --frames N   quit after N frames (0 = run until the window is closed)
//   --snapshot   write snapshot/*.bmp on the last frame
//   --txm FILE   also load texture records from a TXM file or DAT container through CTexture
//                and draw them (the GS emulator path: SetTexture(handle, 0x0F, clutHandle))

#include "../src/unb/Graphic.h"
#include "../src/unb/Texture.h"
#include <utility>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace d3d;

// PS2-style vertex colour: 0x80 = 1.0 with MODULATE2X.
static D3DCOLOR Ps2Color(int r, int g, int b, int a) { return (uint32_t)a << 24 | r << 16 | g << 8 | b; }

// RGBA (alpha 0-255) to a PS2 CLUT as SetPalette expects: alpha 0x80 = opaque, and 256-entry
// CLUTs in CSM1 order (entries 8-15 and 16-23 of each 32 swapped; the swap is its own inverse).
static void ToPs2Clut(uint8_t *pal, int count)
{
    for (int i = 0; i < count; i++) pal[i * 4 + 3] = (uint8_t)((pal[i * 4 + 3] * 128 + 127) / 255);
    if (count < 256) return;
    for (int i = 0; i < 256; i++)
        if (((i >> 3) & 3) == 1)
            for (int c = 0; c < 4; c++) std::swap(pal[i * 4 + c], pal[(i + 8) * 4 + c]);
}

static void MakeTextures()
{
    // Slot 2: 64x64 P8 checkerboard, 16 palette indices.
    CGraphic::LoadTexture(2, 64, 64);
    uint32_t pitch = 0;
    if (auto *p = static_cast<uint8_t *>(CGraphic::LockTexture(2, &pitch))) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) p[y * pitch + x] = (uint8_t)((((x >> 3) ^ (y >> 3)) & 1) ? 1 + (x >> 2) % 15 : 0);
        CGraphic::UnlockTexture(2);
    }
    uint8_t pal[256 * 4] = {};
    for (int i = 0; i < 256; i++) {
        pal[i * 4 + 0] = (uint8_t)(40 + i * 13);
        pal[i * 4 + 1] = (uint8_t)(200 - i * 9);
        pal[i * 4 + 2] = (uint8_t)(90 + i * 11);
        pal[i * 4 + 3] = 255;
    }
    pal[0] = 20; pal[1] = 20; pal[2] = 30; pal[3] = 255;
    ToPs2Clut(pal, 16);
    CGraphic::SetPalette(2, pal, 16);

    // Slot 3: 64x64 colour texture (BGRA) with a radial alpha falloff, uploaded through the same
    // path. In P8 mode it is stored as indices into its own grey-ramp palette instead.
    CGraphic::LoadTexture(3, 64, 64);
    if (auto *p = static_cast<uint8_t *>(CGraphic::LockTexture(3, &pitch))) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                float d = std::sqrt((x - 31.5f) * (x - 31.5f) + (y - 31.5f) * (y - 31.5f)) / 32.0f;
                uint8_t a = (uint8_t)(d >= 1 ? 0 : 255 * (1 - d));
                if (CGraphic::GetTextureFormat() == TEXFMT_P8) p[y * pitch + x] = a;
                else { uint8_t *t = p + y * pitch + x * 4; t[0] = (uint8_t)(x * 4); t[1] = 160; t[2] = (uint8_t)(y * 4); t[3] = a; }
            }
        CGraphic::UnlockTexture(3);
    }
    for (int i = 0; i < 256; i++) { pal[i * 4] = 255; pal[i * 4 + 1] = (uint8_t)(128 + i / 2); pal[i * 4 + 2] = 64; pal[i * 4 + 3] = (uint8_t)i; }
    ToPs2Clut(pal, 256);
    CGraphic::SetPalette(3, pal, 256);
}

static void Sprite(uint32_t tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, D3DCOLOR c)
{
    CGraphic::SetPrimitiveType(TRIANGLESTRIP);
    CGraphic::SetTexture(tex, 0, tex);
    float a[3] = {x0, y0, 0.5f}, b[3] = {x1, y1, 0.5f};
    CGraphic::SetVertex(0, c, a, u0, v0);
    CGraphic::SetSpriteRect(c, b, u1, v1);
    CGraphic::DrawPrimitive(4, 2);
}

static CTexture g_Texture;
static uint8_t *g_pTxmData = nullptr;
static std::vector<TXMHEADER *> g_Records;

// Load up to 12 texture records from a TXM file or a DAT container.
static void LoadRecords(const char *path)
{
    size_t size = 0;
    g_pTxmData = static_cast<uint8_t *>(SDL_LoadFile(path, &size));
    if (!g_pTxmData) { SDL_Log("%s: %s", path, SDL_GetError()); return; }
    std::vector<TXMHEADER *> cand;
    if (size >= 8 && memcmp(g_pTxmData, "DAT", 4) == 0) {
        uint32_t count;
        memcpy(&count, g_pTxmData + 4, 4);
        for (uint32_t i = 0; i < count && cand.size() < 12; i++) {
            uint32_t off;
            memcpy(&off, g_pTxmData + 8 + 8 * i, 4);
            if (off + sizeof(TXMHEADER) <= size) cand.push_back(reinterpret_cast<TXMHEADER *>(g_pTxmData + off));
        }
    } else {
        cand.push_back(reinterpret_cast<TXMHEADER *>(g_pTxmData));
    }
    for (TXMHEADER *p : cand) {
        if (g_Texture.Load(p)) g_Records.push_back(p);
        else SDL_Log("CTexture::Load failed for a record (psm 0x%02x)", p->psm);
    }
    SDL_Log("loaded %d texture records from %s", (int)g_Records.size(), path);
}

// Draw loaded records in a grid, as the GS emulator would: TEX0 gives the handles, UV comes in
// texels and goes through NormalizeUV.
static void DrawRecords()
{
    CGraphic::SetRenderState(RS_ALPHABLENDENABLE, 1);
    CGraphic::SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
    CGraphic::SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
    CGraphic::SetTextureStageState(0, TSS_ALPHAOP, TOP_MODULATE);
    CGraphic::SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_CLAMP);
    CGraphic::SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_CLAMP);
    float x = 8, y = 8, rowH = 0;
    for (TXMHEADER *p : g_Records) {
        float scale = SDL_min(1.0f, 150.0f / SDL_max(p->width, p->height));
        float w = p->width * scale, h = p->height * scale;
        if (x + w > 632) { x = 8; y += rowH + 8; rowH = 0; }
        CGraphic::SetTexture((uint32_t)p->handle, CGraphic::kGroupManaged, (uint32_t)(uint16_t)p->clutHandle);
        float u0 = 0, v0 = 0, u1 = (float)p->width, v1 = (float)p->height;
        CGraphic::NormalizeUV(&u0, &v0);
        CGraphic::NormalizeUV(&u1, &v1);
        CGraphic::SetPrimitiveType(TRIANGLESTRIP);
        float a[3] = {x, y, 0.5f}, b[3] = {x + w, y + h, 0.5f};
        CGraphic::SetVertex(0, Ps2Color(0x80, 0x80, 0x80, 0xff), a, u0, v0);
        CGraphic::SetSpriteRect(Ps2Color(0x80, 0x80, 0x80, 0xff), b, u1, v1);
        CGraphic::DrawPrimitive(4, 2);
        x += w + 8;
        rowH = SDL_max(rowH, h);
    }
    CGraphic::SetRenderState(RS_ALPHABLENDENABLE, 0);
    CGraphic::SetTextureStageState(0, TSS_ALPHAOP, TOP_SELECTARG1);
}

static void DrawFrame(int frame)
{
    float t = frame / 60.0f;
    CGraphic::Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xff203048, 1.0f);
    CGraphic::SetRenderState(RS_ZENABLE, 0);

    // Previous frame (texture slot 1), shrunk into the corner: the screffect feedback path.
    Sprite(1, 440, 20, 620, 155, 0, 0, 1, 1, Ps2Color(0x80, 0x80, 0x80, 0x80));

    // Paletted checkerboard, tiled 3x with WRAP, gently scrolling.
    CGraphic::SetTextureStageState(0, TSS_ADDRESSU, TADDRESS_WRAP);
    CGraphic::SetTextureStageState(0, TSS_ADDRESSV, TADDRESS_WRAP);
    Sprite(2, 40, 40, 280, 280, t * 0.25f, 0, 3 + t * 0.25f, 3, Ps2Color(0x80, 0x80, 0x80, 0x80));

    // Bilinear-filtered paletted sprite (filtering happens after the palette lookup).
    CGraphic::SetTextureStageState(0, TSS_MAGFILTER, TEXF_LINEAR);
    CGraphic::SetTextureStageState(0, TSS_MINFILTER, TEXF_LINEAR);
    Sprite(2, 300, 180, 420, 300, 0, 0, 0.5f, 0.5f, Ps2Color(0x80, 0x80, 0x80, 0x80));
    CGraphic::SetTextureStageState(0, TSS_MAGFILTER, TEXF_POINT);
    CGraphic::SetTextureStageState(0, TSS_MINFILTER, TEXF_POINT);

    // Alpha-blended glow sprite moving in a circle.
    CGraphic::SetRenderState(RS_ALPHABLENDENABLE, 1);
    CGraphic::SetRenderState(RS_SRCBLEND, BLEND_SRCALPHA);
    CGraphic::SetRenderState(RS_DESTBLEND, BLEND_INVSRCALPHA);
    float cx = 160 + 60 * std::cos(t * 2), cy = 160 + 60 * std::sin(t * 2);
    Sprite(3, cx - 64, cy - 64, cx + 64, cy + 64, 0, 0, 1, 1, Ps2Color(0x80, 0x80, 0x80, 0x80));
    CGraphic::SetRenderState(RS_ALPHABLENDENABLE, 0);

    // Untextured triangle strip streamed through AddStripVertex, like GS PRIM=TRISTRIP.
    CGraphic::SetTexture(CGraphic::kMaxTextures, 0, CGraphic::kNoPalette);
    CGraphic::SetPrimitiveType(TRIANGLESTRIP);
    float x = 40, w = 40;
    for (int i = 0; i < 12; i++) {
        float p[3] = {x + (i / 2) * w, (i & 1) ? 440.0f : 340.0f + 20 * std::sin(t * 3 + i), 0.5f};
        D3DCOLOR c = Ps2Color(i * 10, 0x80 - i * 8, 0x40 + i * 6, 0x80);
        if (i < 3) CGraphic::SetVertex(i, c, p, 0, 0);
        else if (i == 3) { CGraphic::SetVertex(3, c, p, 0, 0); CGraphic::DrawPrimitive(4, 2); }
        else {
            // GS emulator pattern: 5th vertex -> slot 3, then even vertex numbers -> 1, odd -> 2.
            int n = i + 1;
            CGraphic::AddStripVertex(n == 5 ? 3 : (n & 1) ? 2 : 1, c, p, 0, 0);
            CGraphic::DrawPrimitive(3, 1);
        }
    }

    // A flat-shaded line, as GS PRIM=LINE with flat shading.
    CGraphic::SetRenderState(RS_SHADEMODE, SHADE_FLAT);
    CGraphic::SetPrimitiveType(LINELIST);
    float l0[3] = {300, 330, 0.5f}, l1[3] = {620, 460, 0.5f};
    CGraphic::SetVertex(0, 0xffffff00, l0, 0, 0);
    CGraphic::SetVertex(1, 0xff00ffff, l1, 0, 0);
    CGraphic::DrawPrimitive(2, 1);
    CGraphic::SetRenderState(RS_SHADEMODE, SHADE_GOURAUD);
}

int main(int argc, char **argv)
{
    int frames = 0;
    bool snapshot = false;
    const char *txm = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--txm") && i + 1 < argc) txm = argv[++i];
        else if (!strcmp(argv[i], "--snapshot")) snapshot = true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    CGraphic::LoadConfig(argc, argv);
    CGraphic::m_Config.snapshot = snapshot ? 1 : CGraphic::m_Config.snapshot;
    SDL_Window *win = SDL_CreateWindow("CGraphic SDL3 GPU demo", CGraphic::m_Config.scrwidth, CGraphic::m_Config.scrheight,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win || !CGraphic::Init(win)) { SDL_Log("init failed: %s", CGraphic::GetLastError()); return 1; }
    MakeTextures();
    g_Texture.Init();
    if (txm) LoadRecords(txm);

    bool quit = false;
    for (int frame = 0; !quit && (frames == 0 || frame < frames); frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) quit = true;
        if (!CGraphic::BeginFrame()) break;
        if (g_Records.empty()) DrawFrame(frame);
        else {
            CGraphic::Clear(CLEAR_TARGET | CLEAR_ZBUFFER, 0xff203048, 1.0f);
            CGraphic::SetRenderState(RS_ZENABLE, 0);
            DrawRecords();
        }
        bool last = frames != 0 && frame == frames - 1;
        if (!CGraphic::EndFrame(last && snapshot)) { SDL_Log("EndFrame: %s", CGraphic::GetLastError()); break; }
    }
    for (TXMHEADER *p : g_Records) g_Texture.Unload(p);
    g_Texture.Term();
    SDL_free(g_pTxmData);
    CGraphic::Term();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
