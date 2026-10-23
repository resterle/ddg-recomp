// dsound.cpp - DirectSound 8 and the mmio WAV file API for the translated sound code
//
// The game's own CSound/CSample (0x407ea0-0x40a2e0) and CWave (0x412d10-0x413600) are translated
// unchanged; this file supplies what they call:
// - DSOUND #11 (DirectSoundCreate8) returning a fake IDirectSound8 whose buffers are mixed here.
//   COM objects are small guest structures [vtable][index]; the vtables point at host functions
//   (rt_host_fn). Buffer data lives in guest memory, because the game writes it through Lock.
// - IDirectSoundNotify: streamed sounds (long announcements, loops) are refilled by game threads
//   that wait on events; the mixer signals those events when the play position passes the
//   registered offsets, as DirectSound does.
// - WINMM mmio*: chunk-based RIFF reading (and writing, used only by debug code) over host files.
//
// A host mixer thread keeps about 40 ms of audio queued in an SDL3 audio stream. All guest-facing
// calls run under the runtime lock; the mixer only takes `g_mix`. It signals notification events
// after releasing `g_mix`, taking the runtime lock (rt_lock_host), so lock order is always
// runtime lock -> g_mix on guest threads and never both on the mixer thread.

#include "runtime.h"
#include <SDL3/SDL.h>
#include <cstdlib>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void rt_event_set(uint32_t h);

namespace {

constexpr uint32_t S_OK = 0, E_FAIL = 0x80004005u, E_NOINTERFACE = 0x80004002u, E_INVALIDARG = 0x80070057u,
                   E_NOTIMPL = 0x80004001u;
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 1, DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4;
constexpr uint32_t DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2, DSBPN_OFFSETSTOP = 0xffffffffu;
constexpr int kOutRate = 48000;

struct Data {                   // sample memory, shared by duplicated buffers
    uint32_t addr = 0, size = 0;
    int refs = 1;
};

struct Buffer {
    uint32_t obj = 0, notifyObj = 0;    // guest COM objects
    int refs = 1;
    bool primary = false;
    Data *data = nullptr;
    uint32_t flags = 0;
    // format
    uint16_t channels = 1, bits = 16, blockAlign = 2;
    uint32_t rate = 22050;
    // playback (mixer state, guarded by g_mix)
    bool playing = false, looping = false;
    double pos = 0;                     // in frames
    int32_t volume = 0, pan = 0;        // hundredths of dB
    uint32_t freq = 0;                  // 0 = the format's rate
    float gainL = 1, gainR = 1;
    std::vector<std::pair<uint32_t, uint32_t>> notify;     // byte offset, event handle

    uint32_t Frames() const { return blockAlign ? data->size / blockAlign : 0; }
};

std::mutex g_mix;
const bool g_trace = getenv("RECOMP_TRACE_SOUND") != nullptr;
std::vector<Buffer *> g_bufs;           // index = id stored in the guest object
uint32_t g_dsObj, g_dsVtbl, g_bufVtbl, g_notifyVtbl;
std::thread g_mixer;
std::atomic<bool> g_mixerRun;
SDL_AudioStream *g_stream;

Buffer *BufOf(uint32_t obj)
{
    uint32_t id = RD32(obj + 4);
    return id < g_bufs.size() ? g_bufs[id] : nullptr;
}

void UpdateGain(Buffer *b)
{
    float g = b->volume <= -10000 ? 0.0f : powf(10.0f, b->volume / 2000.0f);
    b->gainL = b->gainR = g;
    if (b->pan < 0) b->gainR *= powf(10.0f, b->pan / 2000.0f);
    if (b->pan > 0) b->gainL *= powf(10.0f, -b->pan / 2000.0f);
}

// ---------------------------------------------------------------------------------------------
// Mixer

inline float Sample(const uint8_t *p, uint32_t frame, int ch, const Buffer *b)
{
    const uint8_t *s = p + (size_t)frame * b->blockAlign;
    if (b->bits == 8) return ((int)s[ch] - 128) / 128.0f;
    int16_t v;
    memcpy(&v, s + 2 * ch, 2);
    return v / 32768.0f;
}

// Mixes `n` frames into `out` (stereo float); collects events to signal.
void Mix(float *out, int n, std::vector<uint32_t> &events)
{
    memset(out, 0, sizeof(float) * 2 * n);
    std::lock_guard<std::mutex> lock(g_mix);
    for (Buffer *b : g_bufs) {
        if (!b || !b->playing || b->primary) continue;
        uint32_t frames = b->Frames();
        if (!frames) { b->playing = false; continue; }
        const uint8_t *p = MEM + b->data->addr;
        double step = (double)(b->freq ? b->freq : b->rate) / kOutRate;
        double start = b->pos;
        bool stereo = b->channels >= 2, ended = false;
        double pos = b->pos;
        for (int i = 0; i < n; i++) {
            uint32_t f0 = (uint32_t)pos;
            uint32_t f1 = f0 + 1 < frames ? f0 + 1 : (b->looping ? 0 : f0);
            float t = (float)(pos - f0);
            float l = Sample(p, f0, 0, b) * (1 - t) + Sample(p, f1, 0, b) * t;
            float r = stereo ? Sample(p, f0, 1, b) * (1 - t) + Sample(p, f1, 1, b) * t : l;
            out[2 * i] += l * b->gainL;
            out[2 * i + 1] += r * b->gainR;
            pos += step;
            if (pos >= frames) {
                if (b->looping) pos -= frames;
                else { ended = true; break; }
            }
        }
        // notifications passed in [start, pos) (wrapping for loops)
        for (auto &nt : b->notify) {
            if (nt.first == DSBPN_OFFSETSTOP) { if (ended) events.push_back(nt.second); continue; }
            double f = (double)(nt.first / b->blockAlign);
            bool hit = ended ? f >= start
                     : pos >= start ? (f >= start && f < pos)
                                    : (f >= start || f < pos);
            if (hit) events.push_back(nt.second);
        }
        if (ended) { b->playing = false; b->pos = 0; }
        else b->pos = pos;
    }
}

void MixerThread()
{
    // queued audio: 40 ms on desktops, 80 ms on Linux handhelds (slow cores, busy game thread)
#ifdef __APPLE__
    constexpr int kChunk = 256, kTarget = kOutRate * 40 / 1000;
#else
    constexpr int kChunk = 256, kTarget = kOutRate * 80 / 1000;
#endif
    std::vector<float> buf(kChunk * 2);
    std::vector<uint32_t> events;
    uint64_t t0 = SDL_GetTicksNS(), mixed = 0;
    while (g_mixerRun) {
        bool more;
        if (g_stream) {
            more = SDL_GetAudioStreamQueued(g_stream) / (int)(2 * sizeof(float)) < kTarget;
        } else {                                    // no audio device: keep time anyway
            uint64_t due = (SDL_GetTicksNS() - t0) * kOutRate / 1000000000ull;
            more = mixed < due;
        }
        if (!more) { SDL_Delay(3); continue; }
        events.clear();
        Mix(buf.data(), kChunk, events);
        mixed += kChunk;
        for (float &x : buf)                        // soft limit above 0.75 instead of hard clipping
            if (x > 0.75f || x < -0.75f) {
                float a = fabsf(x), y = 0.75f + 0.25f * tanhf((a - 0.75f) * 4.0f);
                x = x < 0 ? -y : y;
            }
        if (g_stream) SDL_PutAudioStreamData(g_stream, buf.data(), (int)(buf.size() * sizeof(float)));
        // debug: RECOMP_SOUND_DUMP=<file> records the mix (raw 48 kHz stereo float32)
        static FILE *dump = getenv("RECOMP_SOUND_DUMP") ? fopen(getenv("RECOMP_SOUND_DUMP"), "wb") : nullptr;
        if (dump) fwrite(buf.data(), sizeof(float), buf.size(), dump);
        if (!events.empty()) {
            rt_lock_host();
            for (uint32_t h : events) rt_event_set(h);
            rt_unlock_host();
        }
    }
}

void StartMixer()
{
    if (g_mixerRun) return;
    if (!getenv("RECOMP_NOSOUND") && SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, kOutRate};
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (g_stream) SDL_ResumeAudioStreamDevice(g_stream);
    }
    fprintf(stderr, "sound: %s\n", g_stream ? SDL_GetCurrentAudioDriver() : "no audio device (silent)");
    g_mixerRun = true;
    g_mixer = std::thread(MixerThread);
    g_mixer.detach();
}

// ---------------------------------------------------------------------------------------------
// IDirectSoundBuffer8 / IDirectSoundNotify

uint32_t NewObject(uint32_t vtbl, uint32_t id)
{
    uint32_t o = rt_heap_alloc(8, 1);
    WR32(o, vtbl);
    WR32(o + 4, id);
    return o;
}

Buffer *NewBuffer(Data *data)
{
    auto *b = new Buffer();
    b->data = data;
    uint32_t id;
    {
        std::lock_guard<std::mutex> lock(g_mix);
        for (id = 0; id < g_bufs.size() && g_bufs[id]; id++) {}
        if (id == g_bufs.size()) g_bufs.push_back(nullptr);
        g_bufs[id] = b;
    }
    b->obj = NewObject(g_bufVtbl, id);
    return b;
}

void ReleaseBuffer(Buffer *b)
{
    if (--b->refs > 0) return;
    {
        std::lock_guard<std::mutex> lock(g_mix);
        g_bufs[RD32(b->obj + 4)] = nullptr;
    }
    if (--b->data->refs == 0) {
        if (b->data->addr) rt_heap_free(b->data->addr);
        delete b->data;
    }
    rt_heap_free(b->obj);
    if (b->notifyObj) rt_heap_free(b->notifyObj);
    delete b;
}

void buf_QueryInterface()
{
    Buffer *b = BufOf(ARG(0));
    uint32_t iid = RD32(ARG(1)), out = ARG(2);
    if (!b) RT_RETURN(E_FAIL, 12);
    if (iid == 0xb0210783u) {                       // IID_IDirectSoundNotify
        if (!b->notifyObj) b->notifyObj = NewObject(g_notifyVtbl, RD32(b->obj + 4));
        b->refs++;
        WR32(out, b->notifyObj);
        RT_RETURN(S_OK, 12);
    }
    if (iid == 0x279afa85u || iid == 0x6825a449u || iid == 0) {     // IDirectSoundBuffer(8), IUnknown
        b->refs++;
        WR32(out, b->obj);
        RT_RETURN(S_OK, 12);
    }
    WR32(out, 0);
    RT_RETURN(E_NOINTERFACE, 12);
}
void buf_AddRef() { Buffer *b = BufOf(ARG(0)); RT_RETURN(b ? ++b->refs : 0, 4); }
void buf_Release()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(0, 4);
    int left = b->refs - 1;
    ReleaseBuffer(b);
    RT_RETURN(left, 4);
}
void buf_GetCaps()
{
    Buffer *b = BufOf(ARG(0));
    uint32_t c = ARG(1);
    if (!b || !c) RT_RETURN(E_INVALIDARG, 8);
    WR32(c + 4, b->flags);
    WR32(c + 8, b->data->size);
    WR32(c + 12, 0);
    WR32(c + 16, 0);
    RT_RETURN(S_OK, 8);
}
void buf_GetCurrentPosition()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 12);
    uint32_t play, write;
    {
        std::lock_guard<std::mutex> lock(g_mix);
        play = (uint32_t)b->pos * b->blockAlign;
        uint32_t ahead = (b->freq ? b->freq : b->rate) / 100 * b->blockAlign;     // ~10 ms
        write = b->data->size ? (play + ahead) % b->data->size : 0;
    }
    if (ARG(1)) WR32(ARG(1), play);
    if (ARG(2)) WR32(ARG(2), write);
    RT_RETURN(S_OK, 12);
}
void buf_GetFormat()
{
    Buffer *b = BufOf(ARG(0));
    uint32_t w = ARG(1), size = ARG(2), written = ARG(3);
    if (!b) RT_RETURN(E_FAIL, 16);
    if (w && size >= 16) {
        WR16(w, 1); WR16(w + 2, b->channels); WR32(w + 4, b->rate);
        WR32(w + 8, b->rate * b->blockAlign); WR16(w + 12, b->blockAlign); WR16(w + 14, b->bits);
        if (size >= 18) WR16(w + 16, 0);
    }
    if (written) WR32(written, 18);
    RT_RETURN(S_OK, 16);
}
void buf_GetVolume() { Buffer *b = BufOf(ARG(0)); if (b && ARG(1)) WR32(ARG(1), (uint32_t)b->volume); RT_RETURN(b ? S_OK : E_FAIL, 8); }
void buf_GetPan() { Buffer *b = BufOf(ARG(0)); if (b && ARG(1)) WR32(ARG(1), (uint32_t)b->pan); RT_RETURN(b ? S_OK : E_FAIL, 8); }
void buf_GetFrequency()
{
    Buffer *b = BufOf(ARG(0));
    if (b && ARG(1)) WR32(ARG(1), b->freq ? b->freq : b->rate);
    RT_RETURN(b ? S_OK : E_FAIL, 8);
}
void buf_GetStatus()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 8);
    uint32_t s;
    {
        std::lock_guard<std::mutex> lock(g_mix);
        s = b->playing ? DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0) : 0;
    }
    if (ARG(1)) WR32(ARG(1), s);
    RT_RETURN(S_OK, 8);
}
void buf_Initialize() { RT_RETURN(0x88780082u, 12); }      // DSERR_ALREADYINITIALIZED
void buf_Lock()
{
    Buffer *b = BufOf(ARG(0));
    uint32_t off = ARG(1), bytes = ARG(2), p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6), flags = ARG(7);
    if (!b || !b->data->size) RT_RETURN(E_INVALIDARG, 32);
    uint32_t size = b->data->size;
    if (flags & DSBLOCK_FROMWRITECURSOR) {
        std::lock_guard<std::mutex> lock(g_mix);
        off = (uint32_t)b->pos * b->blockAlign;
    }
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
    if (off >= size || bytes > size) RT_RETURN(E_INVALIDARG, 32);
    uint32_t first = bytes < size - off ? bytes : size - off;
    static const bool verbose = g_trace && atoi(getenv("RECOMP_TRACE_SOUND")) > 1;
    if (verbose) fprintf(stderr, "sound: lock %u at %u, %u bytes (t=%.2f)\n", RD32(b->obj + 4), off, bytes, SDL_GetTicks() / 1000.0);
    WR32(p1, b->data->addr + off);
    WR32(n1, first);
    if (p2) WR32(p2, bytes > first ? b->data->addr : 0);
    if (n2) WR32(n2, bytes - first);
    RT_RETURN(S_OK, 32);
}
void buf_Play()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 16);
    std::lock_guard<std::mutex> lock(g_mix);
    b->playing = true;
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    if (g_trace) fprintf(stderr, "sound: play %u%s vol %d pan %d freq %u\n", RD32(b->obj + 4), b->looping ? " loop" : "",
                         b->volume, b->pan, b->freq);
    RT_RETURN(S_OK, 16);
}
void buf_SetCurrentPosition()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 8);
    std::lock_guard<std::mutex> lock(g_mix);
    uint32_t f = ARG(1) / b->blockAlign;
    b->pos = f < b->Frames() ? f : 0;
    RT_RETURN(S_OK, 8);
}
void buf_SetFormat() { RT_RETURN(S_OK, 8); }       // primary buffer format: the mixer converts anyway
void buf_SetVolume()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 8);
    std::lock_guard<std::mutex> lock(g_mix);
    b->volume = SDL_clamp((int32_t)ARG(1), -10000, 0);
    UpdateGain(b);
    RT_RETURN(S_OK, 8);
}
void buf_SetPan()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 8);
    std::lock_guard<std::mutex> lock(g_mix);
    b->pan = SDL_clamp((int32_t)ARG(1), -10000, 10000);
    UpdateGain(b);
    RT_RETURN(S_OK, 8);
}
void buf_SetFrequency()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 8);
    std::lock_guard<std::mutex> lock(g_mix);
    b->freq = ARG(1);
    RT_RETURN(S_OK, 8);
}
void buf_Stop()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 4);
    std::vector<uint32_t> events;
    {
        std::lock_guard<std::mutex> lock(g_mix);
        if (b->playing)
            for (auto &nt : b->notify) if (nt.first == DSBPN_OFFSETSTOP) events.push_back(nt.second);
        b->playing = false;
    }
    for (uint32_t h : events) rt_event_set(h);     // we hold the runtime lock here
    RT_RETURN(S_OK, 4);
}
void buf_Unlock() { RT_RETURN(BufOf(ARG(0)) ? S_OK : E_FAIL, 20); }
void buf_Restore() { RT_RETURN(S_OK, 4); }
void buf_SetFX() { RT_RETURN(E_NOTIMPL, 16); }
void buf_AcquireResources() { RT_RETURN(S_OK, 16); }
void buf_GetObjectInPath() { RT_RETURN(E_NOTIMPL, 20); }

void notify_QueryInterface()
{
    Buffer *b = BufOf(ARG(0));
    if (!b) RT_RETURN(E_FAIL, 12);
    b->refs++;
    WR32(ARG(2), RD32(ARG(1)) == 0xb0210783u ? b->notifyObj : b->obj);
    RT_RETURN(S_OK, 12);
}
void notify_SetNotificationPositions()
{
    Buffer *b = BufOf(ARG(0));
    uint32_t n = ARG(1), arr = ARG(2);
    if (!b) RT_RETURN(E_FAIL, 12);
    std::lock_guard<std::mutex> lock(g_mix);
    b->notify.clear();
    for (uint32_t i = 0; i < n; i++) b->notify.push_back({RD32(arr + 8 * i), RD32(arr + 8 * i + 4)});
    RT_RETURN(S_OK, 12);
}

// ---------------------------------------------------------------------------------------------
// IDirectSound8

void ds_QueryInterface() { WR32(ARG(2), g_dsObj); RT_RETURN(S_OK, 12); }
void ds_AddRef() { RT_RETURN(1, 4); }
void ds_Release() { RT_RETURN(0, 4); }
void ds_CreateSoundBuffer()
{
    uint32_t desc = ARG(1), out = ARG(2);
    if (!desc || !out) RT_RETURN(E_INVALIDARG, 16);
    uint32_t flags = RD32(desc + 4), bytes = RD32(desc + 8), wfx = RD32(desc + 16);
    auto *data = new Data();
    Buffer *b = NewBuffer(data);
    b->flags = flags;
    b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (!b->primary) {
        if (!wfx || !bytes) { ReleaseBuffer(b); RT_RETURN(E_INVALIDARG, 16); }
        b->channels = RD16(wfx + 2);
        b->rate = RD32(wfx + 4);
        b->blockAlign = RD16(wfx + 12);
        b->bits = RD16(wfx + 14);
        if (RD16(wfx) != 1 || !b->blockAlign || (b->bits != 8 && b->bits != 16)) {
            fprintf(stderr, "sound: unsupported format %u ch %u bits %u\n", RD16(wfx), b->channels, b->bits);
            ReleaseBuffer(b);
            RT_RETURN(0x88780032u, 16);             // DSERR_BADFORMAT
        }
        data->size = bytes;
        data->addr = rt_heap_alloc(bytes, 0);
        if (g_trace) fprintf(stderr, "sound: buffer %u: %u Hz %u ch %u bit, %u bytes, flags %x\n", RD32(b->obj + 4),
                             b->rate, b->channels, b->bits, bytes, flags);
        memset(MEM + data->addr, b->bits == 8 ? 0x80 : 0, bytes);
    }
    WR32(out, b->obj);
    RT_RETURN(S_OK, 16);
}
void ds_GetCaps() { RT_RETURN(S_OK, 8); }
void ds_DuplicateSoundBuffer()
{
    Buffer *src = BufOf(ARG(1));
    if (!src || src->primary) RT_RETURN(E_INVALIDARG, 12);
    src->data->refs++;
    Buffer *b = NewBuffer(src->data);
    b->flags = src->flags;
    b->channels = src->channels; b->bits = src->bits; b->blockAlign = src->blockAlign; b->rate = src->rate;
    {
        std::lock_guard<std::mutex> lock(g_mix);
        b->volume = src->volume; b->pan = src->pan; b->freq = src->freq;
        UpdateGain(b);
    }
    WR32(ARG(2), b->obj);
    RT_RETURN(S_OK, 12);
}
void ds_SetCooperativeLevel() { RT_RETURN(S_OK, 12); }
void ds_Compact() { RT_RETURN(S_OK, 4); }
void ds_GetSpeakerConfig() { if (ARG(1)) WR32(ARG(1), 4); RT_RETURN(S_OK, 8); }     // stereo
void ds_SetSpeakerConfig() { RT_RETURN(S_OK, 8); }
void ds_Initialize() { RT_RETURN(S_OK, 8); }
void ds_VerifyCertification() { if (ARG(1)) WR32(ARG(1), 1); RT_RETURN(S_OK, 8); }

uint32_t MakeVtable(std::initializer_list<void (*)(void)> fns)
{
    uint32_t v = rt_heap_alloc((uint32_t)fns.size() * 4, 1), i = 0;
    for (auto f : fns) WR32(v + 4 * i++, rt_host_fn(f));
    return v;
}

void imp_DirectSoundCreate8()
{
    if (!g_dsObj) {
        g_dsVtbl = MakeVtable({ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps,
                               ds_DuplicateSoundBuffer, ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig,
                               ds_SetSpeakerConfig, ds_Initialize, ds_VerifyCertification});
        g_bufVtbl = MakeVtable({buf_QueryInterface, buf_AddRef, buf_Release, buf_GetCaps, buf_GetCurrentPosition,
                                buf_GetFormat, buf_GetVolume, buf_GetPan, buf_GetFrequency, buf_GetStatus,
                                buf_Initialize, buf_Lock, buf_Play, buf_SetCurrentPosition, buf_SetFormat,
                                buf_SetVolume, buf_SetPan, buf_SetFrequency, buf_Stop, buf_Unlock, buf_Restore,
                                buf_SetFX, buf_AcquireResources, buf_GetObjectInPath});
        g_notifyVtbl = MakeVtable({notify_QueryInterface, buf_AddRef, buf_Release, notify_SetNotificationPositions});
        g_dsObj = NewObject(g_dsVtbl, 0);
        StartMixer();
    }
    if (!ARG(1)) RT_RETURN(E_INVALIDARG, 12);
    WR32(ARG(1), g_dsObj);
    RT_RETURN(S_OK, 12);
}

// ---------------------------------------------------------------------------------------------
// WINMM mmio: RIFF chunk I/O on host files

constexpr uint32_t kMmioBase = 0x00a00000u;
constexpr uint32_t MMIOERR_CANNOTSEEK = 266, MMIOERR_CHUNKNOTFOUND = 265, MMIOERR_CANNOTWRITE = 264;
constexpr uint32_t FOURCC_RIFF = 0x46464952u, FOURCC_LIST = 0x5453494cu;
std::vector<FILE *> g_mmio;

FILE *MmioFile(uint32_t h)
{
    uint32_t i = h - kMmioBase;
    return i < g_mmio.size() ? g_mmio[i] : nullptr;
}

void imp_mmioOpenA()
{
    uint32_t name = ARG(0), flags = ARG(2);
    if (!name) RT_RETURN(0, 12);                    // memory files (resources): not used by the data
    char win[512], host[1200];
    rt_strget(name, win, sizeof(win));
    rt_map_path(win, host, sizeof(host));
    const char *mode = (flags & 0x1000) ? "w+b" : (flags & 3) ? "r+b" : "rb";    // MMIO_CREATE, WRITE/READWRITE
    FILE *f = fopen(host, mode);
    if (g_trace) fprintf(stderr, "sound: open %s%s\n", win, f ? "" : " (failed)");
    if (!f) RT_RETURN(0, 12);
    size_t i = 0;
    while (i < g_mmio.size() && g_mmio[i]) i++;
    if (i == g_mmio.size()) g_mmio.push_back(nullptr);
    g_mmio[i] = f;
    RT_RETURN(kMmioBase + (uint32_t)i, 12);
}
void imp_mmioClose()
{
    FILE *f = MmioFile(ARG(0));
    if (f) { fclose(f); g_mmio[ARG(0) - kMmioBase] = nullptr; }
    RT_RETURN(0, 8);
}
void imp_mmioRead()
{
    FILE *f = MmioFile(ARG(0));
    if (!f) RT_RETURN(0xffffffffu, 12);
    rt_guest_commit(ARG(1), ARG(2));
    RT_RETURN((uint32_t)fread(MEM + ARG(1), 1, ARG(2), f), 12);
}
void imp_mmioWrite()
{
    FILE *f = MmioFile(ARG(0));
    if (!f) RT_RETURN(0xffffffffu, 12);
    RT_RETURN((uint32_t)fwrite(MEM + ARG(1), 1, ARG(2), f), 12);
}
void imp_mmioSeek()
{
    FILE *f = MmioFile(ARG(0));
    if (!f || fseek(f, (long)(int32_t)ARG(1), (int)ARG(2)) != 0) RT_RETURN(0xffffffffu, 12);
    RT_RETURN((uint32_t)ftell(f), 12);
}
// MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags
void imp_mmioDescend()
{
    FILE *f = MmioFile(ARG(0));
    uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!f || !ck) RT_RETURN(MMIOERR_CHUNKNOTFOUND, 16);
    uint32_t wantId = RD32(ck), wantType = RD32(ck + 8);
    uint32_t end = parent ? RD32(parent + 12) + RD32(parent + 4) : 0xffffffffu;
    for (;;) {
        long at = ftell(f);
        uint32_t hdr[3];
        if ((uint32_t)at + 8 > end || fread(hdr, 1, 8, f) != 8) RT_RETURN(MMIOERR_CHUNKNOTFOUND, 16);
        uint32_t id = hdr[0], size = hdr[1], type = 0;
        bool form = id == FOURCC_RIFF || id == FOURCC_LIST;
        if (form && fread(&type, 1, 4, f) != 4) RT_RETURN(MMIOERR_CHUNKNOTFOUND, 16);
        bool match = (flags & 0x10) ? id == wantId
                   : (flags & 0x20) ? id == FOURCC_RIFF && type == wantType
                   : (flags & 0x40) ? id == FOURCC_LIST && type == wantType
                   : true;
        if (match) {
            WR32(ck, id); WR32(ck + 4, size); WR32(ck + 8, type);
            WR32(ck + 12, (uint32_t)at + 8); WR32(ck + 16, 0);
            RT_RETURN(0, 16);
        }
        if (fseek(f, at + 8 + ((size + 1) & ~1u), SEEK_SET) != 0) RT_RETURN(MMIOERR_CHUNKNOTFOUND, 16);
    }
}
void imp_mmioAscend()
{
    FILE *f = MmioFile(ARG(0));
    uint32_t ck = ARG(1);
    if (!f || !ck) RT_RETURN(MMIOERR_CANNOTSEEK, 12);
    uint32_t data = RD32(ck + 12), size = RD32(ck + 4);
    if (RD32(ck + 16) & 2) {                        // MMIO_DIRTY: created here, write the real size
        size = (uint32_t)ftell(f) - data;
        WR32(ck + 4, size);
        WR32(ck + 16, 0);
        fseek(f, data - 4, SEEK_SET);
        fwrite(&size, 4, 1, f);
        fseek(f, data + size, SEEK_SET);
        if (size & 1) fputc(0, f);
        RT_RETURN(0, 12);
    }
    if (fseek(f, data + ((size + 1) & ~1u), SEEK_SET) != 0) RT_RETURN(MMIOERR_CANNOTSEEK, 12);
    RT_RETURN(0, 12);
}
void imp_mmioCreateChunk()
{
    FILE *f = MmioFile(ARG(0));
    uint32_t ck = ARG(1), flags = ARG(2);
    if (!f || !ck) RT_RETURN(MMIOERR_CANNOTWRITE, 12);
    uint32_t id = (flags & 0x20) ? FOURCC_RIFF : (flags & 0x40) ? FOURCC_LIST : RD32(ck);
    uint32_t hdr[3] = {id, RD32(ck + 4), RD32(ck + 8)};
    long at = ftell(f);
    bool form = (flags & 0x60) != 0;
    if (fwrite(hdr, 4, form ? 3 : 2, f) != (size_t)(form ? 3 : 2)) RT_RETURN(MMIOERR_CANNOTWRITE, 12);
    WR32(ck, id);
    WR32(ck + 12, (uint32_t)at + 8);
    WR32(ck + 16, 2);
    RT_RETURN(0, 12);
}
void imp_FindResourceA() { RT_RETURN(0, 12); }     // WAV data from resources: none in this game

} // namespace

extern "C" RecompFn rt_import_sound(const char *name)
{
    static const struct { const char *name; RecompFn fn; } imps[] = {
        {"DSOUND.DLL!#11", imp_DirectSoundCreate8},
        {"WINMM.DLL!mmioOpenA", imp_mmioOpenA}, {"WINMM.DLL!mmioClose", imp_mmioClose},
        {"WINMM.DLL!mmioRead", imp_mmioRead}, {"WINMM.DLL!mmioWrite", imp_mmioWrite},
        {"WINMM.DLL!mmioSeek", imp_mmioSeek}, {"WINMM.DLL!mmioDescend", imp_mmioDescend},
        {"WINMM.DLL!mmioAscend", imp_mmioAscend}, {"WINMM.DLL!mmioCreateChunk", imp_mmioCreateChunk},
        {"KERNEL32.DLL!FindResourceA", imp_FindResourceA},
    };
    for (auto &i : imps) if (!SDL_strcasecmp(i.name, name)) return i.fn;
    return nullptr;
}
