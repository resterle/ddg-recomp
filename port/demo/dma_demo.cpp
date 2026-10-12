// Drives the port the way the game does: one DMA chain per frame, sent on channel 1 with
// CDma::Send (the original's sceDmaSend replacement). The chain exercises cnt/call/ret/ref/end
// tags, VIF codes in the tag (TTE), DIRECT into the GS emulator, and UNPACK/MSCNT, which go to
// a VU1 handler (CVertex will be that handler; here it just checks what arrives).
//
//   ./dma_demo W [--frames N] [--snapshot]

#include "../src/unb/Dma.h"
#include "../src/unb/Graphic.h"
#include "PacketBuilder.h"
#include <cmath>
#include <cstdlib>

// VIF codes
static uint32_t VNOP() { return 0; }
static uint32_t VDIRECT(uint32_t qwc) { return 0x50u << 24 | qwc; }
static uint32_t VSTCYCL(uint32_t cl, uint32_t wl) { return 0x01u << 24 | wl << 8 | cl; }
static uint32_t VUNPACK(uint32_t vn, uint32_t vl, uint32_t num, uint32_t addr, bool flg)
{
    return (0x60u | vn << 2 | vl) << 24 | num << 16 | (flg ? 0x8000u : 0) | addr;
}
static uint32_t VMSCNT() { return 0x17u << 24; }

// DMA buffer: tag addresses are byte offsets into it.
struct Chain {
    std::vector<uint64_t> q;
    uint32_t Offset() const { return (uint32_t)(q.size() * 8); }
    uint32_t Tag(uint32_t qwc, uint32_t id, uint32_t addr, uint32_t vif0, uint32_t vif1)
    {
        uint32_t at = Offset();
        q.push_back(qwc | (uint64_t)id << 28 | (uint64_t)addr << 32);
        q.push_back(vif0 | (uint64_t)vif1 << 32);
        return at;
    }
    void Append(const Gif &g) { q.insert(q.end(), g.q.begin(), g.q.end()); }
    void Words(std::initializer_list<uint32_t> w)
    {
        std::vector<uint32_t> v(w);
        while (v.size() % 4) v.push_back(VNOP());           // pad to whole qwords
        for (size_t i = 0; i < v.size(); i += 2) q.push_back(v[i] | (uint64_t)v[i + 1] << 32);
    }
    void Patch(uint32_t tagOffset, uint32_t addr) { q[tagOffset / 8] = (q[tagOffset / 8] & 0xffffffffull) | (uint64_t)addr << 32; }
};

enum { REFE = 0, CNT = 1, NEXT = 2, REF = 3, REFS = 4, CALL = 5, RET = 6, END = 7 };

struct TestVu1 : IVu1Handler {
    int unpacks = 0, runs = 0;
    size_t lastBytes[4] = {};
    void Unpack(uint32_t code, const uint8_t *, size_t bytes) override
    {
        if (unpacks < 4) lastBytes[unpacks] = bytes;
        unpacks++;
        (void)code;
    }
    void Run() override { runs++; }
};

static int g_Completions = 0;
static int OnComplete(int) { g_Completions++; return 0; }

static void BuildChain(Chain &c, int frame)
{
    float t = frame / 60.0f;

    // Context and a textured sprite, VIF DIRECT in the tag (TTE).
    Gif g0;
    if (frame == 0) {
        uint8_t clut[16 * 4];
        for (int i = 0; i < 16; i++) {
            clut[i * 4] = (uint8_t)(255 - i * 14); clut[i * 4 + 1] = (uint8_t)(60 + i * 12);
            clut[i * 4 + 2] = (uint8_t)(i * 16); clut[i * 4 + 3] = i ? 0x80 : 0;
        }
        Upload(g0, 100, 0x00, 8, 2, clut, sizeof(clut));
        uint8_t tex[32 * 32 / 2] = {};
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                int d = (int)std::sqrt((float)((x - 16) * (x - 16) + (y - 16) * (y - 16)));
                tex[(y * 32 + x) / 2] |= (uint8_t)((d < 15 ? 1 + d : 0) << ((x & 1) * 4));
            }
        Upload(g0, 101, 0x14, 32, 32, tex, sizeof(tex));
    }
    g0.Tag(5, false, 0, {AD});
    g0.AD(XYOFFSET_1, OFX | (uint64_t)OFY << 32);
    g0.AD(TEST_1, 0);
    g0.AD(ZBUF_1, 1ull << 32);
    g0.AD(ALPHA_1, 0x44);
    g0.AD(CLAMP_1, 0);
    float x = 60 + 30 * std::sin(t);
    g0.Tag(1, false, 1, {PRIM, TEX0_1, RGBAQ, UV, XYZ2, UV, XYZ2, NOP});
    g0.Raw(PrimReg(6, true, true, true, true), Tex0(101, 0x14, 5, 5, 100));
    g0.Raw(RGBA(0x80, 0x80, 0x80, 0x80), UVr(0, 0));
    g0.Raw(XYZ(x, 40), UVr(32, 32));
    g0.Raw(XYZ(x + 200, 240), 0);
    c.Tag((uint32_t)g0.Qwords(), CNT, 0, VNOP(), VDIRECT((uint32_t)g0.Qwords()));
    c.Append(g0);

    // call into a subroutine that draws a triangle strip, then ret back here.
    uint32_t call = c.Tag(0, CALL, 0, VNOP(), VNOP());

    // ref: GIF data stored elsewhere in the buffer (patched below).
    uint32_t ref = c.Tag(0, REF, 0, VNOP(), VNOP());

    // VU1 path: STCYCL, UNPACK V4-32 x2 (32 bytes), UNPACK V2-16 x3 with FLG (12 bytes), MSCNT.
    c.Tag(4, CNT, 0, VNOP(), VNOP());
    c.Words({VSTCYCL(1, 1), VUNPACK(3, 0, 2, 0, true), 0x3f800000, 0, 0, 0, 0, 0, 0, 0x3f800000,
             VUNPACK(1, 1, 3, 2, true), 0x00010002, 0x00030004, 0x00050006, VMSCNT()});

    // end: last drawing in the tag's DIRECT.
    Gif g4;
    g4.Tag(5, true, 0, {RGBAQ, XYZ2}, PrimReg(2, false, false, false, false), true);
    for (int i = 0; i < 5; i++) {
        g4.Packed4(0x80, 0x80, i * 30, 0x80);
        g4.Packed4(GX(320 + i * 70), GY(i & 1 ? 280 : 440), 1, 0);
    }
    c.Tag((uint32_t)g4.Qwords(), END, 0, VNOP(), VDIRECT((uint32_t)g4.Qwords()));
    c.Append(g4);

    // Subroutine for the call: cnt (triangle strip via DIRECT in the tag) + ret.
    uint32_t sub = c.Offset();
    Gif g1;
    g1.Tag(8, true, 0, {RGBAQ, XYZ2}, PrimReg(4, true, false, false, false), true);
    for (int i = 0; i < 8; i++) {
        g1.Packed4(40 + i * 25, 0x80 - i * 10, 0x30 + i * 10, 0x80);
        g1.Packed4(GX(40 + (i / 2) * 70.0f), GY((i & 1) ? 440.0f : 300.0f + 20 * std::sin(t * 2 + i)), 1, 0);
    }
    c.Tag((uint32_t)g1.Qwords(), CNT, 0, VNOP(), VDIRECT((uint32_t)g1.Qwords()));
    c.Append(g1);
    c.Tag(0, RET, 0, VNOP(), VNOP());
    c.Patch(call, sub);

    // Data for the ref tag: a VIF stream with DIRECT, then the GIF packet (an untextured sprite).
    uint32_t data = c.Offset();
    Gif g2;
    g2.Tag(1, true, 1, {PRIM, RGBAQ, XYZ2, XYZ2});
    g2.Raw(PrimReg(6, false, false, true, false), RGBA(0x20, 0x80, 0xff, 0x60));
    g2.Raw(XYZ(320, 40), XYZ(600, 200));
    c.Words({VNOP(), VNOP(), VNOP(), VDIRECT((uint32_t)g2.Qwords())});
    c.Append(g2);
    // ref moves MADR to the data and continues after the ref tag; qwc covers VIF codes + GIF.
    c.q[ref / 8] = (uint64_t)(1 + g2.Qwords()) | (uint64_t)REF << 28 | (uint64_t)data << 32;
}

int main(int argc, char **argv)
{
    int frames = 0;
    bool snapshot = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot")) snapshot = true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    CGraphic::LoadConfig(argc, argv);
    if (snapshot) CGraphic::m_Config.snapshot = 1;
    SDL_Window *win = SDL_CreateWindow("DMA/VIF demo", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win || !CGraphic::Init(win)) { SDL_Log("init failed: %s", CGraphic::GetLastError()); return 1; }
    CGs::Reset();
    TestVu1 vu1;
    CDma::SetVu1Handler(&vu1);
    CDma::SetHandler(dma::VIF1, OnComplete);

    bool quit = false;
    int frame = 0;
    for (; !quit && (frames == 0 || frame < frames); frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) quit = true;
        Chain c;
        BuildChain(c, frame);
        // The original's frame clear came from the game's own packets; clear here for the demo.
        CGraphic::Clear(d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER, 0xff141e2c, 1.0f);
        CDma::Send(dma::VIF1, c.q.data(), 0, dma::CHCR_TTE, snapshot && frame == frames - 1);
    }
    SDL_Log("frames %d, completions %d, VU1 unpacks %d (first sizes %zu, %zu), runs %d", frame, g_Completions,
            vu1.unpacks, vu1.lastBytes[0], vu1.lastBytes[1], vu1.runs);
    CGraphic::Term();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
