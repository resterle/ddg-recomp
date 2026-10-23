// win32.c - the Win32 functions the translated code imports, implemented on the host.
//
// Each import reads its stdcall arguments with ARG(i) and returns with RT_RETURN(value, bytes),
// popping its arguments like the original. Imports that are never expected to run (window
// management that the native WinMain replaces, DirectX) are left out and trap with their name.

#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// from runtime.c
int rt_wait(uint32_t ms);
int rt_wait_objs(const void *const *objs, int n, uint32_t ms);  // wait for signals for these objects
void rt_signal(void);
void rt_signal_obj(const void *obj);                             // wake the threads waiting for obj
int rt_handle_kind(uint32_t h);
void rt_thread_resume(RtThread *t);
int rt_thread_finished(RtThread *t);
uint32_t rt_thread_exitcode(RtThread *t);
void rt_thread_exit(uint32_t code);
uint32_t *rt_tls(void);
uint32_t *rt_last_error(void);
RtThread *rt_thread_by_id(uint32_t tid);
int rt_message_get(uint32_t *msg, uint32_t *wparam, uint32_t *lparam, int remove, int wait);

#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define INVALID_HANDLE 0xffffffffu
#define STD_IN 0xfffffff6u
#define STD_OUT 0xfffffff5u
#define STD_ERR 0xfffffff4u

static void set_error(uint32_t e) { *rt_last_error() = e; }

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}
static uint64_t g_start_us;

// ---------------------------------------------------------------------------------------------
// Sync objects

typedef struct { int manual, signaled; } Event;
typedef struct { int count, max; } Semaphore;
typedef struct { uint32_t owner; int count; } CritSec;

#define MAX_CS 512
static struct { uint32_t addr; CritSec cs; } g_cs[MAX_CS];
static CritSec *crit(uint32_t addr)
{
    for (int i = 0; i < MAX_CS; i++) if (g_cs[i].addr == addr) return &g_cs[i].cs;
    for (int i = 0; i < MAX_CS; i++) if (!g_cs[i].addr) { g_cs[i].addr = addr; return &g_cs[i].cs; }
    RT_Fatal("too many critical sections", addr);
    return NULL;
}

// Signalled state of a waitable handle; consumes auto-reset events / semaphore counts if `take`.
static int signaled(uint32_t h, int take)
{
    switch (rt_handle_kind(h)) {
    case H_EVENT: {
        Event *e = rt_handle_get(h, H_EVENT);
        if (!e->signaled) return 0;
        if (take && !e->manual) e->signaled = 0;
        return 1;
    }
    case H_SEMAPHORE: {
        Semaphore *s = rt_handle_get(h, H_SEMAPHORE);
        if (s->count <= 0) return 0;
        if (take) s->count--;
        return 1;
    }
    case H_THREAD: return rt_thread_finished(rt_handle_get(h, H_THREAD));
    default: return 1;
    }
}

// The object a handle's waiters wait for (rt_signal_obj is called with it when it changes).
static const void *wait_obj(uint32_t h)
{
    switch (rt_handle_kind(h)) {
    case H_EVENT: return rt_handle_get(h, H_EVENT);
    case H_SEMAPHORE: return rt_handle_get(h, H_SEMAPHORE);
    case H_THREAD: return rt_handle_get(h, H_THREAD);
    default: return NULL;
    }
}

static uint32_t wait_handles(int n, const uint32_t *h, int all, uint32_t ms)
{
    uint64_t deadline = ms == 0xffffffffu ? 0 : now_us() + (uint64_t)ms * 1000;
    const void *objs[64];
    int nobj = 0;
    for (int i = 0; i < n && i < 64; i++) if ((objs[nobj] = wait_obj(h[i]))) nobj++;
    for (;;) {
        if (all) {
            int ok = 1;
            for (int i = 0; i < n; i++) ok &= signaled(h[i], 0);
            if (ok) { for (int i = 0; i < n; i++) signaled(h[i], 1); return WAIT_OBJECT_0; }
        } else {
            for (int i = 0; i < n; i++) if (signaled(h[i], 1)) return WAIT_OBJECT_0 + i;
        }
        if (ms == 0) return WAIT_TIMEOUT;
        if (deadline) {
            uint64_t t = now_us();
            if (t >= deadline) return WAIT_TIMEOUT;
            rt_wait_objs(objs, nobj, (uint32_t)((deadline - t + 999) / 1000));
        } else {
            rt_wait_objs(objs, nobj, 0xffffffffu);
        }
    }
}

static void imp_CreateEventA(void) { Event *e = calloc(1, sizeof(Event)); e->manual = ARG(1); e->signaled = ARG(2); RT_RETURN(rt_handle_new(H_EVENT, e), 16); }
static void imp_SetEvent(void) { Event *e = rt_handle_get(ARG(0), H_EVENT); if (e) { e->signaled = 1; rt_signal_obj(e); } RT_RETURN(e != NULL, 4); }
static void imp_ResetEvent(void) { Event *e = rt_handle_get(ARG(0), H_EVENT); if (e) e->signaled = 0; RT_RETURN(e != NULL, 4); }
static void imp_CreateSemaphoreA(void) { Semaphore *s = calloc(1, sizeof(Semaphore)); s->count = ARG(1); s->max = ARG(2); RT_RETURN(rt_handle_new(H_SEMAPHORE, s), 16); }
static void imp_ReleaseSemaphore(void)
{
    Semaphore *s = rt_handle_get(ARG(0), H_SEMAPHORE);
    if (!s) RT_RETURN(0, 12);
    if (ARG(2)) WR32(ARG(2), s->count);
    s->count += ARG(1);
    if (s->count > s->max) s->count = s->max;
    rt_signal_obj(s);
    RT_RETURN(1, 12);
}
static void imp_WaitForSingleObject(void) { uint32_t h = ARG(0); RT_RETURN(wait_handles(1, &h, 0, ARG(1)), 8); }
static void imp_WaitForMultipleObjects(void)
{
    uint32_t n = ARG(0), hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = RD32(ARG(1) + 4 * i);
    RT_RETURN(wait_handles((int)n, hs, ARG(2), ARG(3)), 16);
}
static void imp_InitializeCriticalSection(void) { CritSec *c = crit(ARG(0)); c->owner = 0; c->count = 0; RT_RETURN(0, 4); }
static void imp_DeleteCriticalSection(void)
{
    for (int i = 0; i < MAX_CS; i++) if (g_cs[i].addr == ARG(0)) g_cs[i].addr = 0;
    RT_RETURN(0, 4);
}
static void imp_EnterCriticalSection(void)
{
    CritSec *c = crit(ARG(0));
    uint32_t me = rt_thread_id(rt_self());
    while (c->owner && c->owner != me) {
        const void *o = c;
        rt_wait_objs(&o, 1, 0xffffffffu);
        c = crit(ARG(0));
    }
    c->owner = me;
    c->count++;
    RT_RETURN(0, 4);
}
static void imp_LeaveCriticalSection(void)
{
    CritSec *c = crit(ARG(0));
    if (c->count > 0 && --c->count == 0) { c->owner = 0; rt_signal_obj(c); }
    RT_RETURN(0, 4);
}
static void imp_InterlockedIncrement(void) { uint32_t v = RD32(ARG(0)) + 1; WR32(ARG(0), v); RT_RETURN(v, 4); }
static void imp_InterlockedDecrement(void) { uint32_t v = RD32(ARG(0)) - 1; WR32(ARG(0), v); RT_RETURN(v, 4); }

static void imp_CloseHandle(void)
{
    uint32_t h = ARG(0);
    int k = rt_handle_kind(h);
    if (k == H_FILE) { FILE *f = rt_handle_get(h, H_FILE); if (f) fclose(f); }
    if (k != H_THREAD) rt_handle_close(h);   // thread objects stay for WaitForSingleObject by others
    RT_RETURN(1, 4);
}

// ---------------------------------------------------------------------------------------------
// Threads

static void imp_CreateThread(void)
{
    uint32_t tid;
    uint32_t h = rt_create_thread(ARG(2), ARG(3), (ARG(4) & 4) != 0, &tid);
    if (ARG(5)) WR32(ARG(5), tid);
    RT_RETURN(h, 24);
}
static void imp_ResumeThread(void)
{
    RtThread *t = rt_handle_get(ARG(0), H_THREAD);
    if (t) rt_thread_resume(t);
    RT_RETURN(1, 4);
}
static void imp_ExitThread(void) { rt_thread_exit(ARG(0)); }
static void imp_GetCurrentThread(void) { RT_RETURN(0xfffffffeu, 0); }
static void imp_GetCurrentThreadId(void) { RT_RETURN(rt_thread_id(rt_self()), 0); }
static void imp_GetCurrentProcess(void) { RT_RETURN(0xffffffffu, 0); }
static void imp_SetThreadPriority(void) { RT_RETURN(1, 8); }
static void imp_GetThreadPriority(void) { RT_RETURN(0, 4); }
static void imp_Sleep(void)
{
    uint32_t ms = ARG(0);
    rt_leave();
    if (ms == 0) sched_yield(); else usleep(ms * 1000);
    rt_enter();
    RT_RETURN(0, 4);
}
static void imp_TlsGetValue(void) { RT_RETURN(ARG(0) < 64 ? rt_tls()[ARG(0)] : 0, 4); }
static void imp_TlsSetValue(void) { if (ARG(0) < 64) rt_tls()[ARG(0)] = ARG(1); RT_RETURN(1, 8); }
static void imp_GetLastError(void) { RT_RETURN(*rt_last_error(), 0); }
static void imp_SetLastError(void) { set_error(ARG(0)); RT_RETURN(0, 4); }
static void imp_ExitProcess(void) { fprintf(stderr, "ExitProcess(%u)\n", ARG(0)); fflush(stderr); _exit((int)ARG(0)); }
static void imp_TerminateProcess(void) { fprintf(stderr, "TerminateProcess(%u)\n", ARG(1)); fflush(stderr); _exit((int)ARG(1)); }

// ---------------------------------------------------------------------------------------------
// Memory

static void imp_HeapAlloc(void) { RT_RETURN(rt_heap_alloc(ARG(2), (ARG(1) & 8) != 0), 12); }
static void imp_HeapFree(void) { rt_heap_free(ARG(2)); RT_RETURN(1, 12); }
static void imp_HeapReAlloc(void) { RT_RETURN(rt_heap_realloc(ARG(2), ARG(3), (ARG(1) & 8) != 0), 16); }
static void imp_HeapSize(void) { RT_RETURN(rt_heap_size(ARG(2)), 12); }
static void imp_HeapCreate(void) { RT_RETURN(0x00ee0000u, 12); }
static void imp_HeapDestroy(void) { RT_RETURN(1, 4); }
static void imp_GetProcessHeap(void) { RT_RETURN(0x00ee0000u, 0); }

// VirtualAlloc: reservations come from a bump region; commits inside a reservation just return
// the address (all of MEM is readable/writable and zero-initialised).
static uint32_t g_virtTop = RT_VIRT_BASE;
static void imp_VirtualAlloc(void)
{
    uint32_t addr = ARG(0), size = ARG(1);
    if (addr) RT_RETURN(addr, 16);
    size = (size + 0xffff) & ~0xffffu;
    if (g_virtTop + size > RT_VIRT_END) RT_RETURN(0, 16);
    uint32_t a = g_virtTop;
    g_virtTop += size;
    RT_RETURN(a, 16);
}
static void imp_VirtualFree(void) { RT_RETURN(1, 12); }
uint32_t rt_virt_top(void) { return g_virtTop; }
static void imp_IsBadReadPtr(void) { RT_RETURN(0, 8); }
static void imp_IsBadWritePtr(void) { RT_RETURN(0, 8); }
static void imp_IsBadCodePtr(void) { RT_RETURN(0, 4); }
static void imp_CoTaskMemAlloc(void) { RT_RETURN(rt_heap_alloc(ARG(0), 0), 4); }
static void imp_CoTaskMemFree(void) { rt_heap_free(ARG(0)); RT_RETURN(0, 4); }

// ---------------------------------------------------------------------------------------------
// Files

static void imp_CreateFileA(void)
{
    char win[512], host[1200];
    rt_strget(ARG(0), win, sizeof(win));
    rt_map_path(win, host, sizeof(host));
    uint32_t access = ARG(1), disp = ARG(4);
    const char *mode = "rb";
    if (access & 0x40000000u) {                   // GENERIC_WRITE
        if (disp == 2 || disp == 1) mode = (access & 0x80000000u) ? "w+b" : "wb";     // CREATE_ALWAYS/NEW
        else if (disp == 4) { FILE *t = fopen(host, "ab"); if (t) fclose(t); mode = "r+b"; }  // OPEN_ALWAYS
        else mode = "r+b";
    }
    FILE *f = fopen(host, mode);
    if (!f) {
        set_error(2);   // ERROR_FILE_NOT_FOUND
        fprintf(stderr, "CreateFileA: cannot open %s (%s); stack:", win, host);
        { int n = 0; for (uint32_t q = R.esp; q < R.esp + 0x400 && n < 12; q += 4) { uint32_t v = RD32(q); if (v >= 0x401000 && v < 0x4b2000) { fprintf(stderr, " %08x", v); n++; } } fprintf(stderr, "\n"); }
        RT_RETURN(INVALID_HANDLE, 28);
    }
    RT_RETURN(rt_handle_new(H_FILE, f), 28);
}

static FILE *file_of(uint32_t h)
{
    if (h == STD_OUT || h == STD_ERR) return stderr;
    return rt_handle_get(h, H_FILE);
}

uint64_t g_readNs, g_readBytes;                     // RECOMP_TRACE_SPIKES
static void imp_ReadFile(void)
{
    FILE *f = file_of(ARG(0));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rt_guest_commit(ARG(1), ARG(2));
    size_t n = f ? fread(MEM + ARG(1), 1, ARG(2), f) : 0;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    g_readNs += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
    g_readBytes += n;
    if (ARG(3)) WR32(ARG(3), (uint32_t)n);
    RT_RETURN(f != NULL, 20);
}
static void imp_WriteFile(void)
{
    FILE *f = file_of(ARG(0));
    size_t n = f ? fwrite(MEM + ARG(1), 1, ARG(2), f) : 0;
    if (f == stderr) fflush(stderr);
    if (ARG(3)) WR32(ARG(3), (uint32_t)n);
    RT_RETURN(f != NULL, 20);
}
static void imp_SetFilePointer(void)
{
    FILE *f = file_of(ARG(0));
    if (!f) RT_RETURN(0xffffffffu, 16);
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)RD32(ARG(2)) << 32) | ARG(1));
    fseeko(f, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    int64_t pos = ftello(f);
    if (ARG(2)) WR32(ARG(2), (uint32_t)(pos >> 32));
    RT_RETURN((uint32_t)pos, 16);
}
static void imp_GetFileSize(void)
{
    FILE *f = file_of(ARG(0));
    if (!f) RT_RETURN(0xffffffffu, 8);
    off_t cur = ftello(f);
    fseeko(f, 0, SEEK_END);
    off_t size = ftello(f);
    fseeko(f, cur, SEEK_SET);
    if (ARG(1)) WR32(ARG(1), (uint32_t)((uint64_t)size >> 32));
    RT_RETURN((uint32_t)size, 8);
}
static void imp_SetEndOfFile(void) { FILE *f = file_of(ARG(0)); if (f) { fflush(f); ftruncate(fileno(f), ftello(f)); } RT_RETURN(f != NULL, 4); }
static void imp_FlushFileBuffers(void) { FILE *f = file_of(ARG(0)); if (f) fflush(f); RT_RETURN(1, 4); }
static void imp_GetFileType(void) { uint32_t h = ARG(0); RT_RETURN(h >= STD_ERR ? 2 : 1, 4); }   // CHAR / DISK
static void imp_GetStdHandle(void) { RT_RETURN(ARG(0), 4); }
static void imp_SetStdHandle(void) { RT_RETURN(1, 8); }
static void imp_CreateDirectoryA(void)
{
    char win[512], host[1200];
    rt_strget(ARG(0), win, sizeof(win));
    rt_map_path(win, host, sizeof(host));
    RT_RETURN(mkdir(host, 0755) == 0 || errno == EEXIST, 8);
}

// ---------------------------------------------------------------------------------------------
// Module, version, environment

static void imp_GetModuleHandleA(void) { RT_RETURN(ARG(0) ? 0 : 0x400000u, 4); }
static void imp_GetModuleFileNameA(void)
{
    const char *path = "C:\\perfect\\perfect.exe";
    uint32_t n = (uint32_t)strlen(path);
    if (n >= ARG(2)) n = ARG(2) - 1;
    memcpy(MEM + ARG(1), path, n);
    WR8(ARG(1) + n, 0);
    RT_RETURN(n, 12);
}
static void imp_LoadLibraryA(void) { char n[256]; rt_strget(ARG(0), n, sizeof(n)); fprintf(stderr, "LoadLibraryA(%s): not available\n", n); RT_RETURN(0, 4); }
static void imp_FreeLibrary(void) { RT_RETURN(1, 4); }
static void imp_GetProcAddress(void) { RT_RETURN(0, 8); }
static void imp_GetVersionExA(void)
{
    uint32_t p = ARG(0);
    WR32(p + 4, 5); WR32(p + 8, 1); WR32(p + 12, 2600); WR32(p + 16, 2);    // Windows XP
    WR8(p + 20, 0);
    RT_RETURN(1, 4);
}
static void imp_IsProcessorFeaturePresent(void) { RT_RETURN(0, 4); }
static void imp_SetUnhandledExceptionFilter(void) { RT_RETURN(0, 4); }
static void imp_RaiseException(void) { RT_Fatal("RaiseException", ARG(0)); }
static void imp_RtlUnwind(void) { RT_Fatal("RtlUnwind (C++ exception)", ARG(0)); }

// ---------------------------------------------------------------------------------------------
// Time

static void imp_GetTickCount(void) { RT_RETURN((uint32_t)((now_us() - g_start_us) / 1000), 0); }
static void imp_timeGetTime(void) { RT_RETURN((uint32_t)((now_us() - g_start_us) / 1000), 0); }
static void imp_timeBeginPeriod(void) { RT_RETURN(0, 4); }
static void imp_timeEndPeriod(void) { RT_RETURN(0, 4); }
static void imp_QueryPerformanceFrequency(void) { WR64(ARG(0), 1000000u); RT_RETURN(1, 4); }
// Counted from process start, like a freshly booted machine: the game keeps elapsed time in a
// float (frame limiter 0x4075a0), which has no sub-second precision after weeks of host uptime.
static void imp_QueryPerformanceCounter(void) { WR64(ARG(0), now_us() - g_start_us + 1000000u); RT_RETURN(1, 4); }
static void imp_GetLocalTime(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    uint32_t p = ARG(0);
    WR16(p, lt.tm_year + 1900); WR16(p + 2, lt.tm_mon + 1); WR16(p + 4, lt.tm_wday); WR16(p + 6, lt.tm_mday);
    WR16(p + 8, lt.tm_hour); WR16(p + 10, lt.tm_min); WR16(p + 12, lt.tm_sec); WR16(p + 14, 0);
    RT_RETURN(0, 4);
}

// timeSetEvent: periodic callback(uID, uMsg, dwUser, 0, 0) on its own emulated thread.
typedef struct { uint32_t id, delay, cb, user, periodic; volatile int alive; } MmTimer;
static void *mmtimer_main(void *arg);
static uint32_t g_nextTimer = 1;
static void imp_timeSetEvent(void)
{
    MmTimer *t = calloc(1, sizeof(MmTimer));
    t->id = g_nextTimer++;
    t->delay = ARG(0) ? ARG(0) : 1;
    t->cb = ARG(2);
    t->user = ARG(3);
    t->periodic = ARG(4) & 1;
    t->alive = 1;
    // the callback runs translated code: give it an emulated thread that loops natively
    extern uint32_t rt_create_native_thread(void *(*fn)(void *), void *arg);
    rt_create_native_thread(mmtimer_main, t);
    RT_RETURN(t->id, 20);
}
static MmTimer *g_timers[64];
static void *mmtimer_main(void *arg)
{
    MmTimer *t = arg;
    g_timers[t->id % 64] = t;
    uint64_t next = now_us() + t->delay * 1000ull;
    while (t->alive) {
        rt_leave();
        uint64_t n = now_us();
        if (next > n) usleep((useconds_t)(next - n));
        rt_enter();
        next += t->delay * 1000ull;
        if (!t->alive) break;
        uint32_t args[5] = {t->id, 0, t->user, 0, 0};
        rt_call(t->cb, 5, args);
        if (!t->periodic) break;
    }
    return NULL;
}
static void imp_timeKillEvent(void) { MmTimer *t = g_timers[ARG(0) % 64]; if (t && t->id == ARG(0)) t->alive = 0; RT_RETURN(0, 4); }

// ---------------------------------------------------------------------------------------------
// Messages (thread queues only; the window is native)

static void read_msg(uint32_t p, int remove, int wait, int *got)
{
    uint32_t m, w, l;
    *got = rt_message_get(&m, &w, &l, remove, wait);
    if (*got && p) {
        WR32(p, 0); WR32(p + 4, m); WR32(p + 8, w); WR32(p + 12, l);
        WR32(p + 16, (uint32_t)((now_us() - g_start_us) / 1000)); WR32(p + 20, 0); WR32(p + 24, 0);
    }
}
static void imp_GetMessageA(void)
{
    int got;
    read_msg(ARG(0), 1, 1, &got);
    RT_RETURN(RD32(ARG(0) + 4) == 0x12 ? 0 : 1, 16);   // WM_QUIT ends the loop
}
static void imp_PeekMessageA(void)
{
    int got;
    read_msg(ARG(0), ARG(4) & 1, 0, &got);
    RT_RETURN(got, 20);
}
static void imp_PostThreadMessageA(void) { rt_message_post(ARG(0), ARG(1), ARG(2), ARG(3)); RT_RETURN(1, 16); }
static void imp_GetQueueStatus(void) { RT_RETURN(0, 4); }
static void imp_MsgWaitForMultipleObjects(void)
{
    uint32_t n = ARG(0), hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = RD32(ARG(1) + 4 * i);
    RT_RETURN(wait_handles((int)n, hs, ARG(2), ARG(3)), 20);
}
static void imp_TranslateMessage(void) { RT_RETURN(0, 4); }
static void imp_DispatchMessageA(void) { RT_RETURN(0, 4); }
static void imp_RegisterWindowMessageA(void) { RT_RETURN(0xc000, 4); }
static void imp_SystemParametersInfoA(void) { RT_RETURN(1, 16); }
static void imp_SetCursor(void) { RT_RETURN(0, 4); }
// The game posts only one message: WM_CLOSE to its window from 0x4077d0 when "Exit" is chosen in
// the top menu (the original window procedure then destroyed the window and ended the message
// loop). Quit like closing the window does.
static void imp_PostMessageA(void)
{
    uint32_t msg = ARG(1);
    if (msg == 0x10 /* WM_CLOSE */ || msg == 0x12 /* WM_QUIT */) {
        fprintf(stderr, "quit (game exit)\n");
        fflush(stderr);
        _exit(0);
    }
    RT_RETURN(1, 16);
}
static void imp_MessageBeep(void) { RT_RETURN(1, 4); }
static void imp_MessageBoxA(void)
{
    char t[512], c[256];
    rt_strget(ARG(1), t, sizeof(t));
    rt_strget(ARG(2), c, sizeof(c));
    fprintf(stderr, "MessageBox [%s]: %s\n", c, t);
    RT_RETURN(1, 16);
}
extern uint8_t g_asyncKeys[256];
uint8_t g_asyncKeys[256];
static void imp_GetAsyncKeyState(void) { RT_RETURN(g_asyncKeys[ARG(0) & 0xff] ? 0x8000 : 0, 4); }

// ---------------------------------------------------------------------------------------------
// Registry: no settings stored, so the game uses its defaults.

static void imp_RegOpenKeyA(void) { RT_RETURN(2, 12); }
static void imp_RegOpenKeyExA(void) { RT_RETURN(2, 20); }
static void imp_RegQueryValueExA(void) { RT_RETURN(2, 24); }
static void imp_RegCloseKey(void) { RT_RETURN(0, 4); }

// ---------------------------------------------------------------------------------------------
// Strings, code pages (Shift-JIS, code page 932, like the Japanese original)

static void imp_lstrlenA(void) { RT_RETURN(ARG(0) ? rt_strlen(ARG(0)) : 0, 4); }
static void imp_lstrcpyA(void) { memmove(MEM + ARG(0), MEM + ARG(1), rt_strlen(ARG(1)) + 1); RT_RETURN(ARG(0), 8); }
static void imp_lstrcatA(void) { uint32_t n = rt_strlen(ARG(0)); memmove(MEM + ARG(0) + n, MEM + ARG(1), rt_strlen(ARG(1)) + 1); RT_RETURN(ARG(0), 8); }
static void imp_lstrcpynA(void)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), i = 0;
    for (; i + 1 < n && MEM[s + i]; i++) MEM[d + i] = MEM[s + i];
    if (n) MEM[d + i] = 0;
    RT_RETURN(d, 12);
}
static void imp_MulDiv(void)
{
    int64_t a = (int32_t)ARG(0), b = (int32_t)ARG(1), c = (int32_t)ARG(2);
    if (!c) RT_RETURN(0xffffffffu, 12);
    int64_t p = a * b;
    int64_t r = (p >= 0) == (c > 0) ? (p + c / 2) / c : (p - c / 2) / c;   // round half away from zero
    RT_RETURN((uint32_t)(int32_t)r, 12);
}
static void imp_GetACP(void) { RT_RETURN(932, 0); }
static void imp_GetOEMCP(void) { RT_RETURN(932, 0); }
static void imp_GetCPInfo(void)
{
    uint32_t p = ARG(1);
    memset(MEM + p, 0, 20);
    WR32(p, 2);                         // MaxCharSize
    WR8(p + 4, '?');                    // DefaultChar
    WR8(p + 6, 0x81); WR8(p + 7, 0x9f); WR8(p + 8, 0xe0); WR8(p + 9, 0xfc);   // lead byte ranges
    RT_RETURN(1, 8);
}
static void imp_GetUserDefaultLCID(void) { RT_RETURN(0x411, 0); }   // Japanese
// Byte-to-UTF-16 conversions are only needed for ASCII paths/strings here.
static void imp_MultiByteToWideChar(void)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t len = (int32_t)ARG(3);
    uint32_t n = len < 0 ? rt_strlen(src) + 1 : (uint32_t)len;
    if (!cap) RT_RETURN(n, 24);
    uint32_t i = 0;
    for (; i < n && i < cap; i++) WR16(dst + 2 * i, MEM[src + i]);
    RT_RETURN(i, 24);
}
static void imp_WideCharToMultiByte(void)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t len = (int32_t)ARG(3);
    uint32_t n = 0;
    if (len < 0) { while (RD16(src + 2 * n)) n++; n++; } else n = (uint32_t)len;
    if (!cap) RT_RETURN(n, 32);
    uint32_t i = 0;
    for (; i < n && i < cap; i++) { uint16_t c = RD16(src + 2 * i); WR8(dst + i, c < 0x80 ? c : '?'); }
    RT_RETURN(i, 32);
}
static void imp_GetStringTypeA(void) { RT_RETURN(0, 20); }
static void imp_GetStringTypeW(void) { RT_RETURN(0, 16); }
static void imp_LCMapStringA(void) { RT_RETURN(0, 24); }
static void imp_LCMapStringW(void) { RT_RETURN(0, 24); }
static void imp_GetLocaleInfoA(void) { RT_RETURN(0, 16); }
static void imp_GetLocaleInfoW(void) { RT_RETURN(0, 16); }
static void imp_FormatMessageA(void) { RT_RETURN(0, 28); }

// ---------------------------------------------------------------------------------------------
// COM: only reached by Movie/DirectShow (native stubs) and the shell folder lookup

static void imp_CoInitialize(void) { RT_RETURN(0, 4); }
static void imp_CoUninitialize(void) { RT_RETURN(0, 0); }
static void imp_CoFreeUnusedLibraries(void) { RT_RETURN(0, 0); }
static void imp_CoCreateInstance(void) { RT_RETURN(0x80004002u, 20); }  // E_NOINTERFACE
// Special folders: only CSIDL_APPDATA (0x1a), reported as C:\AppData, which rt_map_path sends
// to the per-user data directory. The ITEMIDLIST is a heap block holding the CSIDL.
static void imp_SHGetSpecialFolderLocation(void)
{
    uint32_t csidl = ARG(1), out = ARG(2);
    if (csidl != 0x1a || !out) RT_RETURN(0x80004005u, 12);
    uint32_t pidl = rt_heap_alloc(8, 1);
    WR32(pidl, csidl);
    WR32(out, pidl);
    RT_RETURN(0, 12);
}
static void imp_SHGetPathFromIDListA(void)
{
    uint32_t pidl = ARG(0), out = ARG(1);
    if (!pidl || RD32(pidl) != 0x1a || !out) RT_RETURN(0, 8);
    const char *path = "C:\\AppData";
    for (int i = 0; i == 0 || path[i - 1]; i++) WR8(out + i, (uint8_t)path[i]);
    RT_RETURN(1, 8);
}
// IMalloc: QueryInterface, AddRef, Release, Alloc, Realloc, Free, GetSize, DidAlloc, HeapMinimize
static void malloc_QueryInterface(void) { RT_RETURN(0x80004002u, 12); }
static void malloc_AddRef(void) { RT_RETURN(1, 4); }
static void malloc_Release(void) { RT_RETURN(1, 4); }
static void malloc_Alloc(void) { RT_RETURN(rt_heap_alloc(ARG(1), 0), 8); }
static void malloc_Free(void) { if (ARG(1)) rt_heap_free(ARG(1)); RT_RETURN(0, 8); }
static void imp_SHGetMalloc(void)
{
    static uint32_t obj;
    if (!obj) {
        void (*fns[6])(void) = {malloc_QueryInterface, malloc_AddRef, malloc_Release, malloc_Alloc, 0, malloc_Free};
        uint32_t vtbl = rt_heap_alloc(9 * 4, 1);
        for (int i = 0; i < 6; i++) if (fns[i]) WR32(vtbl + 4 * i, rt_host_fn(fns[i]));
        obj = rt_heap_alloc(4, 1);
        WR32(obj, vtbl);
    }
    if (!ARG(0)) RT_RETURN(0x80070057u, 4);
    WR32(ARG(0), obj);
    RT_RETURN(0, 4);
}


// ---------------------------------------------------------------------------------------------
// C runtime start-up and misc

static uint32_t g_cmdline, g_env;
static uint32_t mem_string(const char *str)
{
    uint32_t a = rt_heap_alloc((uint32_t)strlen(str) + 2, 1);
    rt_strcpy_to(a, str);
    return a;
}
static void imp_GetVersion(void) { RT_RETURN(0x0a280105u, 0); }       // 5.1 build 2600: Windows XP
static void imp_GetCommandLineA(void) { if (!g_cmdline) g_cmdline = mem_string("perfect.exe"); RT_RETURN(g_cmdline, 0); }
static void imp_GetStartupInfoA(void) { memset(MEM + ARG(0), 0, 68); WR32(ARG(0), 68); RT_RETURN(0, 4); }
static void imp_GetEnvironmentStrings(void) { if (!g_env) g_env = mem_string(""); RT_RETURN(g_env, 0); }   // empty block: "\0\0"
static void imp_GetEnvironmentStringsW(void) { RT_RETURN(0, 0); }   // fall back to the A version
static void imp_FreeEnvironmentStringsA(void) { RT_RETURN(1, 4); }
static void imp_FreeEnvironmentStringsW(void) { RT_RETURN(1, 4); }
static void imp_GetEnvironmentVariableA(void) { RT_RETURN(0, 12); }
static void imp_SetHandleCount(void) { RT_RETURN(ARG(0), 4); }
static uint32_t g_nextTls = 1;
static void imp_TlsAlloc(void) { RT_RETURN(g_nextTls < 64 ? g_nextTls++ : 0xffffffffu, 0); }
static void imp_UnhandledExceptionFilter(void) { RT_RETURN(1, 4); }
static void imp_IsValidCodePage(void) { RT_RETURN(1, 4); }
static void imp_IsValidLocale(void) { RT_RETURN(1, 8); }
static void imp_EnumSystemLocalesA(void) { RT_RETURN(1, 8); }
static void imp_lstrcmpA(void)
{
    int r = strcmp((const char *)MEM + ARG(0), (const char *)MEM + ARG(1));
    RT_RETURN(r < 0 ? -1 : r > 0, 8);
}
static void imp_GetKeyboardState(void) { memcpy(MEM + ARG(0), g_asyncKeys, 256); RT_RETURN(1, 4); }
static void imp_WaitMessage(void) { rt_leave(); usleep(1000); rt_enter(); RT_RETURN(1, 0); }

// Directory search: FindFirstFileA / FindNextFileA / FindClose over a host directory
typedef struct { DIR *dir; char pattern[256]; char path[1024]; } Find;
static int match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') { pat++; if (!*pat) return 1; for (; *s; s++) if (match(pat, s)) return 1; return 0; }
        if (!*s) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}
static int find_next(Find *f, uint32_t data)
{
    struct dirent *de;
    while ((de = readdir(f->dir))) {
        if (!match(f->pattern, de->d_name)) continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", f->path, de->d_name);
        struct stat st;
        stat(full, &st);
        memset(MEM + data, 0, 320);
        WR32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x80);       // FILE_ATTRIBUTE_DIRECTORY / NORMAL
        WR32(data + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        WR32(data + 32, (uint32_t)st.st_size);
        rt_strcpy_to(data + 44, de->d_name);                  // cFileName
        return 1;
    }
    return 0;
}
static void imp_FindFirstFileA(void)
{
    char win[512], host[1200];
    rt_strget(ARG(0), win, sizeof(win));
    rt_map_path(win, host, sizeof(host));
    Find *f = calloc(1, sizeof(Find));
    char *slash = strrchr(host, '/');
    snprintf(f->pattern, sizeof(f->pattern), "%s", slash ? slash + 1 : host);
    if (slash) *slash = 0;
    snprintf(f->path, sizeof(f->path), "%s", host);
    f->dir = opendir(f->path);
    if (!f->dir || !find_next(f, ARG(1))) { if (f->dir) closedir(f->dir); free(f); set_error(2); RT_RETURN(INVALID_HANDLE, 8); }
    RT_RETURN(rt_handle_new(H_FIND, f), 8);
}
static void imp_FindNextFileA(void) { Find *f = rt_handle_get(ARG(0), H_FIND); RT_RETURN(f && find_next(f, ARG(1)), 8); }
static void imp_FindClose(void) { Find *f = rt_handle_get(ARG(0), H_FIND); if (f) { closedir(f->dir); free(f); rt_handle_close(ARG(0)); } RT_RETURN(1, 4); }
static void imp_FileTimeToLocalFileTime(void) { WR64(ARG(1), RD64(ARG(0))); RT_RETURN(1, 8); }
static void imp_FileTimeToSystemTime(void) { memset(MEM + ARG(1), 0, 16); WR16(ARG(1), 2004); WR16(ARG(1) + 2, 1); WR16(ARG(1) + 6, 1); RT_RETURN(1, 8); }

// File mapping: read the whole file into heap memory (read-only use).
typedef struct { FILE *f; } Mapping;
static void imp_CreateFileMappingA(void) { Mapping *m = calloc(1, sizeof(Mapping)); m->f = file_of(ARG(0)); RT_RETURN(m->f ? rt_handle_new(H_MAPPING, m) : 0, 24); }
static void imp_MapViewOfFile(void)
{
    Mapping *m = rt_handle_get(ARG(0), H_MAPPING);
    if (!m) RT_RETURN(0, 20);
    off_t cur = ftello(m->f);
    fseeko(m->f, 0, SEEK_END);
    uint32_t size = (uint32_t)ftello(m->f);
    uint32_t a = rt_heap_alloc(size ? size : 1, 0);
    fseeko(m->f, 0, SEEK_SET);
    rt_guest_commit(a, size);
    size_t got = fread(MEM + a, 1, size, m->f);
    (void)got;
    fseeko(m->f, cur, SEEK_SET);
    RT_RETURN(a, 20);
}
static void imp_UnmapViewOfFile(void) { rt_heap_free(ARG(0)); RT_RETURN(1, 4); }

// ---------------------------------------------------------------------------------------------

typedef struct { const char *name; RecompFn fn; } Imp;
#define I(dll, n) {dll "!" #n, imp_##n}
static const Imp g_imps[] = {
    I("KERNEL32.DLL", CreateEventA), I("KERNEL32.DLL", SetEvent), I("KERNEL32.DLL", ResetEvent),
    I("KERNEL32.DLL", CreateSemaphoreA), I("KERNEL32.DLL", ReleaseSemaphore), I("KERNEL32.DLL", WaitForSingleObject),
    I("KERNEL32.DLL", WaitForMultipleObjects), I("KERNEL32.DLL", InitializeCriticalSection),
    I("KERNEL32.DLL", DeleteCriticalSection), I("KERNEL32.DLL", EnterCriticalSection), I("KERNEL32.DLL", LeaveCriticalSection),
    I("KERNEL32.DLL", InterlockedIncrement), I("KERNEL32.DLL", InterlockedDecrement), I("KERNEL32.DLL", CloseHandle),
    I("KERNEL32.DLL", CreateThread), I("KERNEL32.DLL", ResumeThread), I("KERNEL32.DLL", ExitThread),
    I("KERNEL32.DLL", GetCurrentThread), I("KERNEL32.DLL", GetCurrentThreadId), I("KERNEL32.DLL", GetCurrentProcess),
    I("KERNEL32.DLL", SetThreadPriority), I("KERNEL32.DLL", GetThreadPriority), I("KERNEL32.DLL", Sleep),
    I("KERNEL32.DLL", TlsGetValue), I("KERNEL32.DLL", TlsSetValue), I("KERNEL32.DLL", GetLastError),
    I("KERNEL32.DLL", SetLastError), I("KERNEL32.DLL", ExitProcess), I("KERNEL32.DLL", TerminateProcess),
    I("KERNEL32.DLL", HeapAlloc), I("KERNEL32.DLL", HeapFree), I("KERNEL32.DLL", HeapReAlloc), I("KERNEL32.DLL", HeapSize),
    I("KERNEL32.DLL", HeapCreate), I("KERNEL32.DLL", HeapDestroy), I("KERNEL32.DLL", GetProcessHeap),
    I("KERNEL32.DLL", VirtualAlloc), I("KERNEL32.DLL", VirtualFree), I("KERNEL32.DLL", IsBadReadPtr),
    I("KERNEL32.DLL", IsBadWritePtr), I("KERNEL32.DLL", IsBadCodePtr), I("KERNEL32.DLL", CreateFileA),
    I("KERNEL32.DLL", ReadFile), I("KERNEL32.DLL", WriteFile), I("KERNEL32.DLL", SetFilePointer),
    I("KERNEL32.DLL", GetFileSize), I("KERNEL32.DLL", SetEndOfFile), I("KERNEL32.DLL", FlushFileBuffers),
    I("KERNEL32.DLL", GetFileType), I("KERNEL32.DLL", GetStdHandle), I("KERNEL32.DLL", SetStdHandle),
    I("KERNEL32.DLL", CreateDirectoryA), I("KERNEL32.DLL", GetModuleHandleA), I("KERNEL32.DLL", GetModuleFileNameA),
    I("KERNEL32.DLL", LoadLibraryA), I("KERNEL32.DLL", FreeLibrary), I("KERNEL32.DLL", GetProcAddress),
    I("KERNEL32.DLL", GetVersionExA), I("KERNEL32.DLL", IsProcessorFeaturePresent),
    I("KERNEL32.DLL", SetUnhandledExceptionFilter), I("KERNEL32.DLL", RaiseException), I("KERNEL32.DLL", RtlUnwind),
    I("KERNEL32.DLL", GetTickCount), I("KERNEL32.DLL", QueryPerformanceFrequency), I("KERNEL32.DLL", QueryPerformanceCounter),
    I("KERNEL32.DLL", GetLocalTime), I("KERNEL32.DLL", lstrlenA), I("KERNEL32.DLL", lstrcpyA), I("KERNEL32.DLL", lstrcatA),
    I("KERNEL32.DLL", lstrcpynA), I("KERNEL32.DLL", MulDiv), I("KERNEL32.DLL", GetACP), I("KERNEL32.DLL", GetOEMCP),
    I("KERNEL32.DLL", GetCPInfo), I("KERNEL32.DLL", GetUserDefaultLCID), I("KERNEL32.DLL", MultiByteToWideChar),
    I("KERNEL32.DLL", WideCharToMultiByte), I("KERNEL32.DLL", GetStringTypeA), I("KERNEL32.DLL", GetStringTypeW),
    I("KERNEL32.DLL", LCMapStringA), I("KERNEL32.DLL", LCMapStringW), I("KERNEL32.DLL", GetLocaleInfoA),
    I("KERNEL32.DLL", GetLocaleInfoW), I("KERNEL32.DLL", FormatMessageA),
    I("WINMM.DLL", timeGetTime), I("WINMM.DLL", timeBeginPeriod), I("WINMM.DLL", timeEndPeriod),
    I("WINMM.DLL", timeSetEvent), I("WINMM.DLL", timeKillEvent),
    I("USER32.DLL", GetMessageA), I("USER32.DLL", PeekMessageA), I("USER32.DLL", PostThreadMessageA),
    I("USER32.DLL", GetQueueStatus), I("USER32.DLL", MsgWaitForMultipleObjects), I("USER32.DLL", TranslateMessage),
    I("USER32.DLL", DispatchMessageA), I("USER32.DLL", RegisterWindowMessageA), I("USER32.DLL", SystemParametersInfoA),
    I("USER32.DLL", SetCursor), I("USER32.DLL", PostMessageA), I("USER32.DLL", MessageBeep), I("USER32.DLL", MessageBoxA),
    I("USER32.DLL", GetAsyncKeyState),
    I("ADVAPI32.DLL", RegOpenKeyA), I("ADVAPI32.DLL", RegOpenKeyExA), I("ADVAPI32.DLL", RegQueryValueExA),
    I("ADVAPI32.DLL", RegCloseKey),
    I("OLE32.DLL", CoInitialize), I("OLE32.DLL", CoUninitialize), I("OLE32.DLL", CoFreeUnusedLibraries),
    I("OLE32.DLL", CoCreateInstance), I("OLE32.DLL", CoTaskMemAlloc), I("OLE32.DLL", CoTaskMemFree),
    I("SHELL32.DLL", SHGetSpecialFolderLocation), I("SHELL32.DLL", SHGetPathFromIDListA), I("SHELL32.DLL", SHGetMalloc),
    I("KERNEL32.DLL", GetVersion), I("KERNEL32.DLL", GetCommandLineA), I("KERNEL32.DLL", GetStartupInfoA),
    I("KERNEL32.DLL", GetEnvironmentStrings), I("KERNEL32.DLL", GetEnvironmentStringsW),
    I("KERNEL32.DLL", FreeEnvironmentStringsA), I("KERNEL32.DLL", FreeEnvironmentStringsW),
    I("KERNEL32.DLL", GetEnvironmentVariableA), I("KERNEL32.DLL", SetHandleCount), I("KERNEL32.DLL", TlsAlloc),
    I("KERNEL32.DLL", UnhandledExceptionFilter), I("KERNEL32.DLL", IsValidCodePage), I("KERNEL32.DLL", IsValidLocale),
    I("KERNEL32.DLL", EnumSystemLocalesA), I("KERNEL32.DLL", lstrcmpA), I("KERNEL32.DLL", FindFirstFileA),
    I("KERNEL32.DLL", FindNextFileA), I("KERNEL32.DLL", FindClose), I("KERNEL32.DLL", FileTimeToLocalFileTime),
    I("KERNEL32.DLL", FileTimeToSystemTime), I("KERNEL32.DLL", CreateFileMappingA), I("KERNEL32.DLL", MapViewOfFile),
    I("KERNEL32.DLL", UnmapViewOfFile), I("USER32.DLL", GetKeyboardState), I("USER32.DLL", WaitMessage),
};

void rt_win32_init(void) { g_start_us = now_us(); }

RecompFn rt_import(const char *name)
{
    for (size_t i = 0; i < sizeof(g_imps) / sizeof(g_imps[0]); i++)
        if (!strcasecmp(g_imps[i].name, name)) return g_imps[i].fn;
    return rt_import_sound(name);
}

// For native code (message loop): signal an event handle.
void rt_event_set(uint32_t h)
{
    Event *e = rt_handle_get(h, H_EVENT);
    if (e) { e->signaled = 1; rt_signal_obj(e); }
}
