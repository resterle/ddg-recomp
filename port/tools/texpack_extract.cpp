// texpack_extract - write every texture record of the game's cddata as a texture-pack PNG, named
// like the runtime dump (texdump = 1): <out>/<cddata subfolder>/ddg_<w>x<h>_<hash>.png (the
// subfolder, e.g. 2d, font, 3d, bg, tells interface graphics from scenery). The texels are decoded as
// CTexture::LoadImage does with texformat = 1 (power-of-two size, the texsize limit, zero padding),
// so the hash matches the one the game computes. Upscale the PNGs and put them into the
// textures folder next to graphics.ini.
//
//   texpack_extract <cddata folder> <out folder> [texsize 0-3, default 2]

#include "../src/unb/Texture.h"
#include "../src/unb/TexturePack.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

static std::string g_out, g_root, g_sub;
static uint32_t g_max = 1024;
static std::unordered_set<uint64_t> g_seen;
static int g_files, g_records, g_written;

static uint32_t PowerOfTwo(uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

static bool Plausible(const TXMHEADER *p, size_t avail)
{
    if (avail < sizeof(TXMHEADER)) return false;
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

// as CTexture::LoadImage
static void Extract(const TXMHEADER *p)
{
    g_records++;
    uint32_t pw = PowerOfTwo((uint32_t)p->width), ph = PowerOfTwo((uint32_t)p->height);
    uint32_t tw = SDL_min(pw, g_max), th = SDL_min(ph, g_max);
    int sx = (int)(pw / tw), sy = (int)(ph / th);
    uint8_t clut[256 * 4];
    const uint8_t *pClut = nullptr;
    if (ps2::IsIndexed(p->psm) && p->clutState != 0xff) {
        ps2::ConvertClut(ps2::ClutData(p), SDL_min(p->clutWidth * p->clutHeight, 256), clut);
        pClut = clut;
    }
    std::vector<uint8_t> bgra((size_t)tw * th * 4, 0);
    if (!ps2::Decode(ps2::PixelData(p), ps2::BitsPerPixel(p->psm), p->width, p->height, 0, 0, sx, sy, pClut,
                     TEXFMT_BGRA8, bgra.data(), tw * 4))
        return;
    uint64_t hash = texpack::Hash(bgra.data(), tw, th);
    if (!g_seen.insert(hash).second) return;
    char name[64];
    SDL_snprintf(name, sizeof(name), "ddg_%ux%u_%016llx.png", tw, th, (unsigned long long)hash);
    std::string dir = g_out + g_sub;
    SDL_CreateDirectory(dir.c_str());
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)tw, (int)th, SDL_PIXELFORMAT_BGRA32, bgra.data(), (int)tw * 4);
    if (s && SDL_SavePNG(s, (dir + name).c_str())) g_written++;
    SDL_DestroySurface(s);
}

static void ExtractFile(const std::string &path)
{
    size_t size = 0;
    auto *data = static_cast<uint8_t *>(SDL_LoadFile(path.c_str(), &size));
    if (!data) return;
    if (size >= 8 && memcmp(data, "DAT", 4) == 0) {
        // DAT container: "DAT\0", entry count, then (offset, size) pairs
        uint32_t count;
        memcpy(&count, data + 4, 4);
        for (uint32_t i = 0; i < count && 8 + 8 * (size_t)(i + 1) <= size; i++) {
            uint32_t off, len;
            memcpy(&off, data + 8 + 8 * i, 4);
            memcpy(&len, data + 12 + 8 * i, 4);
            if ((size_t)off + len > size) continue;
            auto *p = reinterpret_cast<const TXMHEADER *>(data + off);
            if (Plausible(p, len)) { Extract(p); g_files++; }
        }
    } else if (Plausible(reinterpret_cast<const TXMHEADER *>(data), size)) {
        Extract(reinterpret_cast<const TXMHEADER *>(data));
        g_files++;
    }
    SDL_free(data);
}

static SDL_EnumerationResult SDLCALL Walk(void *, const char *dir, const char *name)
{
    std::string path = std::string(dir) + name;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) SDL_EnumerateDirectory((path + "/").c_str(), Walk, nullptr);
    else if (info.type == SDL_PATHTYPE_FILE) {
        std::string rel = path.substr(g_root.size());           // first folder below cddata
        size_t slash = rel.find('/');
        g_sub = slash == std::string::npos ? "misc/" : rel.substr(0, slash + 1);
        ExtractFile(path);
    }
    return SDL_ENUM_CONTINUE;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: texpack_extract <cddata folder> <out folder> [texsize 0-3]\n"); return 2; }
    static const uint32_t sizes[4] = {256, 512, 1024, 1024};
    if (argc > 3) g_max = sizes[SDL_clamp(atoi(argv[3]), 0, 3)];
    g_out = std::string(argv[2]) + "/";
    SDL_CreateDirectory(g_out.c_str());
    g_root = std::string(argv[1]) + "/";
    SDL_EnumerateDirectory(g_root.c_str(), Walk, nullptr);
    printf("%d texture records, %d distinct textures written to %s\n", g_records, g_written, argv[2]);
    return 0;
}
