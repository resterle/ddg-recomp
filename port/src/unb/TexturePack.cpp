// TexturePack.cpp - see TexturePack.h.

#include "TexturePack.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace texpack {

namespace {

struct Pack {
    bool dump = false;
    bool bc7 = false;                                               // DDS files usable
    std::string dumpDir;
    std::unordered_map<uint64_t, std::string> files;               // hash -> PNG path, read-only after Init

    // decoded replacements, most recently used first, up to cacheLimit bytes
    struct Entry { std::shared_ptr<const Image> img; std::list<uint64_t>::iterator lru; };
    std::unordered_map<uint64_t, Entry> cache;
    std::list<uint64_t> lru;
    size_t cacheBytes = 0, cacheLimit = 0;
    std::unordered_set<uint64_t> bad;                               // failed to decode
    std::unordered_set<uint64_t> requested;                         // queued or being decoded
    std::deque<uint64_t> queue;
    std::condition_variable queueCv;
    std::mutex lock;                                                // all of the above

    std::vector<std::thread> workers;
    bool stop = false;

    // dump writer: PNG encoding is slow, so it runs on its own thread
    std::thread writer;
    std::mutex dumpLock;
    std::condition_variable dumpCv;
    std::deque<std::pair<std::string, Image>> dumpQueue;
    std::unordered_set<uint64_t> dumped;
    bool writerExit = false;
};

Pack *p = nullptr;

// The 16 hex digits at the end of a file name's stem, e.g. "ddg_256x256_0123456789abcdef.png"
// (or .dds when dds is set).
bool ParseHash(const char *name, uint64_t &hash, bool *dds = nullptr)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot - name < 16) return false;
    bool isDds = SDL_strcasecmp(dot, ".dds") == 0;
    if (SDL_strcasecmp(dot, ".png") != 0 && !(isDds && dds)) return false;
    if (dds) *dds = isDds;
    hash = 0;
    for (const char *c = dot - 16; c < dot; c++) {
        int v = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
              : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
        if (v < 0) return false;
        hash = hash << 4 | (uint64_t)v;
    }
    return true;
}

SDL_EnumerationResult SDLCALL Scan(void *, const char *dir, const char *name)
{
    std::string path = std::string(dir) + name;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        SDL_EnumerateDirectory((path + "/").c_str(), Scan, nullptr);
    } else if (uint64_t hash = 0; info.type == SDL_PATHTYPE_FILE) {
        bool dds = false;
        if (!ParseHash(name, hash, &dds) || (dds && !p->bc7)) return SDL_ENUM_CONTINUE;
        auto f = p->files.find(hash);
        bool haveDds = f != p->files.end() && f->second.size() > 4 && SDL_strcasecmp(f->second.c_str() + f->second.size() - 4, ".dds") == 0;
        if (dds || !haveDds) p->files[hash] = path;                // a DDS wins over a PNG
    }
    return SDL_ENUM_CONTINUE;
}

// BC7 DDS with a DX10 header: the blocks of all mip levels are read as they are.
std::shared_ptr<const Image> LoadDDS(const std::string &path)
{
    size_t size = 0;
    auto *f = static_cast<uint8_t *>(SDL_LoadFile(path.c_str(), &size));
    if (!f) { SDL_Log("texture pack: %s: %s", path.c_str(), SDL_GetError()); return nullptr; }
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, f + o, 4); return v; };
    const size_t hdr = 4 + 124 + 20;
    std::shared_ptr<Image> img;
    if (size >= hdr && memcmp(f, "DDS ", 4) == 0 && memcmp(f + 84, "DX10", 4) == 0 && u32(128) == 98 /* BC7_UNORM */ &&
        u32(140) == 1) {
        img = std::make_shared<Image>();
        img->bc7 = true;
        img->h = u32(12);
        img->w = u32(16);
        img->levels = SDL_max(u32(28), 1u);
        size_t need = 0;
        for (uint32_t l = 0; l < img->levels; l++) need += BC7LevelSize(img->w, img->h, l);
        if (img->w == 0 || img->h == 0 || img->w >> (img->levels - 1) == 0 || hdr + need > size) img = nullptr;
        else img->data.assign(f + hdr, f + hdr + need);
    }
    if (!img) SDL_Log("texture pack: %s: not a BC7 DDS file", path.c_str());
    SDL_free(f);
    return img;
}

std::shared_ptr<const Image> Decode(const std::string &path)
{
    if (path.size() > 4 && SDL_strcasecmp(path.c_str() + path.size() - 4, ".dds") == 0) return LoadDDS(path);
    SDL_Surface *s = SDL_LoadPNG(path.c_str());
    if (!s) {
        SDL_Log("texture pack: %s: %s", path.c_str(), SDL_GetError());
        return nullptr;
    }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(s);
    if (!c) return nullptr;
    auto img = std::make_shared<Image>();
    img->w = (uint32_t)c->w;
    img->h = (uint32_t)c->h;
    img->data.resize((size_t)img->w * img->h * 4);
    for (uint32_t y = 0; y < img->h; y++)
        memcpy(&img->data[(size_t)y * img->w * 4], (const uint8_t *)c->pixels + (size_t)y * c->pitch, (size_t)img->w * 4);
    SDL_DestroySurface(c);
    return img;
}

// Adds a decoded image to the cache, dropping the least recently used ones over the budget.
// Caller holds p->lock.
void Keep(uint64_t hash, const std::shared_ptr<const Image> &img)
{
    if (p->cache.count(hash)) return;
    p->lru.push_front(hash);
    p->cache[hash] = {img, p->lru.begin()};
    p->cacheBytes += img->data.size();
    while (p->cacheBytes > p->cacheLimit && p->lru.size() > 1) {
        auto old = p->cache.find(p->lru.back());
        p->cacheBytes -= old->second.img->data.size();
        p->cache.erase(old);
        p->lru.pop_back();
    }
}

// Decodes requested replacements in the order they were asked for.
void WorkerMain()
{
    std::unique_lock<std::mutex> l(p->lock);
    for (;;) {
        p->queueCv.wait(l, [] { return p->stop || !p->queue.empty(); });
        if (p->stop) return;
        uint64_t hash = p->queue.front();
        p->queue.pop_front();
        l.unlock();
        auto img = Decode(p->files[hash]);
        l.lock();
        p->requested.erase(hash);
        if (img) Keep(hash, img);
        else p->bad.insert(hash);
    }
}

void WriterMain()
{
    for (;;) {
        std::pair<std::string, Image> job;
        {
            std::unique_lock<std::mutex> l(p->dumpLock);
            p->dumpCv.wait(l, [] { return p->writerExit || !p->dumpQueue.empty(); });
            if (p->dumpQueue.empty()) return;
            job = std::move(p->dumpQueue.front());
            p->dumpQueue.pop_front();
        }
        Image &img = job.second;
        if (SDL_Surface *s = SDL_CreateSurfaceFrom((int)img.w, (int)img.h, SDL_PIXELFORMAT_BGRA32, img.data.data(), (int)img.w * 4)) {
            if (!SDL_SavePNG(s, job.first.c_str())) SDL_Log("texture dump: %s: %s", job.first.c_str(), SDL_GetError());
            SDL_DestroySurface(s);
        }
    }
}

} // namespace

void Init(const std::string &prefDir, bool dump, bool load, uint32_t memMB, bool bc7)
{
    Term();
    p = new Pack();
    p->bc7 = bc7;
    p->cacheLimit = (size_t)(memMB ? memMB : 1024) << 20;
    if (load) {
        std::string dir = prefDir + "textures/";
        SDL_PathInfo info;
        if (SDL_GetPathInfo(dir.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            SDL_EnumerateDirectory(dir.c_str(), Scan, nullptr);
            SDL_Log("texture pack: %zu replacements in %s", p->files.size(), dir.c_str());
        }
    }
    if (dump) {
        p->dump = true;
        p->dumpDir = prefDir + "texdump/";
        SDL_CreateDirectory(p->dumpDir.c_str());
        // textures dumped in earlier sessions are not written again
        SDL_EnumerateDirectory(p->dumpDir.c_str(), [](void *, const char *, const char *name) {
            if (uint64_t hash; ParseHash(name, hash)) p->dumped.insert(hash);
            return SDL_ENUM_CONTINUE;
        }, nullptr);
        p->writer = std::thread(WriterMain);
        SDL_Log("texture dump: writing new textures to %s", p->dumpDir.c_str());
    }
    if (!p->files.empty()) {
        // the game's own loading is mostly one thread; leave a few cores to it
        int n = SDL_clamp(SDL_GetNumLogicalCPUCores() - 2, 1, 8);
        for (int i = 0; i < n; i++) p->workers.emplace_back(WorkerMain);
    }
    if (!p->dump && p->files.empty()) Term();
}

void Term()
{
    if (!p) return;
    {
        std::lock_guard<std::mutex> l(p->lock);
        p->stop = true;
    }
    p->queueCv.notify_all();
    for (std::thread &t : p->workers) t.join();
    {
        std::lock_guard<std::mutex> l(p->dumpLock);
        p->writerExit = true;
    }
    p->dumpCv.notify_all();
    if (p->writer.joinable()) p->writer.join();          // finishes the queued dumps
    delete p;
    p = nullptr;
}

bool Active() { return p != nullptr; }

uint64_t Hash(const uint8_t *data, uint32_t w, uint32_t h)
{
    // four independent multiply-xorshift lanes over 8-byte words, then mixed (stable across runs and
    // platforms; little-endian loads)
    const uint64_t k = 0x9e3779b97f4a7c15ull, m = 0xbf58476d1ce4e5b9ull;
    uint64_t a = k ^ w, b = k ^ ((uint64_t)h << 32), c = ~k, d = k * 3;
    size_t n = (size_t)w * h * 4, i = 0;
    auto load = [&](size_t o) { uint64_t v; memcpy(&v, data + o, 8); return v; };
    for (; i + 32 <= n; i += 32) {
        a = (a ^ load(i)) * m;      a ^= a >> 31;
        b = (b ^ load(i + 8)) * m;  b ^= b >> 31;
        c = (c ^ load(i + 16)) * m; c ^= c >> 31;
        d = (d ^ load(i + 24)) * m; d ^= d >> 31;
    }
    for (; i < n; i++) { a = (a ^ data[i]) * m; a ^= a >> 31; }
    uint64_t x = a ^ (b * k) ^ (c * m) ^ (d * (k + m)) ^ n;
    x ^= x >> 30; x *= m; x ^= x >> 27; x *= 0x94d049bb133111ebull; x ^= x >> 31;
    return x;
}

void Dump(uint64_t hash, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    if (!p || !p->dump) return;
    std::lock_guard<std::mutex> l(p->dumpLock);
    if (!p->dumped.insert(hash).second) return;
    char name[64];
    SDL_snprintf(name, sizeof(name), "ddg_%ux%u_%016llx.png", w, h, (unsigned long long)hash);
    Image img;
    img.w = w;
    img.h = h;
    img.data.assign(bgra, bgra + (size_t)w * h * 4);
    p->dumpQueue.emplace_back(p->dumpDir + name, std::move(img));
    p->dumpCv.notify_one();
}

bool Has(uint64_t hash) { return p && p->files.count(hash); }

std::shared_ptr<const Image> Find(uint64_t hash)
{
    if (!p || !p->files.count(hash)) return nullptr;
    std::lock_guard<std::mutex> l(p->lock);
    auto c = p->cache.find(hash);
    if (c != p->cache.end()) {
        p->lru.splice(p->lru.begin(), p->lru, c->second.lru);
        return c->second.img;
    }
    if (!p->bad.count(hash) && p->requested.insert(hash).second) {
        p->queue.push_back(hash);
        p->queueCv.notify_one();
    }
    return nullptr;
}

} // namespace texpack
