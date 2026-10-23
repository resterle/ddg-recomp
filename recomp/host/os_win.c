// os_win.c - what the runtime needs from Windows: the POSIX subset of host/win/include (threads,
// sleeping, clocks, directory listing), the emulated memory, fibers, the crash report.
// The only host file that includes <windows.h> (its names collide with the Win32 shim's).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <process.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win/include/pthread.h"
#include "win/include/dirent.h"
#include "win/include/sched.h"
#include "win/include/unistd.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

// ---------------------------------------------------------------------------------------------
// Threads, locks, condition variables

int pthread_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive((PSRWLOCK)m); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); return 0; }
int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    struct timespec now;
    rt_clock_gettime(CLOCK_REALTIME, &now);
    int64_t ns = (int64_t)(abstime->tv_sec - now.tv_sec) * 1000000000 + (abstime->tv_nsec - now.tv_nsec);
    DWORD ms = ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000);
    if (SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}

typedef struct { void *(*fn)(void *); void *arg; } ThreadStart;
static unsigned __stdcall thread_start(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof(ThreadStart));
    s->fn = fn;
    s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, a ? (unsigned)a->stack : 0, thread_start, s,
                                 a && a->stack ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, NULL);
    if (!h) { free(s); return EAGAIN; }
    *t = (pthread_t)h;
    return 0;
}

int sched_yield(void) { SwitchToThread(); return 0; }

// Sleeps with a high-resolution waitable timer (Windows 10 1803+; Sleep() rounds up to the
// timer tick otherwise), one per thread.
int usleep(unsigned us)
{
    static _Thread_local HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)us * 10;              // relative, in 100 ns
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep((us + 999) / 1000); return 0; }
    WaitForSingleObject(timer, INFINITE);
    return 0;
}

int rt_clock_gettime(int clock, struct timespec *ts)
{
    if (clock == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        ts->tv_sec = (time_t)(c.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000 / freq.QuadPart);
    } else {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull;
        ts->tv_sec = (time_t)(t / 10000000);
        ts->tv_nsec = (long)(t % 10000000) * 100;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Directories

struct RtDir { HANDLE h; WIN32_FIND_DATAA fd; int pending; struct dirent e; };

DIR *opendir(const char *path)
{
    char pat[MAX_PATH + 4];
    snprintf(pat, sizeof(pat), "%s\\*", path);
    DIR *d = calloc(1, sizeof(DIR));
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); errno = ENOENT; return NULL; }
    d->pending = 1;
    return d;
}

struct dirent *readdir(DIR *d)
{
    if (!d->pending && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->pending = 0;
    snprintf(d->e.d_name, sizeof(d->e.d_name), "%s", d->fd.cFileName);
    return &d->e;
}

int closedir(DIR *d)
{
    FindClose(d->h);
    free(d);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Emulated memory (host/guestmem.h): one pagefile-backed section of 4 GB, viewed at `want` and
// again directly below it. The section is only reserved; pages are committed in 64 KB pieces
// when first touched (the exception handler below) or before the kernel writes them
// (rt_win_commit: ReadFile into an uncommitted page fails instead of faulting). Committing all
// of it would charge 4 GB against the system's commit limit.

static uintptr_t g_guestLo, g_guestHi;

static int commit(uintptr_t a, size_t n)
{
    uintptr_t lo = a & ~(uintptr_t)0xffff, hi = (a + n + 0xffff) & ~(uintptr_t)0xffff;
    return VirtualAlloc((void *)lo, hi - lo, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static LONG CALLBACK commit_on_fault(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionInformation[1];
    if (a < g_guestLo || a >= g_guestHi) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)a, &mi, sizeof(mi)) || mi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    return commit(a, 1) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void rt_win_commit(void *p, size_t n)
{
    uintptr_t a = (uintptr_t)p;
    if (n && a >= g_guestLo && a + n <= g_guestHi) commit(a, n);
}

uint8_t *rt_win_map_guest_memory(uint64_t want, uint64_t size, uint64_t alias)
{
    if (!want) {                                    // tests: any place with room for both views
        void *p = VirtualAlloc(NULL, alias + size, MEM_RESERVE, PAGE_NOACCESS);
        if (!p) return NULL;
        VirtualFree(p, 0, MEM_RELEASE);
        want = (uint64_t)(uintptr_t)p + alias;
    }
    HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE,
                                  (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!s) return NULL;
    uint8_t *base = MapViewOfFileEx(s, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size, (void *)(uintptr_t)want);
    uint8_t *lo = base ? MapViewOfFileEx(s, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)alias, base - alias) : NULL;
    CloseHandle(s);                                 // the views keep the section
    if (!base || !lo) {
        static int told;
        if (!told++) fprintf(stderr, "guest memory: mapping at %llx failed (%lu)\n", (unsigned long long)want, GetLastError());
        if (base) UnmapViewOfFile(base);
        return NULL;
    }
    g_guestLo = (uintptr_t)lo;
    g_guestHi = (uintptr_t)base + size;
    static int handler;
    if (!handler++) AddVectoredExceptionHandler(1, commit_on_fault);
    return base;
}

// ---------------------------------------------------------------------------------------------
// Fibers (runtime.c rt_fiber_*): Windows fibers, which keep the thread's stack bounds right for
// the system (stack probes, longjmp's unwinding) and save the floating-point control state.

void *rt_win_fiber_create(void (*fn)(void *), void *arg, size_t stack)
{
    return CreateFiberEx(64 * 1024, stack, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)fn, arg);
}

// `*self`: the running fiber, NULL for a thread that never switched (it becomes a fiber now)
void rt_win_fiber_switch(void **self, void *to)
{
    if (!*self) *self = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    SwitchToFiber(to);
}

void rt_win_fiber_delete(void *f) { DeleteFiber(f); }

// ---------------------------------------------------------------------------------------------
// Crash report: where the exception happened (module + offset, for llvm-symbolizer), a host
// backtrace, then `report` (the guest registers); the process ends.

static void (*g_report)(unsigned code, void *pc, void *addr);

static void print_module(const char *what, void *pc)
{
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m) && m)
        GetModuleFileNameA(m, name, sizeof(name));
    const char *base = strrchr(name, '\\');
    fprintf(stderr, "    %s %p: %s+0x%llx\n", what, pc, base ? base + 1 : name,
            (unsigned long long)((char *)pc - (char *)m));
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    void *pc = r->ExceptionAddress;
    void *addr = r->NumberParameters >= 2 ? (void *)(uintptr_t)r->ExceptionInformation[1] : NULL;
    fprintf(stderr, "\n*** crash: exception 0x%08lx at address %p, pc %p\n", r->ExceptionCode, addr, pc);
    print_module("pc", pc);
    void *bt[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, bt, NULL);
    fprintf(stderr, "    host backtrace:\n");
    for (USHORT i = 0; i < n; i++) print_module("at", bt[i]);
    fflush(stderr);
    if (g_report) g_report(r->ExceptionCode, pc, addr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

void rt_win_install_crash_handler(void (*report)(unsigned code, void *pc, void *addr))
{
    g_report = report;
    SetUnhandledExceptionFilter(crash_filter);
    ULONG guarantee = 64 * 1024;                    // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
}

// Process setup: 1 ms timer resolution (condition variable timeouts, Sleep), UTF-8 console output,
// stderr unbuffered also into a pipe or file (the C library buffers it there: lost on a crash).
void rt_win_init(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);
}

// --- RECOMP_JIT (host/jit.cpp) ---------------------------------------------------------------

// The program's own exported symbol (the runtime's interface, exported by the .def the Makefile
// writes), or NULL.
void *rt_win_exe_symbol(const char *name)
{
    return (void *)GetProcAddress(GetModuleHandleA(NULL), name);
}

// Address space for the compiled game code within 2 GB of the program: the code calls the
// runtime and reads its data with 32-bit pc-relative addresses (the small code model), and the
// unwind tables are 32-bit offsets from a base. Reserved once, committed as handed out.
static uint8_t *g_jit_base, *g_jit_next, *g_jit_end;
static SRWLOCK g_jit_lock = SRWLOCK_INIT;

static int jit_reserve(size_t size)
{
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    uintptr_t lo = (uintptr_t)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    const uintptr_t step = 64u << 20;
    // after the program first, then before it; every byte of it within 2 GB of every byte of the program
    for (int dir = 0; dir < 2; dir++)
        for (uintptr_t i = 1; i < 32; i++) {
            uintptr_t a = dir == 0 ? ((hi + step - 1) & ~(step - 1)) + (i - 1) * step : (lo & ~(step - 1)) - i * step - size;
            if (dir == 0 ? a + size - lo > (1ull << 31) - (1u << 20) : hi - a > (1ull << 31) - (1u << 20)) break;
            void *p = VirtualAlloc((void *)a, size, MEM_RESERVE, PAGE_NOACCESS);
            if (p) { g_jit_base = g_jit_next = (uint8_t *)p; g_jit_end = g_jit_base + size; return 1; }
        }
    return 0;
}

// size bytes of read/write memory (64 KB aligned) near the program, or NULL.
void *rt_win_jit_alloc(size_t size)
{
    AcquireSRWLockExclusive(&g_jit_lock);
    if (!g_jit_base && !jit_reserve(1u << 30)) jit_reserve(256u << 20);
    size = (size + 0xffff) & ~(size_t)0xffff;
    void *p = NULL;
    if (g_jit_base && size <= (size_t)(g_jit_end - g_jit_next)) {
        p = VirtualAlloc(g_jit_next, size, MEM_COMMIT, PAGE_READWRITE);
        if (p) g_jit_next += size;
    }
    ReleaseSRWLockExclusive(&g_jit_lock);
    return p;
}

// prot: 1 read, 2 write, 4 execute (llvm::sys::Memory's flags)
int rt_win_jit_protect(void *p, size_t size, unsigned prot)
{
    DWORD f = (prot & 4) ? ((prot & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                         : ((prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS);
    DWORD old;
    if (!VirtualProtect(p, size, f, &old)) return -1;
    if (prot & 4) FlushInstructionCache(GetCurrentProcess(), p, size);
    return 0;
}

// The unwind table (.pdata) of one compiled object, for exceptions and longjmp across its
// functions (runtime.c's thread exit). base: what its entries are relative to.
int rt_win_jit_unwind(void *pdata, size_t size, uint64_t base)
{
    return RtlAddFunctionTable((PRUNTIME_FUNCTION)pdata, (DWORD)(size / sizeof(RUNTIME_FUNCTION)), base) ? 0 : -1;
}
