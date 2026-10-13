// TexturePack.h - texture dumping and replacement packs (HD textures).
//
// A texture is identified by a hash of its decoded BGRA texels (and size), so the key does not
// depend on file names, load order or handles. With "texdump = 1" in graphics.ini every new
// texture is written once to <settings folder>/texdump/ddg_<w>x<h>_<hash>.png. PNGs placed under
// <settings folder>/textures/ (any subfolder, any size, the name ending in the 16-digit hash)
// replace the texture with that hash. So do BC7-compressed DDS files (DX10 header, with their mip
// levels; recomp/tools/texpack_bc7.py), which are preferred to a PNG of the same hash when the GPU
// supports BC7: they need no decoding and stay compressed on the GPU; "texpack = 0" turns them off. Replacements are decoded on
// demand by worker threads, so the game never waits for them: until one is ready the original is
// drawn. Decoded images are kept for reuse up to "texpackmem" MB (default 1024), least recently
// used dropped first.
//
// Only BGRA textures are covered (texformat = 1): with P8 textures the palette is applied on the
// GPU, so the texels on the CPU are indices.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace texpack {

struct Image {
    uint32_t w = 0, h = 0;
    bool bc7 = false;               // BC7 blocks of `levels` mip levels, else BGRA (one level)
    uint32_t levels = 1;
    std::vector<uint8_t> data;      // BGRA: w * h * 4, rows top first; BC7: the levels one after another
};

// bytes of mip level `level` of a BC7 image
inline uint32_t BC7LevelSize(uint32_t w, uint32_t h, uint32_t level)
{
    uint32_t lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1;
    return ((lw + 3) / 4) * ((lh + 3) / 4) * 16;
}

// prefDir ends with a separator. Does nothing unless dumping is on or the textures folder has PNGs.
void Init(const std::string &prefDir, bool dump, bool load, uint32_t memMB, bool bc7);
void Term();

bool Active();                      // hashing is needed (dumping, or a pack is loaded)
uint64_t Hash(const uint8_t *bgra, uint32_t w, uint32_t h);
void Dump(uint64_t hash, const uint8_t *bgra, uint32_t w, uint32_t h);     // once per hash, if dumping
bool Has(uint64_t hash);                                                  // the pack has a replacement
// The decoded replacement, or nullptr: none, or not decoded yet (then it is queued; ask again later).
std::shared_ptr<const Image> Find(uint64_t hash);

} // namespace texpack
