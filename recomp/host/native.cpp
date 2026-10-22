// native.cpp - native replacements for the PC-layer functions the translated code calls, plus the
// native WinMain, message loop and host entry point.
//
// Called from translated code like the originals: arguments on the emulated stack (ARG(i)),
// `this` in ECX for member functions, and each function pops what the original popped.

#include "runtime.h"
#include "launcher.h"
#include "overlay.h"
#include "../../port/src/unb/Dma.h"
#include "../../port/src/unb/Graphic.h"
#include "../../port/src/unb/Gs.h"
#include "../../port/src/unb/Texture.h"
#include "../../port/src/unb/Vertex.h"
#include <SDL3/SDL.h>
#include <unordered_map>
#ifdef __APPLE__
#include <mach/mach.h>
#include <libproc.h>
#endif
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <string>
#include <vector>
#ifdef RECOMP_GL
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <thread>
#endif

// getenv once per call site (debug hooks run in hot paths)
#define CACHED_ENV(name) ([]() -> const char * { static const char *v_ = getenv(name); return v_; }())

extern "C" {
void rt_event_set(uint32_t h);
int rt_init(const char *exe, const char *gameDir);
extern "C" int rt_jit_cached(const char *exe);       // host/jit.cpp
extern "C" void rt_jit_start(const char *exe);
extern "C" int rt_jit_poll(const char *exe, char *text, size_t size);
extern "C" int rt_jit_ensure(const char *exe);
extern uint8_t g_asyncKeys[256];
}

static SDL_Window *g_window;
static CTexture g_texture;
static CVertex g_vertex;
static bool g_snapshotKey;
#ifdef RECOMP_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
#endif
static bool g_shotPending;      // RECOMP_SHOT_AFTER: held until a frame consumed it
// Test hooks (RECOMP_KEYS, RECOMP_SHOT_AFTER, RECOMP_MEMDUMP) count time in vsync ticks
// (1000/60 ms each) rather than wall time, so scripted runs are reproducible.
static uint64_t g_tickCount;
static uint64_t TickMs() { return g_tickCount * 1000 / 60; }

#define NATIVE extern "C" void

// ---------------------------------------------------------------------------------------------
// Asynchronous frame chains (GL renderer with its render thread). The frame driver task sends the
// frame built during the previous tick (sceDmaSend from the flip, 0x419270) and the other tasks
// then build the next one into the other half of the game's double-buffered packet memory (PS2
// heritage: DMA ran beside the CPU there). Walking the chain (VIF, VU1 packets, GS, recording
// the draws) took about as long as the game logic on the R36H, one after the other on the game
// thread; the DMA thread now walks it while the game computes the next tick. The game reuses the
// chain's memory right after the send (black frames when it was walked in place), so the send
// copies the chain first (CDma::Flatten). Data that ref tags point to inside registered model
// packets (most of the chain's bytes) is read in place, as those change only with model
// (un)registration; RECOMP_DMA_COPYREFS=1 copies them too. Everything from game
// code that reaches the packet path's state (CGs, CVertex, CTexture, CGraphic) waits for the
// chain first (DmaSync), so it happens in the original order: the next send, the GS draw
// environment swap, texture and model (un)loading, viewport and gamma. RECOMP_ASYNC_DMA=0 turns
// it off.
#ifdef RECOMP_GL
static bool g_asyncDma;
static std::mutex g_dmaLock;
static std::condition_variable g_dmaCv;
static bool g_dmaPending;                           // a chain is queued or being walked
static struct { uint32_t chcr; bool shot; } g_dmaJob;
static std::vector<CDma::Segment> g_dmaSegs;       // the chain at the send (CDma::Flatten)
static std::vector<uint8_t> g_dmaCopy;
static uint64_t g_dmaWaitNs, g_dmaCopyNs;          // game thread: waiting for the DMA thread, copying chains (RECOMP_TRACE_TICKS)
static std::atomic<uint64_t> g_dmaChainNs;         // DMA thread: time of the last chain (DrawLoadTick)

static void DmaMain()
{
    for (;;) {
        std::unique_lock<std::mutex> lk(g_dmaLock);
        g_dmaCv.wait(lk, [] { return g_dmaPending; });
        auto job = g_dmaJob;
        lk.unlock();
        uint64_t t0 = SDL_GetTicksNS();
        CDma::SendSegments(g_dmaSegs, g_dmaCopy, job.chcr, job.shot);
        g_dmaChainNs = SDL_GetTicksNS() - t0;
        lk.lock();
        g_dmaPending = false;
        g_dmaCv.notify_all();
    }
}

static void DmaSync()
{
    if (!g_asyncDma) return;
    uint64_t t0 = SDL_GetTicksNS();
    std::unique_lock<std::mutex> lk(g_dmaLock);
    g_dmaCv.wait(lk, [] { return !g_dmaPending; });
    g_dmaWaitNs += SDL_GetTicksNS() - t0;
}

static double DmaWaitMs()                           // per tick of the last 600, then reset
{
    double ms = g_dmaWaitNs / 600 / 1e6;
    if (g_asyncDma) fprintf(stderr, "dma thread: game thread waited %.2f ms/tick, copied chains %.2f ms/tick\n", ms, g_dmaCopyNs / 600 / 1e6);
    g_dmaWaitNs = g_dmaCopyNs = 0;
    return ms;
}

static void DmaStart()
{
    const char *e = getenv("RECOMP_ASYNC_DMA");
    g_asyncDma = CGraphic::Threaded() && !(e && atoi(e) == 0);
    if (g_asyncDma) std::thread([] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "dma");
#elif defined(__APPLE__)
        pthread_setname_np("dma");
#endif
        DmaMain();
    }).detach();
}
#else
static void DmaSync() {}
static void DmaStart() {}
static double DmaWaitMs() { return 0; }
#endif

// ---------------------------------------------------------------------------------------------
// Graphics (CGraphic, Graphic.cpp)

NATIVE N_00402a50(void) { RT_RETURN(0, 0); }                                     // CGraphic::Term (at exit)
NATIVE N_00403560(void) { DmaSync(); RT_RETURN(CGraphic::SetGamma(RDF32(R.esp + 4), ARG(1) != 0), 0); }
NATIVE N_00404390(void)
{
    DmaSync();
    if (CACHED_ENV("RECOMP_TRACE_VIEWPORT")) fprintf(stderr, "SetViewport(%d, %d) from %08x\n", (int)ARG(0), (int)ARG(1), RD32(R.esp));
    RT_RETURN(CGraphic::SetViewport((int)ARG(0), ARG(1) != 0), 0);
}

// ---------------------------------------------------------------------------------------------
// Input: keyboard state is written by the message loop; no joystick yet.

NATIVE N_00405830(void) { RT_RETURN(0, 0); }                                     // input shutdown
NATIVE N_00405ef0(void) { RT_RETURN(0, 0); }                                     // poll joystick n: none
// Joystick/mascon type in use: 0 = none, so the game reads the keyboard-emulated pad
// (0x40f5e0: arrows, Return = START/circle, Space, Esc, ...).
NATIVE N_00406000(void) { RT_RETURN(0, 0); }
// Joystick pad state (0x406040): no joystick, so no data.
NATIVE N_00406040(void) { RT_RETURN(0, 0); }

// ---------------------------------------------------------------------------------------------
// Movie: not supported yet. The game asks the window to open a movie (message 0x400 with the
// file name and a flag to set once done) and spins until the flag is set; it then plays only if
// the movie object (0x670a7c) exists. Reporting "done, no movie" skips the movies.

NATIVE N_004066f0(void)                                                          // OpenMovie(name, pDone)
{
    WR32(0x670a7c, 0);
    if (ARG(1)) WR32(ARG(1), 1);
    RT_RETURN(0, 0);
}
NATIVE N_00406710(void) { RT_RETURN(0, 0); }                                     // CloseMovie
NATIVE N_004068d0(void) { RT_RETURN(0, 0); }                                     // CMovie::IsBusy

// ---------------------------------------------------------------------------------------------
// Textures (CTexture, Texture.cpp): member functions, `this` in ECX is the original object

NATIVE N_0040a2e0(void) { RT_RETURN(R.ecx, 0); }                                 // destructor
NATIVE N_0040a800(void)                                                          // Load(record, allocOnly, variant)
{
    // called every tick for the scenery's textures, nearly always loaded already
    if (!CTexture::IsLoaded(reinterpret_cast<const TXMHEADER *>(MEM + ARG(0)), ARG(1) != 0)) DmaSync();
    bool ok = g_texture.Load(reinterpret_cast<TXMHEADER *>(MEM + ARG(0)), ARG(1) != 0, (int)ARG(2));
    if (CACHED_ENV("RECOMP_TRACE_TEX")) {
        uint32_t h = ARG(0);
        fprintf(stderr, "texload %x: %08x %08x %08x %08x alloc %u var %u -> %d\n", h, RD32(h), RD32(h + 4), RD32(h + 8), RD32(h + 12), ARG(1), ARG(2), ok);
    }
    RT_RETURN(ok, 12);
}
NATIVE N_0040a8a0(void)                                                          // Unload(record)
{
    DmaSync();
    if (ARG(0)) g_texture.Unload(reinterpret_cast<TXMHEADER *>(MEM + ARG(0)));
    RT_RETURN(0, 4);
}
NATIVE N_0040b140(void)                                                          // Update(record, pixels, x, y)
{
    DmaSync();
    bool ok = g_texture.Update(reinterpret_cast<TXMHEADER *>(MEM + ARG(0)), MEM + ARG(1), (int)ARG(2), (int)ARG(3));
    if (CACHED_ENV("RECOMP_TRACE_TEX")) {
        static int n; uint32_t h = ARG(0);
        if (n++ < 30) fprintf(stderr, "texupdate %x: %08x %08x %08x %08x px %x x %d y %d -> %d\n", h, RD32(h), RD32(h + 4), RD32(h + 8), RD32(h + 12), ARG(1), ARG(2), ARG(3), ok);
    }
    RT_RETURN(ok, 16);
}

// ---------------------------------------------------------------------------------------------
// Models (CVertex, Vertex.cpp). In the running game the model object holds absolute (relocated)
// 32-bit pointers to its models, and the texture list is an array of 8-byte entries whose first
// word points at a TXM record.

NATIVE N_00411150(void) { RT_RETURN(R.ecx, 0); }                                 // constructor
NATIVE N_004111e0(void) { RT_RETURN(0, 0); }                                     // destructor

static uint8_t *ModelPtr(uint32_t obj, int i)
{
    uint32_t p = RD32(obj + 0x10 + 8 * i), size = RD32(obj + 0x14 + 8 * i);
    if (!size || !p) return nullptr;
    return MEM + (p >= obj ? p : obj + p);       // relocated pointer, or still a file offset
}

NATIVE N_00411540(void)                                                          // RegisterObject(object, texList, ?)
{
    uint32_t obj = ARG(0), list = ARG(1);
    uint32_t flags = RD32(obj);
    if (flags & 0x80000000u) RT_RETURN(1, 12);     // registered already (every tick for the scenery)
    DmaSync();
    // texture list: as many entries as the models' texture tables reference
    int need = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t *m = ModelPtr(obj, i);
        if (!m) continue;
        for (int k = 0; k < m[1]; k++) {
            uint16_t idx;
            memcpy(&idx, m + 2 + 6 * k, 2);
            need = SDL_max(need, idx + 1);
        }
    }
    std::vector<TXMHEADER *> tex(need);
    for (int i = 0; i < need && list; i++) tex[i] = reinterpret_cast<TXMHEADER *>(MEM + RD32(list + 8 * i));
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++)
        if (uint8_t *m = ModelPtr(obj, i)) ok = g_vertex.RegisterModel(m, list ? tex.data() : nullptr, need, &g_texture);
    if (ok) WR32(obj, flags | 0x80000000u);
    RT_RETURN(ok, 12);
}

// RegisterModel(model, texList, ?) (0x411600): one model with absolute pointers; the texture
// list holds 8-byte entries whose first word points at a TXM record.
NATIVE N_00411600(void)
{
    DmaSync();
    uint32_t model = ARG(0), list = ARG(1);
    uint8_t *m = MEM + model;
    int need = 0;
    for (int k = 0; k < m[1]; k++) {
        uint16_t idx;
        memcpy(&idx, m + 2 + 6 * k, 2);
        need = SDL_max(need, idx + 1);
    }
    std::vector<TXMHEADER *> tex(need);
    for (int i = 0; i < need && list; i++) tex[i] = reinterpret_cast<TXMHEADER *>(MEM + RD32(list + 8 * i));
    bool ok = g_vertex.RegisterModel(m, list ? tex.data() : nullptr, need, &g_texture);
    RT_RETURN(ok, 12);
}
NATIVE N_004117f0(void)                                                          // UnregisterModel(model, ?)
{
    DmaSync();
    if (ARG(0)) g_vertex.UnregisterModel(MEM + ARG(0));
    RT_RETURN(1, 8);
}

NATIVE N_004115b0(void)                                                          // UnregisterObject(object)
{
    DmaSync();
    uint32_t obj = ARG(0);
    if (obj && (RD32(obj) & 0x80000000u)) {
        WR32(obj, RD32(obj) & 0x7fffffffu);
        for (int i = 0; i < 3; i++)
            if (uint8_t *m = ModelPtr(obj, i)) g_vertex.UnregisterModel(m);
    }
    RT_RETURN(0, 4);
}

// ---------------------------------------------------------------------------------------------
// DMA: sceDmaSend(channel, tadr) (0x4145b0). The channel structs live in the game's memory
// (0x676300, 0x90 bytes each): CHCR at +0, TADR at +0x30. Afterwards the original called the
// channel's completion handler from its table (0x40d180).

static uint32_t g_frames;
static uint32_t g_framesThisTick;       // DMA frame sends during the current vsync tick
static uint64_t g_tickBusyNs, g_tickBusyMaxNs;   // game work inside ticks (RECOMP_TRACE_TICKS)
static uint64_t g_renderNs;                        // part of it in sceDmaSend (DMA/GS/VU1/GPU submit)
static uint64_t g_renderInstr;                     // instructions retired in sceDmaSend
extern "C" uint64_t g_readNs, g_readBytes;          // win32.c ReadFile (RECOMP_TRACE_SPIKES)
#ifdef RECOMP_GL
extern uint64_t g_gfxResNs, g_gfxResCatNs[6], g_gfxWaitNs;     // GraphicGL.cpp resource calls
#endif

// Instructions retired by the whole process per tick since the previous call (600 ticks); a
// stable measure of work, unlike time on a fast out-of-order core. 0 where unavailable.
static double ProcessMInstrPerTick()
{
#ifdef __APPLE__
    static uint64_t last;
    rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri) != 0) return 0;
    double d = last ? (double)(ri.ri_instructions - last) / 600 / 1e6 : 0;
    last = ri.ri_instructions;
    return d;
#else
    return 0;
#endif
}

// Instructions retired under the runtime lock per tick (game code + natives incl. rendering)
static double GameMInstrPerTick()
{
    static uint64_t last;
    uint64_t now = rt_locked_instructions();
    double d = last ? (double)(now - last) / 600 / 1e6 : 0;
    last = now;
    return d;
}

// CPU time of all threads (game, driver, audio...) per tick since the previous call, 600 ticks
static double ProcessCpuMsPerTick()
{
    static double last;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double now = ru.ru_utime.tv_sec * 1e3 + ru.ru_utime.tv_usec / 1e3 + ru.ru_stime.tv_sec * 1e3 + ru.ru_stime.tv_usec / 1e3;
    double d = last ? (now - last) / 600 : 0;
    last = now;
    return d;
}
static uint32_t g_presents, g_presentsLate;       // presents, and those > 20 ms after the previous one
static uint64_t g_presentGapMaxNs;
static uint64_t g_phaseNs[3], g_phaseMaxNs[3];    // message loop: present, waiting for the GIL, pacing delay
static void Phase(int i, uint64_t t0)
{
    uint64_t d = SDL_GetTicksNS() - t0;
    g_phaseNs[i] += d;
    if (d > g_phaseMaxNs[i]) g_phaseMaxNs[i] = d;
}

NATIVE N_004145b0(void)
{
    uint64_t w0 = SDL_GetTicksNS();
    DmaSync();                                      // the previous chain first (and its counters)
    g_renderNs += SDL_GetTicksNS() - w0;
    uint32_t ch = ARG(0), tadr = ARG(1);
    if ((ch - 0x676300u) / 0x90 == 1) g_framesThisTick++;
    // RECOMP_TRACE_STATS: DMA/GS counters every 120 frames
    static const bool stats = getenv("RECOMP_TRACE_STATS") != nullptr;
    if ((ch - 0x676300u) / 0x90 == 1 && (++g_frames % 120) == 0 && stats) {
        fprintf(stderr, "frames %u: per 120 frames dma tags %u, direct qw %u, gs regs %u, kicks %u, images %u, unpacks %u, mscnt %u, max tex alpha %02x\n",
                g_frames, CDma::stats.tags, CDma::stats.directQw, CGs::stats.regs, CGs::stats.kicks, CGs::stats.images,
                CDma::stats.unpacks, CDma::stats.mscnt, CGs::stats.maxAlpha);
        CDma::stats = DmaStats();
        CGs::stats = GsStats();
    }
    if (tadr) {
        WR32(ch + 0x30, tadr);
        WR32(ch, RD32(ch) | 0x100);
    }
    int idx = (int)((ch - 0x676300u) / 0x90);
    bool shot = g_snapshotKey || (idx == 1 && g_shotPending);
    if (idx == 1 && g_shotPending && CACHED_ENV("RECOMP_SHOT_TRACE")) CGs::traceKicks = 2000;
    if (idx == 1) g_shotPending = false;
    uint64_t r0 = SDL_GetTicksNS(), i0 = rt_thread_instructions();
#ifdef RECOMP_GL
    if (idx == 1 && g_asyncDma) {
        std::lock_guard<std::mutex> lk(g_dmaLock);
        uint64_t c0 = SDL_GetTicksNS();
        static const bool copyRefs = getenv("RECOMP_DMA_COPYREFS") && atoi(getenv("RECOMP_DMA_COPYREFS"));
        CDma::Flatten(MEM, RD32(ch + 0x30), g_dmaSegs, g_dmaCopy, [](const uint8_t *p, size_t n) {
            return !copyRefs && g_vertex.InModelPackets(p, n);
        });
        g_dmaCopyNs += SDL_GetTicksNS() - c0;
        g_dmaJob = {RD32(ch), shot};
        g_dmaPending = true;
        g_dmaCv.notify_all();
    } else
#endif
    CDma::Send(idx, MEM, RD32(ch + 0x30), RD32(ch), shot);
    g_renderNs += SDL_GetTicksNS() - r0;
    g_renderInstr += rt_thread_instructions() - i0;
    WR32(ch, RD32(ch) & ~0x100u);
    if (idx >= 0 && idx < 10 && RD32(0x673000 + 4 * idx) && RD32(0x672fd8 + 4 * idx)) {
        uint32_t a = (uint32_t)idx;
        rt_call(RD32(0x672fd8 + 4 * idx), 1, &a);
    }
    RT_RETURN(0, 0);
}

// sceGsSwapDBuffDc(dc, field) (0x414c60). The original copied the selected half of the double
// buffer's draw environment (A+D pairs) into its GS register globals; here they go to CGs. The
// globals are still written for game code that reads them back.

NATIVE N_00414c60(void)
{
    DmaSync();
    uint32_t dc = ARG(0), field = ARG(1);
    uint32_t disp = dc + field * 0x28;
    static const uint32_t kDisp[5] = {0x673250, 0x673258, 0x673270, 0x673278, 0x673298};
    for (int i = 0; i < 5; i++) {
        WR32(kDisp[i], RD32(disp + i * 8));
        WR32(kDisp[i] + 4, RD32(disp + i * 8 + 4));
    }
    // Offsets of the A+D pairs in the first half and the globals they were copied to. PRMODECONT,
    // COLCLAMP and DTHE (0xa0..0xc0) were skipped by the original.
    // The PC port leaves the A+D address fields empty, so the register comes from the
    // sceGsDrawEnv1/2 layout.
    static const struct { uint32_t off, global, reg; } kEnv[] = {
        {0x60, 0x6731d0, 0x4c}, {0x70, 0x6731e0, 0x4e}, {0x80, 0x6730c8, 0x18}, {0x90, 0x673170, 0x40},
        {0xd0, 0x6731a8, 0x47}, {0xe0, 0x6731d8, 0x4d}, {0xf0, 0x6731e8, 0x4f}, {0x100, 0x6730d0, 0x19},
        {0x110, 0x673178, 0x41}, {0x120, 0x6730d8, 0x1a}, {0x130, 0x6731a0, 0x46}, {0x140, 0x673198, 0x45},
        {0x150, 0x6731b0, 0x48},
    };
    for (const auto &e : kEnv) {
        uint32_t a = dc + e.off + (field ? 0x170 : 0);
        uint32_t lo = RD32(a), hi = RD32(a + 4);
        WR32(e.global, lo);
        WR32(e.global + 4, hi);
        CGs::WriteReg(e.reg, (uint64_t)hi << 32 | lo);
    }
    RT_RETURN(0, 0);
}

// Frame limiter (0x4075a0), called by the flip function: returns whether to draw this frame. The
// original slept when ahead of a 1/60 s schedule and skipped drawing while behind it (for up to
// 1/6 s). The message loop already paces ticks to exactly 60 Hz, and the schedule anchored to the
// first frame turned small timing jitter into 1/6 s skips (stutter), so by default every frame is
// drawn. Each tick of the message loop runs one whole game frame (logic and drawing), so on a slow
// machine ticks fall behind the 60 Hz schedule and the game runs in slow motion. With
// `frameskip = N` (graphics.ini) the frame of a tick that started more than one period late is
// not drawn (at most N in a row): such ticks only run game logic and catch up.
// `maxfps = 30` (or 20, 15: 60 divided by a whole number) draws only every second (third, fourth)
// tick, so a slow device spends the time on fewer, steadier frames (or a longer draw distance);
// a frame that is due but skipped for being late is drawn on the next tick instead.
static int g_frameskipMax;
static uint32_t g_drawInterval = 1;
static uint32_t g_framesSkipped;
static uint64_t g_tickLateNs, g_tickPeriodNs;  // how late the current tick started (message loop)

// Automatic draw distance (drawdistance = auto): the game's controller 0x47e1d0 moves RendPow up
// while its load is below the budget and down above it (plan.MEM_OVERRIDES). The load is the
// busiest stage's share of a drawn frame's period, in the controller's units (kLoadFrame = one
// period): the game thread's work over the ticks of a frame (it includes waiting for the DMA and
// render threads when they fall behind), the DMA thread's last chain, and the renderer's slowest
// stage (CGraphic::FrameLoadNs: list thread, render thread + waiting for the GPU).
static constexpr int kLoadFrame = 525;
static bool g_ddAuto;
static uint32_t g_ddLoadSum, g_ddLoadMax, g_ddLoadTicks;     // RECOMP_TRACE_TICKS
static double g_ddPowSum;

static void DrawLoadTick(uint64_t busyNs, uint64_t periodNs)
{
    if (!g_ddAuto) return;
    if (!periodNs) periodNs = 16666667;
    static uint64_t ring[6];
    static uint32_t n;
    uint32_t k = SDL_min(g_drawInterval, 6u);
    ring[n++ % 6] = busyNs;
    uint64_t game = 0;
    for (uint32_t i = 0; i < k; i++) game += ring[(n - 1 - i) % 6];
    uint64_t frameNs = periodNs * k, stage = 0;
#ifdef RECOMP_GL
    stage = SDL_max((uint64_t)g_dmaChainNs, CGraphic::FrameLoadNs());
#endif
    uint64_t worst = SDL_max(game, stage);
    uint32_t load = (uint32_t)SDL_min(worst * kLoadFrame / frameNs, (uint64_t)(8 * kLoadFrame));
    // smoothed over ~16 ticks: single frames vary by a factor of two or more
    static uint32_t smooth16;           // 16 x the smoothed load
    smooth16 = smooth16 ? smooth16 - smooth16 / 16 + load : load * 16;
    WR32(0x7ffc0018, smooth16 / 16);    // plan.SETTING_DD_LOAD
    g_ddLoadSum += load;
    g_ddLoadMax = SDL_max(g_ddLoadMax, load);
    g_ddLoadTicks++;
    float pow;
    uint32_t u = RD32(0x5db0f8);
    memcpy(&pow, &u, 4);
    g_ddPowSum += pow;
    // RECOMP_TRACE_DD: every tick, the load and its parts (game, DMA thread, renderer) with the
    // draw distance and the train's speed (0x74f604), which sets the controller's fine steps
    if (CACHED_ENV("RECOMP_TRACE_DD")) {
        float speed;
        u = RD32(0x74f604);
        memcpy(&speed, &u, 4);
#ifdef RECOMP_GL
        uint64_t dma = g_dmaChainNs, gfx = CGraphic::FrameLoadNs();
#else
        uint64_t dma = 0, gfx = 0;
#endif
        fprintf(stderr, "dd t=%.2f load %u smoothed %u (game %.1f dma %.1f gfx %.1f ms) pow %.3f speed %.2f\n", TickMs() / 1000.0, load, smooth16 / 16,
                game / 1e6, dma / 1e6, gfx / 1e6, pow, speed);
    }
}
extern "C" void N_0041af10(void);            // host/tasks.cpp

// RECOMP_SKIP=<addr>@<s>[,...]: functions translated with LIFT_SKIPPABLE return at once from tick
// time s on (reverse engineering: what does this function do?).
extern "C" int rt_skip(uint32_t addr)
{
    static const char *spec = getenv("RECOMP_SKIP");
    if (!spec) return 0;
    double now = TickMs() / 1000.0;
    for (const char *p = spec; *p;) {
        uint32_t a = (uint32_t)strtoul(p, (char **)&p, 16);
        double from = *p == '@' ? strtod(p + 1, (char **)&p) : 0;
        if (a == addr && now >= from) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

// RECOMP_TRACE_TASKS: the game's cooperative tasks (scheduler 0x41ab70; list head 0x758208, next
// at +0x0c; created by 0x41ad00(id, entry, arg, stack, priority | wait << 8)) with their CPU time
// over the last 600 ticks (each task runs on its own guest thread, handle at +0x28).
static void TraceTasks()
{
    static std::unordered_map<uint32_t, uint64_t> lastCpu;
    fprintf(stderr, "tasks: id prio entry    arg      wait susp flags  cpu ms/tick\n");
    int n = 0;
    for (uint32_t t = RD32(0x758208); t && n < 256; t = RD32(t + 0x0c), n++) {
        uint32_t h = RD32(t + 0x28);
        uint64_t cpu = rt_thread_cpu_ns(h), &last = lastCpu[t ^ h];
        double ms = last && cpu >= last ? (cpu - last) / 600.0 / 1e6 : 0;
        last = cpu;
        fprintf(stderr, "tasks: %02x  %02x  %08x %08x %4u %4u %02x  %6.3f%s\n", RD32(t + 4) & 0xff, MEM[t + 1], RD32(t + 0x24),
                RD32(t + 0x1c), RD16(t + 8), RD16(t + 0x0a), MEM[t], ms, RD32(0x758204) == t ? "  (current)" : "");
    }
}

NATIVE N_004075a0(void)
{
    static int skipped;
    static uint32_t sinceDraw;
    if (++sinceDraw < g_drawInterval) RT_RETURN(0, 0);     // between maxfps frames: logic only
    bool draw = !(g_frameskipMax > 0 && g_tickPeriodNs && g_tickLateNs > g_tickPeriodNs && skipped < g_frameskipMax);
    // test hook: RECOMP_FORCE_SKIP=<n> skips n frames after every drawn one (a slow device's pattern)
    static const int forced = getenv("RECOMP_FORCE_SKIP") ? atoi(getenv("RECOMP_FORCE_SKIP")) : 0;
    if (forced > 0) draw = skipped >= forced;
#ifdef RECOMP_GL
    // GL render thread still drawing the previous frame: skip this one unless it should be done by
    // the time this frame is built (the game thread then waits a few ms at EndFrame, which keeps
    // the render thread busy: frames every ~37 ms instead of every third tick, 50 ms). Long waits
    // (a scenery change's uploads take a few hundred ms on Mali) are skipped however long they
    // last; nothing in the game reads the skip counter but this limiter.
    else if (draw && g_frameskipMax > 0) {
        static const uint64_t aheadNs = (uint64_t)((getenv("RECOMP_GL_AHEAD_MS") ? atof(getenv("RECOMP_GL_AHEAD_MS")) : 10.0) * 1e6);
        if (CGraphic::FrameBusyNs() > aheadNs) draw = false;
    }
#endif
    if (draw) { skipped = 0; sinceDraw = 0; }
    else { skipped++; g_framesSkipped++; }
    WR32(0x670ad8, (uint32_t)skipped);          // frames skipped so far
    RT_RETURN(draw ? 1 : 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Window, WinMain and the message loop

NATIVE N_00406f70(void) { RT_RETURN(1, 0); }       // CreateMainWindow: the host created the window

// Windows virtual-key code for an SDL scancode (0 = none)
static uint8_t VirtualKey(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return (uint8_t)('A' + (s - SDL_SCANCODE_A));
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return (uint8_t)('1' + (s - SDL_SCANCODE_1));
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return (uint8_t)(0x70 + (s - SDL_SCANCODE_F1));
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return (uint8_t)(0x61 + (s - SDL_SCANCODE_KP_1));
    switch (s) {
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0d;
    case SDL_SCANCODE_ESCAPE: return 0x1b;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_INSERT: return 0x2d;
    case SDL_SCANCODE_DELETE: return 0x2e;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_COMMA: return 0xbc;
    case SDL_SCANCODE_PERIOD: return 0xbe;
    case SDL_SCANCODE_MINUS: return 0xbd;
    case SDL_SCANCODE_EQUALS: return 0xbb;
    case SDL_SCANCODE_SEMICOLON: return 0xba;
    case SDL_SCANCODE_SLASH: return 0xbf;
    default: return 0;
    }
}

// Gamepads act as keyboards (the game's keyboard layout, see docs/DEVELOPMENT.md): d-pad/left stick =
// arrows, A/Start = Return, B/Back = Escape, X = Space, shoulders = PageUp/PageDown,
// triggers = Home/End. Start+Back quits (handhelds have no keyboard), R3 takes a screenshot, Y
// (unused by the game's layout) opens the options menu (*menu, host/overlay.cpp).
static void GamepadKeys(uint8_t *keys, bool *menu)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; i++) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        auto btn = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
        auto axis = [pad](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a); };
        struct { bool on; uint8_t vk; } map[] = {
            {btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis(SDL_GAMEPAD_AXIS_LEFTX) < -16000, 0x25},
            {btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || axis(SDL_GAMEPAD_AXIS_LEFTY) < -16000, 0x26},
            {btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis(SDL_GAMEPAD_AXIS_LEFTX) > 16000, 0x27},
            {btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis(SDL_GAMEPAD_AXIS_LEFTY) > 16000, 0x28},
            {btn(SDL_GAMEPAD_BUTTON_SOUTH) || btn(SDL_GAMEPAD_BUTTON_START), 0x0d},
            {btn(SDL_GAMEPAD_BUTTON_EAST) || btn(SDL_GAMEPAD_BUTTON_BACK), 0x1b},
            {btn(SDL_GAMEPAD_BUTTON_WEST), 0x20},
            {btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), 0x21},
            {btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), 0x22},
            {axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000, 0x24},
            {axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000, 0x23},
        };
        for (auto &m : map) if (m.on) keys[m.vk] = 0x80;
        if (btn(SDL_GAMEPAD_BUTTON_NORTH)) *menu = true;
        // right stick click: one screenshot (snapshot/*.bmp next to the program), like F12
        static bool r3Prev;
        bool r3 = btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        if (r3 && !r3Prev) {
            g_shotPending = true;
            CGraphic::m_Config.snapshot = 1;
        }
        r3Prev = r3;
        if (btn(SDL_GAMEPAD_BUTTON_START) && btn(SDL_GAMEPAD_BUTTON_BACK)) {
            fprintf(stderr, "quit (Start+Back)\n");
            rt_profile_dump();
            _exit(0);
        }
    }
    SDL_free(ids);
}

// Waits until the next tick. SDL_DelayPrecise sleeps in 1 ms steps and spins through the last
// one: on the handhelds that was most of the main thread's CPU time (heat, and the RK3326 then
// clocks down). One sleep is accurate to ~0.1 ms on Linux, which is plenty (the display paces
// the frames); elsewhere SDL's precise delay.
static void DelayTick(uint64_t ns)
{
#ifdef __linux__
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
#else
    SDL_DelayPrecise(ns);
#endif
}

// 0x4074a0: runs on the main thread until the program quits. The original pumped Windows
// messages and, when idle, copied the keyboard state to 0x6708e0, ran the vsync/pad hook
// 0x41af10 and woke the file I/O thread.
NATIVE N_004074a0(void)
{
    uint8_t keys[256];
    for (;;) {
        bool menuButton = false;                        // gamepad Y (GamepadKeys) or F10
        rt_leave();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                fprintf(stderr, "quit\n");
                rt_profile_dump();
                _exit(0);
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad *pad = SDL_OpenGamepad(e.gdevice.which))
                    fprintf(stderr, "gamepad: %s\n", SDL_GetGamepadName(pad));
            }
            // F11 or Alt+Return: toggle borderless fullscreen
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.scancode == SDL_SCANCODE_F11 ||
                 (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(g_window, !fs);
            }
        }
        memset(keys, 0, sizeof(keys));
        int n = 0;
        const bool *state = SDL_GetKeyboardState(&n);
        for (int s = 0; s < n; s++)
            if (state[s]) if (uint8_t vk = VirtualKey((SDL_Scancode)s)) keys[vk] = 0x80;
        GamepadKeys(keys, &menuButton);
        g_snapshotKey = state[SDL_SCANCODE_F12];
        // test hook: RECOMP_EXIT_AFTER=<seconds> (tick time) quits; PGO builds write their
        // profile first (an alarm signal would kill the process before it does)
        if (const char *ex = CACHED_ENV("RECOMP_EXIT_AFTER")) {
            if (TickMs() / 1000.0 >= atof(ex)) {
#ifdef RECOMP_PGO_GEN
                // instrumented build (linux/pgo.sh): the profile runtime's writer (a hidden symbol)
                fprintf(stderr, "profile written: %d\n", __llvm_profile_write_file());
#endif
                fprintf(stderr, "exit (RECOMP_EXIT_AFTER)\n");
                rt_profile_dump();
                _exit(0);
            }
        }
        // test hook: RECOMP_SHOT_AFTER=<seconds> takes one screenshot (snapshot/*.bmp)
        // test hook: RECOMP_SHOT_AFTER=<seconds>[,<seconds>...] takes screenshots (snapshot/*.bmp)
        static const char *shots = getenv("RECOMP_SHOT_AFTER");
        if (shots && *shots && TickMs() / 1000.0 >= atof(shots)) {
            g_shotPending = true;
            CGraphic::m_Config.snapshot = 1;
            shots = strchr(shots, ',');
            if (shots) shots++;
        }
        bool presented;
        {
            uint64_t p0 = SDL_GetTicksNS();
            presented = CGraphic::PresentPending();
            Phase(0, p0);
            if (presented) {
                static uint64_t lastPresent;
                uint64_t t = SDL_GetTicksNS();
                if (lastPresent) {
                    uint64_t gap = t - lastPresent;
                    g_presents++;
                    if (gap > 20000000) g_presentsLate++;
                    if (gap > g_presentGapMaxNs) g_presentGapMaxNs = gap;
                }
                lastPresent = t;
            }
            static uint64_t blocked; static int nb;
            if (presented && CACHED_ENV("RECOMP_TRACE_PRESENT")) {
                blocked += SDL_GetTicksNS() - p0;
                if (++nb == 300) { fprintf(stderr, "present call avg %.2f ms\n", blocked / 300 / 1e6); blocked = 0; nb = 0; }
            }
            // test hook: RECOMP_TRACE_PRESENT: histogram of intervals between presents (ms)
            static const bool trace = getenv("RECOMP_TRACE_PRESENT") != nullptr;
            if (trace && presented) {
                static uint64_t last, start;
                static int hist[40], n;
                uint64_t t = SDL_GetTicksNS();
                if (last) hist[SDL_min((int)((t - last) / 1000000), 39)]++;
                if (!start) start = t;
                last = t;
                if (++n % 300 == 0) {
                    fprintf(stderr, "present intervals (ms:count) over %.1fs:", (t - start) / 1e9);
                    for (int i = 0; i < 40; i++) if (hist[i]) fprintf(stderr, " %d:%d", i, hist[i]);
                    fprintf(stderr, "\n");
                    memset(hist, 0, sizeof(hist));
                    start = t;
                }
            }
        }
        if (!presented) SDL_Delay(1);       // a present already waited for vsync
        uint64_t e0 = SDL_GetTicksNS();
        rt_enter();
        Phase(1, e0);
        // test hook: RECOMP_KEYS="ms:vk[:dur],..." presses virtual key vk (hex) at tick time ms for dur ms (100)
        static const char *script = getenv("RECOMP_KEYS");
        if (script) {
            uint64_t now = TickMs();
            for (const char *p = script; *p;) {
                unsigned long at = strtoul(p, (char **)&p, 10), vk = 0, dur = 100;
                if (*p == ':') vk = strtoul(p + 1, (char **)&p, 16);
                if (*p == ':') dur = strtoul(p + 1, (char **)&p, 10);
                if (vk < 256 && now >= at && now < at + dur) keys[vk] = 0x80;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        if (CACHED_ENV("RECOMP_TRACE_PAD")) {
            static uint64_t last;
            uint64_t now = SDL_GetTicks();
            if (now - last >= 250) {
                last = now;
                uint32_t pad = RD32(0x758270);
                fprintf(stderr, "pad t=%llu mode %u pad0 %x buttons %08x raw %08x kbd[0d]=%02x game %08x %08x %08x %08x\n", (unsigned long long)now,
                        RD32(0x670a60), pad, pad ? RD32(pad + 0x28) : 0, pad ? RD32(pad + 0x80) : 0, keys[0x0d], RD32(0x72bb60), RD32(0x758274), RD32(RD32(0x758274) + 0x20), RD32(RD32(0x758274) + 0x28));
            }
        }
        // F10 (also from RECOMP_KEYS: vk 79) opens the options menu instead of reaching the game;
        // while the menu is open the game gets no input (host/overlay.cpp)
        menuButton |= keys[0x79] != 0;
        keys[0x79] = 0;
        OverlayUpdate(keys, menuButton);
        memcpy(MEM + 0x6708e0, keys, 256);
        memcpy(g_asyncKeys, keys, 256);
        // One call is one PS2 vsync: it wakes the game's tasks for a tick. The original was paced
        // by the blocking D3D present; here ticks are paced to NTSC field rate.
        static uint64_t nextTick;
        uint64_t now = SDL_GetTicksNS();
        static const double tickHz = getenv("RECOMP_TICK_HZ") ? atof(getenv("RECOMP_TICK_HZ")) : 60.0;
        const uint64_t period = tickHz > 0 ? (uint64_t)(1e9 / tickHz) : 0;
        // Ticks are paced to exactly 60 Hz. The game's frame limiter (0x4075a0) targets 1/60 s per
        // frame and skips rendering whenever it falls behind, so a slower rate (59.94) causes
        // bursts; at 60 Hz it renders every tick without sleeping. The original relied on the
        // blocking D3D present, which does not pace anything on high-refresh displays.
        // RECOMP_TICK_HZ=<hz> overrides (0 = unpaced: only the game's own Sleep-based limiter).
        if (tickHz <= 0) nextTick = 0;
        if (now < nextTick) {
            rt_leave();
            uint64_t d0 = SDL_GetTicksNS();
            DelayTick(nextTick - now);
            Phase(2, d0);
            uint64_t e1 = SDL_GetTicksNS();
            rt_enter();
            Phase(1, e1);
            now = SDL_GetTicksNS();
        }
        // Resync after stalls: normally after 4 periods. With frameskip the schedule has to survive
        // slow drawn frames (several periods each) so that skipped frames can catch up: 0.25 s.
        g_tickLateNs = nextTick && now > nextTick ? now - nextTick : 0;
        g_tickPeriodNs = period;
        uint64_t slack = g_frameskipMax > 0 ? 250000000ull : 4 * period;
        nextTick = (now - nextTick < slack) ? nextTick + period : now + period;
        static uint64_t ticks, since;
        if (++ticks % 600 == 0 && CACHED_ENV("RECOMP_TRACE_TICKS")) {
            CGraphic::DebugStats &gs = CGraphic::GetDebugStats();
            struct { uint64_t resident_size; } mi = {};
#ifdef __APPLE__
            {
                mach_task_basic_info_data_t ti = {};
                mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
                task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&ti, &cnt);
                mi.resident_size = ti.resident_size;
            }
#else
            if (FILE *sf = fopen("/proc/self/statm", "r")) {
                unsigned long pages = 0, rss = 0;
                if (fscanf(sf, "%lu %lu", &pages, &rss) == 2) mi.resident_size = (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE);
                fclose(sf);
            }
#endif
#ifdef RT_COUNT_SAVES
            {
                static uint64_t lastSaves;
                if (RT_SAVES) fprintf(stderr, "register saves per tick: %.0f\n", (double)(RT_SAVES - lastSaves) / 600);
                lastSaves = RT_SAVES;
            }
#endif
            if (g_frameskipMax) { fprintf(stderr, "frameskip: %u of 600 frames skipped\n", g_framesSkipped); g_framesSkipped = 0; }
            if (g_ddAuto && g_ddLoadTicks) {
                fprintf(stderr, "draw distance: %.3f (average), load %.0f%% average, %.0f%% max of the frame period\n",
                        g_ddPowSum / g_ddLoadTicks, 100.0 * g_ddLoadSum / g_ddLoadTicks / kLoadFrame, 100.0 * g_ddLoadMax / kLoadFrame);
                g_ddLoadSum = g_ddLoadMax = g_ddLoadTicks = 0;
                g_ddPowSum = 0;
            }
            fprintf(stderr, "t=%.0fs ticks %.1f/s clock %u | game %.2f render %.2f ms/tick (dma wait %.2f) | process cpu %.2f ms/tick %.2f Minstr/tick | game %.2f render %.2f Minstr/tick | loop ms avg/max: present %.2f/%.1f gil %.2f/%.1f delay %.2f/%.1f | presents %u, late %u, worst gap %.0f ms | mesh draws/frame %.0f, param blocks %.0f, all draws %.0f, pipeline binds %.0f | work/tick avg %.2f max %.2f ms | flushes/tick %.1f, "
                    "upload %.0f KB/s (tex %u = %.0f KB/s, mesh %.0f, vertices %.0f) | textures %u (+%u -%u), meshes %u (+%u -%u), pipelines %u | heap %08x (live %.1f MB) virt %08x | rss %llu MB (guest %llu MB)\n",
                    TickMs() / 1000.0, 600e9 / (double)(now - since), RD32(0x717fc4),
                    (g_tickBusyNs - g_renderNs) / 600 / 1e6, g_renderNs / 600 / 1e6, DmaWaitMs(), ProcessCpuMsPerTick(), ProcessMInstrPerTick(), GameMInstrPerTick() - g_renderInstr / 600 / 1e6, g_renderInstr / 600 / 1e6,
                    g_phaseNs[0] / 600 / 1e6, g_phaseMaxNs[0] / 1e6, g_phaseNs[1] / 600 / 1e6, g_phaseMaxNs[1] / 1e6,
                    g_phaseNs[2] / 600 / 1e6, g_phaseMaxNs[2] / 1e6, g_presents, g_presentsLate,
                    g_presentGapMaxNs / 1e6, gs.meshDraws / 600.0, gs.meshParamBlocks / 600.0, gs.drawCmds / 600.0,
                    gs.pipelineBinds / 600.0, g_tickBusyNs / 600 / 1e6,
                    g_tickBusyMaxNs / 1e6, gs.flushes / 600.0, gs.uploadBytes / 10.0 / 1024, gs.texUploads,
                    gs.texUploadBytes / 10.0 / 1024, gs.meshUploadBytes / 10.0 / 1024, gs.vertexBytes / 10.0 / 1024, gs.liveTextures,
                    gs.texCreates, gs.texDestroys, gs.liveMeshes, gs.meshCreates, gs.meshDestroys, gs.pipelines,
                    rt_heap_top(), rt_heap_live() / 1048576.0, rt_virt_top(), (unsigned long long)(mi.resident_size >> 20),
                    (unsigned long long)(rt_guest_rss_kb() >> 10));
            if (CACHED_ENV("RECOMP_TRACE_TASKS")) TraceTasks();
            g_presents = g_presentsLate = 0; g_presentGapMaxNs = 0;
            memset(g_phaseNs, 0, sizeof(g_phaseNs)); memset(g_phaseMaxNs, 0, sizeof(g_phaseMaxNs));
            gs.meshDraws = gs.meshParamBlocks = gs.drawCmds = gs.pipelineBinds = 0;
            gs.texUploads = gs.texUploadBytes = gs.meshUploadBytes = gs.vertexBytes = 0;
            gs.flushes = gs.uploadBytes = gs.texCreates = gs.texDestroys = gs.meshCreates = gs.meshDestroys = 0;
            g_tickBusyNs = g_tickBusyMaxNs = 0;
            g_renderNs = 0;
            g_renderInstr = 0;
            since = now;
        }
        // test hook: RECOMP_MEMDUMP=<sec>,<sec>: dump 0x4b2000-0x860000 and the heap start to build/mem<n>.bin
        static const char *dumps = getenv("RECOMP_MEMDUMP");
        static int ndump;
        if (dumps && *dumps && TickMs() / 1000.0 >= atof(dumps)) {
            char path[64];
            snprintf(path, sizeof(path), "build/mem%d.bin", ndump++);
            if (FILE *f = fopen(path, "wb")) {
                fwrite(MEM + 0x4b2000, 1, 0x860000 - 0x4b2000, f);
                fwrite(MEM + 0x10000000, 1, 0x2000000, f);
                fclose(f);
            }
            fprintf(stderr, "memdump %s at tick %llu\n", path, (unsigned long long)ticks);
            dumps = strchr(dumps, ',');
            if (dumps) dumps++;
        }
        uint64_t tickStart = SDL_GetTicksNS();
        g_framesThisTick = 0;
        // test hook: RECOMP_GPUTEX="<sec>:<handle>" compares a managed texture's GPU and CPU copies
        if (const char *gt = CACHED_ENV("RECOMP_GPUTEX")) {
            static bool done;
            if (!done && TickMs() / 1000.0 >= atof(gt)) {
                done = true;
                uint32_t h = (uint32_t)strtoul(strchr(gt, ':') + 1, nullptr, 0);
                fprintf(stderr, "gputex %u: %d texels differ (frame serial %u)\n", h, CGraphic::DebugCompareManagedTexture(h, "build/gputex"), CGraphic::GetFrameSerial());
            }
        }
        g_tickCount++;
        // test hook: RECOMP_SUSPEND_TASK=<entry hex>@<s>[,...] keeps the game task with that entry
        // suspended (+0x0a, honoured by the scheduler 0x41ab70) from tick time s on
        if (const char *st = CACHED_ENV("RECOMP_SUSPEND_TASK")) {
            for (const char *p = st; *p;) {
                uint32_t entry = (uint32_t)strtoul(p, (char **)&p, 16);
                double from = *p == '@' ? strtod(p + 1, (char **)&p) : 0;
                if (TickMs() / 1000.0 >= from)
                    for (uint32_t t = RD32(0x758208), n = 0; t && n < 256; t = RD32(t + 0x0c), n++)
                        if (RD32(t + 0x24) == entry) WR16(t + 0x0a, 1);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
#ifdef RECOMP_GL
        uint64_t res0 = g_gfxResNs, cat0[6], wait0 = g_gfxWaitNs;
        memcpy(cat0, g_gfxResCatNs, sizeof(cat0));
#else
        uint64_t res0 = 0;
#endif
        uint64_t read0 = g_readNs, readB0 = g_readBytes, render0 = g_renderNs;
        R.esp -= 4;                                 // vsync tick: the task scheduler (host/tasks.cpp)
        WR32(R.esp, 0);
        N_0041af10();
        {
            uint64_t busy = SDL_GetTicksNS() - tickStart;
            DrawLoadTick(busy, g_tickPeriodNs);
            g_tickBusyNs += busy;
            if (busy > g_tickBusyMaxNs) g_tickBusyMaxNs = busy;
            // RECOMP_TRACE_SPIKES=<ms>: break down every tick that takes longer
            static const double spikeMs = getenv("RECOMP_TRACE_SPIKES") ? atof(getenv("RECOMP_TRACE_SPIKES")) : 0;
            if (spikeMs > 0 && busy > spikeMs * 1e6) {
#ifdef RECOMP_GL
                uint64_t res = g_gfxResNs - res0;
#else
                uint64_t res = 0;
#endif
                uint64_t rd = g_readNs - read0, rnd = g_renderNs - render0;
                fprintf(stderr, "spike t=%.1fs: %.1f ms = textures/models %.1f, file reads %.1f (%llu KB), draw submit %.1f, other game code %.1f\n",
                        TickMs() / 1000.0, busy / 1e6, res / 1e6, rd / 1e6, (unsigned long long)((g_readBytes - readB0) / 1024),
                        rnd / 1e6, (double)(busy - res - rd - rnd) / 1e6);
#ifdef RECOMP_GL
                fprintf(stderr, "  waiting for the render thread %.1f | create tex %.1f, destroy tex %.1f, unlock %.1f, load %.1f, create mesh %.1f, destroy mesh %.1f\n",
                        (g_gfxWaitNs - wait0) / 1e6,
                        (g_gfxResCatNs[0] - cat0[0]) / 1e6, (g_gfxResCatNs[1] - cat0[1]) / 1e6, (g_gfxResCatNs[2] - cat0[2]) / 1e6,
                        (g_gfxResCatNs[3] - cat0[3]) / 1e6, (g_gfxResCatNs[4] - cat0[4]) / 1e6, (g_gfxResCatNs[5] - cat0[5]) / 1e6);
#endif
            }
        }
        // test hook: RECOMP_TRACE_TICKSEQ: one char per tick (frames sent during it; '+' = tick
        // took over 8 ms), with the gap since the previous tick when it exceeds 20 ms
        static const bool seq = getenv("RECOMP_TRACE_TICKSEQ") != nullptr;
        if (seq) {
            static std::string line;
            static uint64_t prevTick;
            uint64_t end = SDL_GetTicksNS();
            if (prevTick && tickStart - prevTick > 20000000) line += "[" + std::to_string((tickStart - prevTick) / 1000000) + "]";
            prevTick = tickStart;
            static uint32_t prevFlips;
            uint32_t flips = RD32(0x758364);
            // frames sent / flips (limiter calls) / limiter skip counter
            if (CACHED_ENV("RECOMP_TRACE_TICKSEQ")[0] == '2')
                line += std::to_string(g_framesThisTick) + "/" + std::to_string(flips - prevFlips) + "/" + std::to_string(RD32(0x670ad8)) + " ";
            else
                line += (char)('0' + SDL_min(g_framesThisTick, 9u));
            prevFlips = flips;
            if (end - tickStart > 8000000) line += "+" + std::to_string((end - tickStart) / 1000000);
            if (line.size() >= 120) { fprintf(stderr, "ticks %s\n", line.c_str()); line.clear(); }
        }
        if (RD32(0x5eb6f8)) rt_event_set(RD32(0x5eb6f0));
    }
}

// 0x407260 WinMain(hInstance, hPrev, cmdLine, show): the original parsed the command line, read
// its settings, created the window and the devices, started the file I/O thread and called the
// PS2 main(). Settings come from the port's graphics.ini instead of the registry.
// ---------------------------------------------------------------------------------------------
// Enhancements in the spirit of DDGModernizer (github.com/hunterbridges/DDGModernizer), read from
// graphics.ini next to the CGraphic keys:
//   fullscreen   = 0 | 1          borderless fullscreen at start (F11 / Alt+Return toggle)
//   aspect       = auto | <x>     3D projection X aspect; auto = (4/3 * scrheight) / scrwidth,
//                                 1.0 is the original 4:3 (UI is stretched, not re-laid out)
//   aspecty      = <y>            3D projection Y aspect (original 0.5)
//   drawdistance = auto | original | <n>
//                                 auto: the game's own controller (0x47e1d0) steering the draw
//                                 distance by the load measured here (DrawLoadTick); original:
//                                 the same controller fed with the original timing (always 0.45
//                                 on a PC); <n>: fixed RendPow (DDGModernizer's patch; 0.9 is the
//                                 exe's default value)
//   ddmin, ddmax = <n>            auto: range of the draw distance (0.45, 1.0; the PSP version
//                                 used 0.625-1.0 on a 30 frames/s budget)
//   ddload       = <fraction>     auto: share of the frame period the busiest stage is steered
//                                 to (0.80)
//   okidist      = <OkiDistArg>   distance of placed objects with a fixed draw distance (0.4)
//   frameskip    = <n>            skip up to n frames in a row when behind (0 = draw every frame)
//   maxfps       = <n>            draw at most n frames per second (60/n ticks apart; 0 = 60)
// DDGModernizer's aspect patches are applied when the exe is loaded (recomp/exepatch.py, host/
// runtime.c read_exe) with neutral values that these settings replace; its draw distance patches
// are a run-time switch (plan.SETTING_DD_FIXED, MEM_OVERRIDES / BRANCH_OVERRIDES).

struct GameSettings {
    bool fullscreen = false;
    float aspectX = 0;          // 0 = auto
    float aspectY = 0.5f;
    float rendPow = 0;          // 0 = auto
    bool ddOriginal = false;    // drawdistance = original
    float ddMin = 0.45f, ddMax = 1.0f, ddLoad = 0.80f;
    float okiDist = 0.4f;
    int frameskip = 0;
    int maxfps = 0;
};

static std::string PrefDir();

static GameSettings LoadGameSettings()
{
    GameSettings gs;
    std::string path = PrefDir() + "/graphics.ini";
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return gs;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[64];
        if (sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        float v = (float)atof(val);
        if (!SDL_strcasecmp(key, "fullscreen")) gs.fullscreen = atoi(val) != 0;
        else if (!SDL_strcasecmp(key, "aspect")) gs.aspectX = SDL_strcasecmp(val, "auto") ? v : 0;
        else if (!SDL_strcasecmp(key, "aspecty") && v > 0) gs.aspectY = v;
        else if (!SDL_strcasecmp(key, "drawdistance")) {
            gs.ddOriginal = !SDL_strcasecmp(val, "original");
            gs.rendPow = SDL_strcasecmp(val, "auto") && v > 0 ? v : 0;
        }
        else if (!SDL_strcasecmp(key, "ddmin") && v > 0) gs.ddMin = v;
        else if (!SDL_strcasecmp(key, "ddmax") && v > 0) gs.ddMax = v;
        else if (!SDL_strcasecmp(key, "ddload") && v > 0) gs.ddLoad = SDL_clamp(v, 0.2f, 1.5f);
        else if (!SDL_strcasecmp(key, "okidist") && v > 0) gs.okiDist = v;
        else if (!SDL_strcasecmp(key, "frameskip")) gs.frameskip = SDL_clamp(atoi(val), 0, 10);
        else if (!SDL_strcasecmp(key, "maxfps")) gs.maxfps = SDL_max(atoi(val), 0);
    }
    fclose(f);
    return gs;
}

static void WRF(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); WR32(a, u); }

static float g_okiDist = 0.4f, g_ddMin = 0.45f;

// Draw distance: rendPow > 0 fixed (DDGModernizer's patches, as a switch), 0 auto (the game's
// controller fed with the measured load, plan.SETTING_DD_*), < 0 original (the controller with
// the original timing). At startup and from the options menu.
static void SetDrawDistance(float rendPow, float ddMax)
{
    if (rendPow > 0) {
        WR32(0x7ffc0008, 1);            // plan.SETTING_DD_FIXED
        WRF(0x7ffc000c, g_okiDist);     // plan.SETTING_OKIDIST
        WRF(0x5db0f8, rendPow);         // RendPow
        WRF(0x4c6ed4, rendPow);         // second copy DDGModernizer also sets
        WRF(0x5db0f4, g_okiDist);       // OkiDistArg
    } else {
        WR32(0x7ffc0008, 0);            // the game recomputes RendPow every tick
    }
    // the load feeds the controller also with a fixed draw distance (its result then goes to a
    // scratch slot), so that switching to auto continues from an adapted value
    bool feed = rendPow >= 0;
    if (feed && !RD32(0x7ffc0014)) WR32(0x730c60, 0);  // original's hold (plan.BRANCH_OVERRIDES)
    g_ddAuto = feed;
    WR32(0x7ffc0014, feed);             // plan.SETTING_DD_AUTO
    WRF(0x7ffc0024, SDL_max(ddMax, g_ddMin));          // plan.SETTING_DD_MAX
}

static void SetMaxFps(int maxfps)
{
    g_drawInterval = maxfps > 0 && maxfps < 60 ? (uint32_t)SDL_clamp((60 + maxfps / 2) / maxfps, 1, 6) : 1;
}

static void ApplyGameSettings(const GameSettings &gs)
{
    const GRAPHIC_CONFIG &c = CGraphic::m_Config;
    float ax = gs.aspectX > 0 ? gs.aspectX : (4.0f * c.scrheight / 3.0f) / (float)c.scrwidth;
    WRF(0x7ffc0000, ax);                // plan.SETTING_ASPECT_X (read by the patched aspect sites)
    WRF(0x7ffc0004, gs.aspectY);        // plan.SETTING_ASPECT_Y
    g_okiDist = gs.okiDist;
    g_ddMin = gs.ddMin;
    WR32(0x7ffc0018, 0);                // plan.SETTING_DD_LOAD
    WR32(0x7ffc001c, (uint32_t)lroundf(gs.ddLoad * kLoadFrame));   // plan.SETTING_DD_BUDGET
    WRF(0x7ffc0020, gs.ddMin);          // plan.SETTING_DD_MIN
    WRF(0x7ffc0028, 0.006f);            // plan.SETTING_DD_DOWN: 0.36 per second at most
    WRF(0x7ffc002c, 0.002f);            // plan.SETTING_DD_UP: 0.12 per second at most
    WRF(0x7ffc0030, 0.0008f);           // plan.SETTING_DD_UP2
    SetDrawDistance(gs.ddOriginal ? -1 : gs.rendPow, gs.ddMax);
    g_frameskipMax = gs.frameskip;
    SetMaxFps(gs.maxfps);
    char dd[40];
    if (gs.rendPow > 0) snprintf(dd, sizeof(dd), "%.2f, okidist %.2f", gs.rendPow, gs.okiDist);
    else if (!gs.ddOriginal) snprintf(dd, sizeof(dd), "auto %.2f-%.2f at %.0f%% load", gs.ddMin, SDL_max(gs.ddMax, gs.ddMin), gs.ddLoad * 100);
    else snprintf(dd, sizeof(dd), "original");
    fprintf(stderr, "settings: %dx%d, aspect %.4f/%.2f, draw distance %s, frameskip %d, %d frames/s at most%s\n",
            c.scrwidth, c.scrheight, ax, gs.aspectY, dd, gs.frameskip, 60 / (int)g_drawInterval,
            gs.fullscreen ? ", fullscreen" : "");
}

NATIVE N_00407260(void)
{
    rt_call(0x407170, 0, nullptr);                  // general settings (translated)
    CGraphic::LoadConfig();
    GameSettings settings = LoadGameSettings();
    CGraphic::m_Config.windowed = 1;                // the host window; fullscreen is set below
    if (settings.fullscreen) SDL_SetWindowFullscreen(g_window, true);
    ApplyGameSettings(settings);
    if (!CGraphic::Init(g_window)) {
        fprintf(stderr, "CGraphic::Init failed: %s\n", CGraphic::GetLastError());
        _exit(1);
    }
    CGraphic::SetDeferredPresent(true);
    {
        OverlayHost oh;
        oh.current.scrWidth = CGraphic::m_Config.scrwidth;
        oh.current.scrHeight = CGraphic::m_Config.scrheight;
        oh.current.drawDistance = settings.ddOriginal ? -1 : settings.rendPow;
        oh.current.ddMax = settings.ddMax;
        oh.current.maxfps = settings.maxfps;
        oh.current.fpsCounter = CGraphic::FpsCounter();
        oh.iniPath = PrefDir() + "/graphics.ini";
        oh.apply = [](const OverlayOptions &o) {
            SetDrawDistance(o.drawDistance, o.ddMax);
            SetMaxFps(o.maxfps);
            CGraphic::SetFpsCounter(o.fpsCounter);
        };
        oh.drawDistanceNow = [] { float v; uint32_t u = RD32(0x5db0f8); memcpy(&v, &u, 4); return v; };
        OverlayInit(oh);
    }
    {
        uint32_t hwnd = RD32(0x4c8bdc);
        if (!rt_call(0x409060, 1, &hwnd)) fprintf(stderr, "sound init failed\n");     // CSound init (translated)
    }
    CGs::Reset();
    g_texture.Init();
    g_vertex.Init();
    CDma::SetVu1Handler(&g_vertex);
    DmaStart();
    rt_call(0x401430, 0, nullptr);                  // file I/O thread
    uint32_t args[2] = {0, 0};
    rt_call(0x478ed0, 2, args);                     // PS2 main(0, 0)
    RT_RETURN(0, 16);
}

// ---------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------
// Game folder: the Densha de GO! FINAL install with cddata/ and the unprotected perfect.exe.
// Chosen by, in order: command line, DENGO_FINAL_DIR, the folder remembered in game.ini (pref
// path), or the settings window (host/launcher.cpp). DENGO_FINAL_EXE overrides the exe.

static std::string PrefDir()
{
    std::string d;
    if (const char *o = getenv("RECOMP_PREF_DIR")) return o;          // tests and benchmarks
    if (char *pref = SDL_GetPrefPath("TAITO", "DengoFinal")) {
        d = pref;
        SDL_free(pref);
        if (!d.empty() && d.back() == '/') d.pop_back();
    }
    return d;
}

static std::string LoadSavedGameDir()
{
    std::string line, path = PrefDir() + "/game.ini";
    if (FILE *f = fopen(path.c_str(), "r")) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), f)) {
            line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.rfind("gamedir=", 0) == 0) { fclose(f); return line.substr(8); }
        }
        fclose(f);
    }
    return "";
}

static void SaveGameDir(const std::string &dir)
{
    std::string path = PrefDir() + "/game.ini";
    if (FILE *f = fopen(path.c_str(), "w")) {
        fprintf(f, "gamedir=%s\n", dir.c_str());
        fclose(f);
    }
}

static bool IsDir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

// perfect.exe, or perfect2.exe (the name earlier versions of this port used) when only that one
// fits this build.
static std::string ExePath(const std::string &dir)
{
    const char *e = getenv("DENGO_FINAL_EXE");
    if (e && *e) return e;
    for (const char *name : {"perfect.exe", "perfect2.exe"})
        if (rt_check_exe((dir + "/" + name).c_str()) == RT_EXE_OK) return dir + "/" + name;
    return dir + "/perfect.exe";
}

// Empty if the folder can be used, otherwise a message for the player.
#if defined(RECOMP_GL) && !defined(__APPLE__)
// SDL loads libGLESv2.so.2 / libEGL.so.1. On handheld firmwares those can be the wrong library:
// ArkOS (RK3326) has libEGL.so.1 as the glvnd dispatcher, which ends up in Mesa and fails on the
// Mali GPU, while libEGL.so / libGLESv2.so point at the vendor driver libMali.so. So the vendor
// driver is tried first, then the usual names; the first library that loads and has the entry
// point is passed to SDL (unless SDL_OPENGL_LIBRARY / SDL_EGL_LIBRARY are set).
static void PickGLLibraries()
{
    // Desktop sessions (X11, Wayland) have consistent libraries, and SDL's X11 GLX path needs
    // libGL, not libGLESv2: only the console (KMSDRM) handhelds need the choice below.
    if ((getenv("DISPLAY") && *getenv("DISPLAY")) || (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY"))) return;
    static const char *const dirs[] = {"", "/usr/lib/aarch64-linux-gnu/", "/usr/lib64/", "/usr/lib/",
                                       "/usr/local/lib/", "/usr/lib/mali/"};
    auto pick = [&](const char *hint, std::initializer_list<const char *> names, const char *symbol) {
        if (SDL_GetHint(hint)) { fprintf(stderr, "gl: %s=%s (from environment)\n", hint, SDL_GetHint(hint)); return; }
        for (const char *dir : dirs)
            for (const char *name : names) {
                std::string path = std::string(dir) + name;
                if (*dir && access(path.c_str(), F_OK) != 0) continue;
                SDL_SharedObject *h = SDL_LoadObject(path.c_str());
                if (!h) { if (*dir) fprintf(stderr, "gl: %s: %s\n", path.c_str(), SDL_GetError()); continue; }
                bool ok = SDL_LoadFunction(h, symbol) != nullptr;
                SDL_UnloadObject(h);
                if (!ok) { fprintf(stderr, "gl: %s has no %s\n", path.c_str(), symbol); continue; }
                SDL_SetHint(hint, path.c_str());
                fprintf(stderr, "gl: %s=%s\n", hint, path.c_str());
                return;
            }
        fprintf(stderr, "gl: no library with %s found\n", symbol);
    };
    pick(SDL_HINT_OPENGL_LIBRARY, {"libMali.so", "libmali.so", "libmali.so.1", "libGLESv2.so.2", "libGLESv2.so"},
         "glDrawArrays");
    pick(SDL_HINT_EGL_LIBRARY, {"libMali.so", "libmali.so", "libmali.so.1", "libEGL.so.1", "libEGL.so"},
         "eglChooseConfig");
}
#endif

static std::string CheckGameDir(const std::string &dir, const std::string &exe)
{
    if (dir.empty() || !IsDir(dir)) return "The folder does not exist:\n" + dir;
    if (!IsDir(dir + "/cddata"))
        return "This folder has no cddata folder, so it is not a Densha de GO! FINAL install:\n" + dir;
    switch (rt_check_exe(exe.c_str())) {
    case RT_EXE_OK: return "";
    case RT_EXE_MISSING: return "perfect.exe was not found:\n" + exe;
    case RT_EXE_SAFEDISC:
        return "This perfect.exe is the SafeDisc-protected original from the CD, which cannot be used.\n"
               "An unprotected perfect.exe of the same version is needed:\n" + exe;
    case RT_EXE_NOT_PE: return "This is not a Windows program:\n" + exe;
    case RT_EXE_UNKNOWN: return "This is not a version of perfect.exe that this build knows:\n" + exe;
    default:
        return "This perfect.exe differs from the one this build was made from (another version or a\n"
               "translation patch). Generate and build again from this exe:\n" + exe;
    }
}

// the exe the settings select: exePath, or the folder's
static std::string ExeFor(const std::string &dir, const std::string &exePath)
{
    return exePath.empty() ? ExePath(dir) : exePath;
}

static std::string CheckGameDirForLauncher(const std::string &dir, const std::string &exePath)
{
    return CheckGameDir(dir, ExeFor(dir, exePath));
}

#ifdef RECOMP_JIT
// the settings window prepares the game code for the chosen exe (host/jit.cpp)
static const LauncherPrepare kPrepare = {
    [](const std::string &dir, const std::string &exePath) { rt_jit_start(ExeFor(dir, exePath).c_str()); },
    [](const std::string &dir, const std::string &exePath, std::string &text) {
        char buf[256];
        int state = rt_jit_poll(ExeFor(dir, exePath).c_str(), buf, sizeof buf);
        text = buf;
        return state;
    },
};
#endif

int main(int argc, char **argv)
{
    rt_install_crash_handler();
    rt_profile_start();
    rt_reserve_guest_memory();          // before SDL/GPU drivers take address space (see runtime.c)
    bool settingsFlag = false, play = false;
    std::string dir, exe;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--settings" || a == "--choose-folder") settingsFlag = true;
        else if (a == "--play") play = true;                           // skip the launcher
        else if (a == "W" || a == "w") continue;                       // windowed flag (CGraphic)
        else if (dir.empty()) dir = a;
        else if (exe.empty()) exe = a;
    }
    if (dir.empty()) {
        const char *e = getenv("DENGO_FINAL_DIR");
        if (e && *e) dir = e;
    }
    if (dir.empty()) dir = LoadSavedGameDir();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }

    // Settings window: when asked for, when enabled ("Show at startup"), when Shift is held, or
    // when the game folder is unusable.
    std::string pref = PrefDir();
    LauncherSettings ls = LoadLauncherSettings(pref, dir);
    ls.hasFpsCounter = CGraphic::HasFpsCounter();
    if (!exe.empty()) ls.exePath = exe;              // given on the command line
    std::string problem = CheckGameDir(dir, ExeFor(dir, ls.exePath));
    SDL_PumpEvents();
    bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    bool show = settingsFlag || ls.showLauncher || shift || !problem.empty();
    const LauncherPrepare *prep = nullptr;
#ifdef RECOMP_JIT
    // the settings window is also where the game code is prepared, the first time for an exe
    if (problem.empty() && !rt_jit_cached(ExeFor(dir, ls.exePath).c_str())) show = true;
    prep = &kPrepare;
#endif
    if (!play && show) {
        if (!RunLauncher(ls, CheckGameDirForLauncher, prep)) return 0;
        SaveLauncherSettings(pref, ls);
        dir = ls.gameDir;
        SaveGameDir(dir);
        problem = CheckGameDir(dir, ExeFor(dir, ls.exePath));
    }
    if (!problem.empty()) {
        fprintf(stderr, "game folder: %s\n(run with --settings to choose one)\n", problem.c_str());
        return 1;
    }
    exe = ExeFor(dir, ls.exePath);
    fprintf(stderr, "game folder: %s\n", dir.c_str());
#ifdef RECOMP_JIT
    // the game code (prepared in the settings window, or now); then the full check of the exe
    // against the tables it brought
    if (!rt_jit_ensure(exe.c_str())) return 1;
    problem = CheckGameDir(dir, exe);
    if (!problem.empty()) { fprintf(stderr, "game folder: %s\n", problem.c_str()); return 1; }
#endif

    SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef RECOMP_GL
    // GraphicGL.cpp: OpenGL ES 3.0 (desktop GL 3.3 core on macOS, for testing)
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);      // the scene has its own depth buffer
    wflags |= SDL_WINDOW_OPENGL;
#ifndef __APPLE__
    PickGLLibraries();
#endif
#endif
    g_window = SDL_CreateWindow("Densha de GO! FINAL (recompiled)", 640, 480, wflags);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s (video driver %s)\n", SDL_GetError(), SDL_GetCurrentVideoDriver());
        return 1;
    }

    if (!rt_init(exe.c_str(), dir.c_str())) return 1;
    CVertex::s_pDebugClock = TickMs;
    // Saves (the PS2 memory card, kept under %APPDATA%\TAITO\perfect by the PC port)
    if (!pref.empty()) {
        rt_set_appdata_dir((pref + "/AppData").c_str());
        mkdir((pref + "/AppData").c_str(), 0755);
    }
    rt_call(0x4a45af, 0, nullptr);                  // CRT entry -> WinMain
    return 0;
}
