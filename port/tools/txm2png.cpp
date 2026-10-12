// txm2png - decode the game's PS2 texture records (TXM files and DAT containers) to PNG using
// the same ps2:: decoder as the CTexture port. No GPU needed.
//
//   txm2png <file.TXM | container.dat> [out_dir]

#include "../src/unb/Texture.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <string>
#include <vector>

static bool Plausible(const TXMHEADER *p, size_t avail)
{
    int bpp = ps2::BitsPerPixel(p->psm);
    if (bpp != 4 && bpp != 8 && bpp != 32) return false;
    if (p->width <= 0 || p->height <= 0 || p->width > 2048 || p->height > 2048) return false;
    if (ps2::IsIndexed(p->psm)) {
        int n = p->clutWidth * p->clutHeight;
        if (p->clutState != 0x00 || (n != 16 && n != 256)) return false;
    } else if (p->clutState != 0xff) {
        return false;
    }
    return ps2::RecordSize(p) <= avail;
}

static bool Save(const TXMHEADER *p, const std::string &path)
{
    uint8_t clut[256 * 4] = {};
    if (ps2::IsIndexed(p->psm)) ps2::ConvertClut(ps2::ClutData(p), SDL_min(p->clutWidth * p->clutHeight, 256), clut);
    std::vector<uint8_t> bgra((size_t)p->width * p->height * 4);
    if (!ps2::Decode(ps2::PixelData(p), ps2::BitsPerPixel(p->psm), p->width, p->height, 0, 0, 1, 1, clut,
                     TEXFMT_BGRA8, bgra.data(), (uint32_t)p->width * 4))
        return false;
    SDL_Surface *s = SDL_CreateSurfaceFrom(p->width, p->height, SDL_PIXELFORMAT_BGRA32, bgra.data(), p->width * 4);
    bool ok = s && SDL_SavePNG(s, path.c_str());
    SDL_DestroySurface(s);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: txm2png <file> [out_dir]\n"); return 2; }
    std::string out = argc > 2 ? argv[2] : ".";
    SDL_CreateDirectory(out.c_str());
    size_t size = 0;
    auto *data = static_cast<uint8_t *>(SDL_LoadFile(argv[1], &size));
    if (!data) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }

    std::string base = argv[1];
    base = base.substr(base.find_last_of("/\\") + 1);
    base = base.substr(0, base.find_last_of('.'));

    int written = 0, skipped = 0;
    if (size >= 8 && memcmp(data, "DAT", 4) == 0) {
        // DAT container: "DAT\0", entry count, then (offset, size) pairs.
        uint32_t count;
        memcpy(&count, data + 4, 4);
        for (uint32_t i = 0; i < count && 8 + 8 * (i + 1) <= size; i++) {
            uint32_t off, len;
            memcpy(&off, data + 8 + 8 * i, 4);
            memcpy(&len, data + 12 + 8 * i, 4);
            if (off + sizeof(TXMHEADER) > size || off + len > size) { skipped++; continue; }
            auto *p = reinterpret_cast<const TXMHEADER *>(data + off);
            if (!Plausible(p, len)) { skipped++; continue; }
            char name[64];
            SDL_snprintf(name, sizeof(name), "_%03u_%dx%d.png", i, p->width, p->height);
            if (Save(p, out + "/" + base + name)) written++; else skipped++;
        }
    } else if (size >= sizeof(TXMHEADER) && Plausible(reinterpret_cast<const TXMHEADER *>(data), size)) {
        written += Save(reinterpret_cast<const TXMHEADER *>(data), out + "/" + base + ".png");
    } else {
        skipped++;
    }
    printf("%s: %d written, %d skipped (not texture records)\n", argv[1], written, skipped);
    SDL_free(data);
    return written ? 0 : 1;
}
