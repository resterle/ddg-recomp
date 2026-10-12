// Drives the SDL3 GPU port only through GS packets, the way the game's VIF DIRECT data reaches
// CGs::ProcessGif: image transfers (CLUT + 4-bit texture) into the direct table, then sprites,
// a triangle strip and a line strip. With --txm, a game texture loaded through CTexture is drawn
// as well, referenced from TEX0 with PSM = 0x0F like the game does.
//
//   ./gs_demo W [--frames N] [--snapshot] [--txm FILE]

#include "../src/unb/Graphic.h"
#include "../src/unb/Gs.h"
#include "../src/unb/Texture.h"
#include "PacketBuilder.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>


static void BuildTextures(Gif &g)
{
    // 16-entry CLUT (8x2, PSMCT32) at "CBP" 100, alpha 0x80 = opaque.
    uint8_t clut[16 * 4];
    for (int i = 0; i < 16; i++) {
        clut[i * 4 + 0] = (uint8_t)(i * 16);
        clut[i * 4 + 1] = (uint8_t)(255 - i * 12);
        clut[i * 4 + 2] = (uint8_t)(128 + i * 7);
        clut[i * 4 + 3] = i == 0 ? 0x00 : 0x80;         // index 0 transparent
    }
    Upload(g, 100, 0x00, 8, 2, clut, sizeof(clut));
    // 32x32 PSMT4 texture at "TBP" 101: a diamond of colour bands.
    uint8_t tex[32 * 32 / 2] = {};
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int d = abs(x - 16) + abs(y - 16);
            int idx = d < 15 ? 1 + d % 15 : 0;
            tex[(y * 32 + x) / 2] |= (uint8_t)(idx << ((x & 1) * 4));
        }
    Upload(g, 101, 0x14, 32, 32, tex, sizeof(tex));
}

static TXMHEADER *g_pRecord = nullptr;

static void BuildFrame(Gif &g, int frame)
{
    float t = frame / 60.0f;
    // Context setup: offsets, tests (no depth), normal alpha blending, clamp.
    g.Tag(5, false, 0, {AD});
    g.AD(XYOFFSET_1, OFX | (uint64_t)OFY << 32);
    g.AD(TEST_1, 0);
    g.AD(ZBUF_1, 1ull << 32);                    // ZMSK
    g.AD(ALPHA_1, 0x44);                         // (Cs - Cd) * As + Cd
    g.AD(CLAMP_1, 0);                            // REPEAT

    // Textured sprite from the direct table (TEX0: TBP 101, PSMT4, CBP 100), REGLIST format.
    float x = 40 + 20 * std::sin(t);
    g.Tag(1, false, 1, {PRIM, TEX0_1, RGBAQ, UV, XYZ2, UV, XYZ2, NOP});
    g.Raw(PrimReg(6, true, true, true, true), Tex0(101, 0x14, 5, 5, 100));
    g.Raw(RGBA(0x80, 0x80, 0x80, 0x80), UVr(0, 0));
    g.Raw(XYZ(x, 40), UVr(32, 32));
    g.Raw(XYZ(x + 192, 232), 0);

    // Same texture tiled 2x with REPEAT, in PACKED format with PRE.
    g.Tag(1, false, 0, {TEX0_1, RGBAQ, UV, XYZ2, UV, XYZ2}, PrimReg(6, true, true, true, true), true);
    g.Raw(Tex0(101, 0x14, 5, 5, 100), TEX0_1);
    g.Packed4(0x80, 0x60, 0x60, 0x80);
    g.Packed4(0, 0, 0, 0);
    g.Packed4(GX(260), GY(40), 1, 0);
    g.Packed4(64 * 16, 64 * 16, 0, 0);
    g.Packed4(GX(388), GY(168), 1, 0);

    // Gouraud triangle strip, untextured: drawing starts at the 4th vertex like the original.
    g.Tag(8, false, 0, {RGBAQ, XYZ2}, PrimReg(4, true, false, false, false), true);
    for (int i = 0; i < 8; i++) {
        float px = 40 + (i / 2) * 60, py = (i & 1) ? 440.0f : 300.0f + 20 * std::sin(t * 2 + i);
        g.Packed4(40 + i * 25, 0x80 - i * 10, 0x30 + i * 10, 0x80);
        g.Packed4(GX(px), GY(py), 1, 0);
    }

    // Flat-shaded line strip.
    g.Tag(5, false, 0, {RGBAQ, XYZ2}, PrimReg(2, false, false, false, false), true);
    for (int i = 0; i < 5; i++) {
        g.Packed4(0x80, 0x80, i * 30, 0x80);
        g.Packed4(GX(300 + i * 70), GY(i & 1 ? 300 : 440), 1, 0);
    }

    // Game texture loaded through CTexture: TEX0.PSM = record state (0x0F), TBP/CBP = handles.
    if (g_pRecord) {
        TXMHEADER *p = g_pRecord;
        float w = SDL_min(200.0f, (float)p->width), h = w * p->height / p->width;
        g.Tag(1, true, 1, {PRIM, TEX0_1, RGBAQ, UV, XYZ2, UV, XYZ2, NOP});
        g.Raw(PrimReg(6, true, true, true, true), Tex0((uint16_t)p->handle, p->state, 0, 0, (uint16_t)p->clutHandle));
        g.Raw(RGBA(0x80, 0x80, 0x80, 0x80), UVr(0, 0));
        g.Raw(XYZ(420, 40), UVr(p->width, p->height));
        g.Raw(XYZ(420 + w, 40 + h), 0);
    }
}

int main(int argc, char **argv)
{
    int frames = 0;
    bool snapshot = false;
    const char *txm = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot")) snapshot = true;
        else if (!strcmp(argv[i], "--txm") && i + 1 < argc) txm = argv[++i];
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    CGraphic::LoadConfig(argc, argv);
    if (snapshot) CGraphic::m_Config.snapshot = 1;
    SDL_Window *win = SDL_CreateWindow("GS emulator demo", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win || !CGraphic::Init(win)) { SDL_Log("init failed: %s", CGraphic::GetLastError()); return 1; }
    CGs::Reset();

    CTexture texture;
    texture.Init();
    uint8_t *file = nullptr;
    if (txm && (file = static_cast<uint8_t *>(SDL_LoadFile(txm, nullptr)))) {
        auto *p = reinterpret_cast<TXMHEADER *>(file);
        if (memcmp(file, "DAT", 4) == 0) { uint32_t off; memcpy(&off, file + 8, 4); p = reinterpret_cast<TXMHEADER *>(file + off); }
        if (texture.Load(p)) g_pRecord = p;
        else SDL_Log("CTexture::Load failed");
    }

    Gif init;
    BuildTextures(init);
    CGs::ProcessGif(init.q.data(), init.Qwords());

    bool quit = false;
    for (int frame = 0; !quit && (frames == 0 || frame < frames); frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) quit = true;
        CGraphic::BeginFrame();
        CGraphic::Clear(d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER, 0xff182838, 1.0f);
        Gif g;
        BuildFrame(g, frame);
        size_t used = CGs::ProcessGif(g.q.data(), g.Qwords());
        if (used != g.Qwords()) SDL_Log("ProcessGif consumed %zu of %zu qwords", used, g.Qwords());
        CGraphic::EndFrame(snapshot && frame == frames - 1);
    }
    if (g_pRecord) texture.Unload(g_pRecord);
    texture.Term();
    SDL_free(file);
    CGraphic::Term();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
