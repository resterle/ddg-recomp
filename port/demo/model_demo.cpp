// Shows a 3D model from the game's data through the full port: CTexture loads its textures,
// CVertex registers it, and each frame a DMA chain (GS context via DIRECT, camera and object
// blocks via UNPACK, then a ref into the model's own VIF stream) goes through CDma -> VIF1 ->
// CVertex (UNPACK/MSCNT) -> CGraphic::DrawMesh.
//
//   ./model_demo W <model_pdb.dat> <textures_txm.dat> [--entry N] [--mode M] [--frames N] [--snapshot]
//   --mode: lighting mode of the object block (0 = lit, 2 = stzofs, 6 = specular)
//   --entry: model object in the container

#include "../src/unb/Dma.h"
#include "../src/unb/Graphic.h"
#include "../src/unb/Gs.h"
#include "../src/unb/Texture.h"
#include "../src/unb/Vertex.h"
#include "PacketBuilder.h"
#include <cmath>
#include <cstdlib>

static uint32_t U32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

struct Writer {             // appends to the DMA buffer; tag addresses are offsets into it
    std::vector<uint8_t> &buf;
    size_t at;
    void Put(const void *p, size_t n) { memcpy(&buf[at], p, n); at += n; }
    void W(uint32_t v) { Put(&v, 4); }
    void F(float v) { Put(&v, 4); }
    void Tag(uint32_t qwc, uint32_t id, uint32_t addr) { uint64_t t = qwc | (uint64_t)id << 28 | (uint64_t)addr << 32; Put(&t, 8); W(0); W(0); }
};

static void Mat(float m[16], const float *v) { memcpy(m, v, 64); }

int main(int argc, char **argv)
{
    const char *pdbPath = nullptr, *txmPath = nullptr;
    int entry = 0, mode = 0, frames = 0;
    bool snapshot = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot")) snapshot = true;
        else if (argv[i][0] != '-' && strcmp(argv[i], "W") && strcmp(argv[i], "w")) (pdbPath ? txmPath : pdbPath) = argv[i];
    }
    if (!pdbPath || !txmPath) { SDL_Log("usage: model_demo W <model_pdb.dat> <textures_txm.dat> [options]"); return 2; }

    size_t pdbSize = 0, txmSize = 0;
    auto *pdb = static_cast<uint8_t *>(SDL_LoadFile(pdbPath, &pdbSize));
    auto *txm = static_cast<uint8_t *>(SDL_LoadFile(txmPath, &txmSize));
    if (!pdb || !txm || memcmp(pdb, "DAT", 4) || memcmp(txm, "DAT", 4)) { SDL_Log("cannot read DAT files"); return 1; }

    // DMA buffer: 64 KB for the per-frame chain, then the model container (16-byte aligned).
    const size_t kChain = 64 * 1024;
    std::vector<uint8_t> dmaBuf(kChain + pdbSize + 16);
    memcpy(&dmaBuf[kChain], pdb, pdbSize);
    uint32_t count = U32(pdb + 4);
    if (entry < 0 || (uint32_t)entry >= count) { SDL_Log("entry out of range (0-%u)", count - 1); return 1; }
    uint32_t objOff = (uint32_t)kChain + U32(pdb + 8 + 8 * entry);
    uint8_t *object = &dmaBuf[objOff];

    // Texture list = the records of the _txm container, in order.
    std::vector<TXMHEADER *> texList;
    for (uint32_t i = 0; i < U32(txm + 4); i++) texList.push_back(reinterpret_cast<TXMHEADER *>(txm + U32(txm + 8 + 8 * i)));

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    CGraphic::LoadConfig(argc, argv);
    if (snapshot) CGraphic::m_Config.snapshot = 1;
    SDL_Window *win = SDL_CreateWindow("CVertex model demo", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win || !CGraphic::Init(win)) { SDL_Log("init failed: %s", CGraphic::GetLastError()); return 1; }
    CGs::Reset();
    CTexture texture;
    texture.Init();
    CVertex vertex;
    vertex.Init();
    CDma::SetVu1Handler(&vertex);
    if (!vertex.RegisterObject(object, texList.data(), (int)texList.size(), &texture)) { SDL_Log("RegisterObject failed"); return 1; }

    // Bounding box (8 corners at the offset stored in +8) for framing the camera.
    const uint8_t *bb = object + U32(object + 8);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 3; j++) { float v; memcpy(&v, bb + 16 * k + 4 * j, 4); lo[j] = SDL_min(lo[j], v); hi[j] = SDL_max(hi[j], v); }
    float ctr[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    float radius = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) / 2;
    float dist = radius * 2.6f;

    bool quit = false;
    int frame = 0;
    for (; !quit && (frames == 0 || frame < frames); frame++) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) quit = true;
        float t = frame / 60.0f + 0.6f;
        Writer w{dmaBuf, 0};

        // 1) GS context through DIRECT: depth test GEQUAL + writes, alpha blend, fog colour.
        Gif g;
        g.Tag(6, true, 0, {AD});
        g.AD(XYOFFSET_1, OFX | (uint64_t)OFY << 32);
        g.AD(TEST_1, 0x50000);                   // ZTE, ZTST = GEQUAL
        g.AD(ZBUF_1, 0);
        g.AD(ALPHA_1, 0x44);
        g.AD(CLAMP_1, 0);
        g.AD(FOGCOL, 0x302820);
        w.Tag((uint32_t)g.Qwords(), 1 /*cnt*/, 0);
        memcpy(&dmaBuf[8], (uint32_t[2]){0, 0x50000000u | (uint32_t)g.Qwords()}, 8);   // TTE: NOP, DIRECT
        w.Put(g.q.data(), g.q.size() * 8);

        // 2) Camera (28 qwords) and object (6 qwords + mode) blocks via UNPACK V4-32.
        size_t tagAt = w.at;
        w.Tag(0, 1, 0);
        size_t start = w.at;
        w.W(0); w.W(0); w.W(0); w.W(0x6c000000u | 28u << 16);        // UNPACK V4-32 x28, addr 0
        size_t cam = w.at;
        std::vector<float> c(28 * 4, 0.0f);
        c[0x6c / 4] = dist * 4; c[0x7c / 4] = dist * 12;              // fog start / end
        // +0x80: view (look at the centre from a rotating eye), row-vector, right-handed like the
        // PS2 libraries: the model data's strip cull modes assume this handedness.
        float ex = ctr[0] + dist * std::sin(t), ey = ctr[1] + dist * 0.35f, ez = ctr[2] - dist * std::cos(t);
        float fz[3] = {ex - ctr[0], ey - ctr[1], ez - ctr[2]};      // camera z points away from the target
        float fl = std::sqrt(fz[0] * fz[0] + fz[1] * fz[1] + fz[2] * fz[2]);
        for (float &v : fz) v /= fl;
        float up[3] = {0, 1, 0};
        float rx[3] = {up[1] * fz[2] - up[2] * fz[1], up[2] * fz[0] - up[0] * fz[2], up[0] * fz[1] - up[1] * fz[0]};
        float rl = std::sqrt(rx[0] * rx[0] + rx[1] * rx[1] + rx[2] * rx[2]);
        for (float &v : rx) v /= rl;
        float uy[3] = {fz[1] * rx[2] - fz[2] * rx[1], fz[2] * rx[0] - fz[0] * rx[2], fz[0] * rx[1] - fz[1] * rx[0]};
        float view[16] = {rx[0], uy[0], fz[0], 0, rx[1], uy[1], fz[1], 0, rx[2], uy[2], fz[2], 0,
                          -(rx[0] * ex + rx[1] * ey + rx[2] * ez), -(uy[0] * ex + uy[1] * ey + uy[2] * ez),
                          -(fz[0] * ex + fz[1] * ey + fz[2] * ez), 1};
        Mat(&c[0x80 / 4], view);
        // +0x100: light directions as columns (towards the light), +0x140: colours, row 3 ambient
        float L0[3] = {0.4f, 0.8f, -0.45f}, L1[3] = {-0.6f, 0.2f, 0.7f};
        for (int i = 0; i < 3; i++) { c[0x100 / 4 + i * 4 + 0] = L0[i]; c[0x100 / 4 + i * 4 + 1] = L1[i]; }
        float colors[16] = {0.75f, 0.72f, 0.65f, 0, 0.25f, 0.28f, 0.35f, 0, 0, 0, 0, 0, 0.35f, 0.35f, 0.38f, 0};
        Mat(&c[0x140 / 4], colors);
        // +0x180: projection, 45 degrees, z in [0, 1]
        float zn = radius * 0.2f, zf = radius * 20.0f, ys = 1.0f / std::tan(0.3927f), xs = ys * 480.0f / 640.0f;
        float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0};
        Mat(&c[0x180 / 4], proj);
        w.Put(c.data(), c.size() * 4);
        (void)cam;

        w.W(0); w.W(0); w.W(0); w.W(0x6c000000u | 6u << 16);         // UNPACK V4-32 x6
        float obj[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,   // model matrix
                         128, 128, 128, 128,                                // base colour, alpha
                         0, 0, 0, 0};                                       // S, T, Z offsets
        w.Put(obj, sizeof(obj));
        w.W(0); w.W(0); w.W(0); w.W((uint32_t)mode);                   // NOP whose immediate is the mode (+0x6C)
        uint32_t qwc = (uint32_t)((w.at - start) / 16);
        uint64_t tag = qwc | 1ull << 28;
        memcpy(&dmaBuf[tagAt], &tag, 8);

        // 3) ref into each model's VIF stream (packet block + 0x10); its MSCNTs draw.
        for (int k = 0; k < 3; k++) {
            uint32_t off = U32(object + 0x10 + 8 * k), size = U32(object + 0x14 + 8 * k);
            if (!size) continue;
            uint8_t *block = CVertex::PacketBlock(object + off);
            uint32_t addr = (uint32_t)(block + 0x10 - dmaBuf.data());
            w.Tag((CVertex::PacketBlockSize(object + off) - 0x10) / 16, 3 /*ref*/, addr);
        }
        w.Tag(0, 7 /*end*/, 0);

        CGraphic::Clear(d3d::CLEAR_TARGET | d3d::CLEAR_ZBUFFER, 0xff302820, 1.0f);
        CDma::Send(dma::VIF1, dmaBuf.data(), 0, dma::CHCR_TTE, snapshot && frame == frames - 1);
    }
    vertex.UnregisterObject(object);
    vertex.Term();
    texture.Term();
    CGraphic::Term();
    SDL_DestroyWindow(win);
    SDL_free(pdb);
    SDL_free(txm);
    SDL_Quit();
    return 0;
}
