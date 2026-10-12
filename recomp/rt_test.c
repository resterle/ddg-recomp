#include "host/guestmem.h"
// Test runtime: the recompiled functions run on a 4 GB reservation; verify.py drives it.
#include "recomp_rt.h"
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

CPU R;
uint8_t *MEM;
uint8_t RT_DIRTY[1u << 20];

// Pages written since the last call (page numbers), then cleared.
int rt_dirty(uint32_t *out, int max)
{
    int n = 0;
    for (uint32_t p = 0; p < (1u << 20); p++)
        if (RT_DIRTY[p]) { if (n < max) out[n] = p; n++; RT_DIRTY[p] = 0; }
    return n;
}
static sigjmp_buf g_trap;
static char g_err[160];

typedef struct { uint32_t addr; RecompFn fn; } Entry;
extern const Entry RT_TABLE[];
extern const int RT_TABLE_N;

uint8_t *rt_init(void)
{
    if (!MEM) {
        MEM = rt_map_guest_memory(0);       // with the alias below it, like the game runtime
    }
    return MEM;
}

CPU *rt_cpu(void) { return &R; }
int rt_after_fork(void) { return MEM ? rt_guest_realias(MEM) : 0; }
const char *rt_error(void) { return g_err; }

static RecompFn lookup(uint32_t a)
{
    int lo = 0, hi = RT_TABLE_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (RT_TABLE[mid].addr == a) return RT_TABLE[mid].fn;
        if (RT_TABLE[mid].addr < a) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void RT_Fatal(const char *what, uint32_t addr)
{
    snprintf(g_err, sizeof(g_err), "%s @%08x", what, addr);
    siglongjmp(g_trap, 1);
}

void RT_CallAddr(uint32_t a)
{
    RecompFn f = lookup(a);
    if (!f) RT_Fatal("call to unknown address", a);
    f();
}

static void on_alarm(int sig)
{
    (void)sig;
    snprintf(g_err, sizeof(g_err), "timeout (endless loop?)");
    siglongjmp(g_trap, 1);
}

// Run one function; returns 0 on success, 1 if the runtime trapped (see rt_error).
int rt_run(uint32_t addr)
{
    g_err[0] = 0;
    signal(SIGALRM, on_alarm);
    struct itimerval t = {{0, 0}, {1, 0}}, off = {{0, 0}, {0, 0}};
    if (sigsetjmp(g_trap, 1)) { setitimer(ITIMER_REAL, &off, NULL); return 1; }
    setitimer(ITIMER_REAL, &t, NULL);
    RT_CallAddr(addr);
    setitimer(ITIMER_REAL, &off, NULL);
    return 0;
}

// Translator intrinsics for the VU0 helpers (see host/d3dx.cpp, x86lift.INTRINSICS)
static double rt_f_(uint32_t a) { return RDF32(a); }
void rt_vu0_apply(uint32_t out, uint32_t m, uint32_t v)
{
    if (RD32(0x5e321c) != 0x490c09) RT_Fatal("Vu0ApplyMatrix: unexpected D3DX slot", RD32(0x5e321c));
    double x = rt_f_(v), y = rt_f_(v + 4), z = rt_f_(v + 8), r[4];
    for (int i = 0; i < 4; i++)
        r[i] = ((x * rt_f_(m + 4 * i) + y * rt_f_(m + 16 + 4 * i)) + z * rt_f_(m + 32 + 4 * i)) + rt_f_(m + 48 + 4 * i);
    for (int i = 0; i < 4; i++) WRF32(out + 4 * i, r[i]);
}
void rt_vu0_mul(uint32_t out, uint32_t a, uint32_t b)
{
    if (RD32(0x5e3224) != 0x490e56) RT_Fatal("Vu0MulMatrix: unexpected D3DX slot", RD32(0x5e3224));
    float A[16], B[16], r[16];
    for (int i = 0; i < 16; i++) { A[i] = RDF32(b + 4 * i); B[i] = RDF32(a + 4 * i); }   // out = b * a
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = (double)A[i * 4 + 3] * B[12 + j] + (double)A[i * 4 + 1] * B[4 + j];
            s += (double)A[i * 4] * B[j];
            s += (double)A[i * 4 + 2] * B[8 + j];
            r[i * 4 + j] = (float)s;
        }
    for (int i = 0; i < 16; i++) WRF32(out + 4 * i, r[i]);
}
