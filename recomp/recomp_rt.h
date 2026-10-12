// recomp_rt.h - runtime for statically recompiled x86 (32-bit) code
//
// The translated code runs on a flat 32-bit address space: MEM points at a 4 GB reservation,
// and x86 address A lives at MEM + A. The original image is mapped at its own addresses, so
// pointers stored in game data keep working unchanged.
//
// Each translated function is `void F_xxxxxxxx(void)`. It loads the CPU registers into locals,
// writes them back around calls and on return, and keeps the stack in emulated memory exactly
// like the original, so stack arguments, return values in EAX/EDX/ST0 and callee-popped
// arguments all behave as on x86.

#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    double st[8];           // x87 stack (computed in double precision)
    uint32_t top;           // x87 TOP
    uint32_t sw;            // x87 status word (C0/C2/C3 from compares)
    uint32_t cw;            // x87 control word
} CPU;

extern CPU R;
#ifdef RT_MEM_BASE
// The emulated 4 GB live at a fixed address chosen at build time: every access compiles to a
// constant base plus offset (a global pointer had to be reloaded after every store).
#define MEM ((uint8_t *)(uintptr_t)(RT_MEM_BASE))
#else
extern uint8_t *MEM;
#endif

typedef void (*RecompFn)(void);

// Linear address of an access into the emulated memory. Translated code computes addresses in
// 64-bit signed arithmetic (base and index sign-extended): identical for every address below
// 2 GB, where all emulated memory lives, and it lets the compiler fold the base into ARM64/x86-64
// addressing modes. Host code passes plain uint32_t addresses.
typedef int64_t RtAddr;
// stack slot relative to the current esp (same signed scheme)
#define RT_SP(off) ((RtAddr)(int32_t)esp + (off))

// Base of the fake thread information block that fs: addresses point into (SEH frames).
#define RT_FS_BASE 0x7ffd0000u

// Registers live in locals inside a translated function; these move them to/from R.
#ifdef RT_COUNT_SAVES
extern uint64_t RT_SAVES;
#define RT_COUNT_SAVE() (RT_SAVES++)
#else
#define RT_COUNT_SAVE() ((void)0)
#endif
#define SAVE() (RT_COUNT_SAVE(), R.eax = eax, R.ecx = ecx, R.edx = edx, R.ebx = ebx, R.esp = esp, R.ebp = ebp, R.esi = esi, R.edi = edi)
#define LOAD() (eax = R.eax, ecx = R.ecx, edx = R.edx, ebx = R.ebx, esp = R.esp, ebp = R.ebp, esi = R.esi, edi = R.edi)

// Calling convention between translated functions: the eight registers are C arguments, and
// EAX/EDX/ECX/ESP come back in a 16-byte struct (returned in registers on ARM64 and x86-64).
// EBX/EBP/ESI/EDI are returned through R. Natives and the dispatch table keep the R-based
// void(void) convention; dispatch.c wraps every translated function in an adapter (A_xxx).
typedef struct { uint32_t eax, edx, ecx, esp; } RtRet;
// esp is signed: pushes/pops then never "wrap", so the compiler keeps one stack pointer register
#define RT_PARAMS uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, int32_t esp, uint32_t ebp, uint32_t esi, uint32_t edi
#define RT_ARGS eax, ecx, edx, ebx, esp, ebp, esi, edi
// return from a translated function
#define RT_RET() do { RT_COUNT_SAVE(); R.ebx = ebx; R.ebp = ebp; R.esi = esi; R.edi = edi; \
                      return (RtRet){eax, edx, ecx, esp}; } while (0)
// return after a native or indirect callee left all registers in R
#define RT_RET_FROM_R() do { return (RtRet){R.eax, R.edx, R.ecx, R.esp}; } while (0)
// call a translated function
// Variants for callee-saved registers the callee provably preserves (mask bits: ebx, ebp, esi,
// edi; x86lift.preserve_mask): those are neither written back to R nor reloaded from it.
#define RT_RETM(m) do { RT_COUNT_SAVE(); if (!((m) & 1)) R.ebx = ebx; if (!((m) & 2)) R.ebp = ebp; \
                        if (!((m) & 4)) R.esi = esi; if (!((m) & 8)) R.edi = edi; \
                        return (RtRet){eax, edx, ecx, esp}; } while (0)
#define RT_CALLM(f, m) do { RtRet r_ = f(RT_ARGS); eax = r_.eax; edx = r_.edx; ecx = r_.ecx; esp = r_.esp; \
                            if (!((m) & 1)) ebx = R.ebx; if (!((m) & 2)) ebp = R.ebp; \
                            if (!((m) & 4)) esi = R.esi; if (!((m) & 8)) edi = R.edi; } while (0)
#define RT_CALL(f) do { RtRet r_ = f(RT_ARGS); eax = r_.eax; edx = r_.edx; ecx = r_.ecx; esp = r_.esp; \
                        ebx = R.ebx; ebp = R.ebp; esi = R.esi; edi = R.edi; } while (0)

// Optional stack-balance check around direct calls (build with -DRT_CHECK_STACK): the callee
// must pop its return address plus exactly the bytes of its own "ret N".
#ifdef RT_CHECK_STACK
#define RT_ESP_MARK() uint32_t espm_ = esp
#define RT_ESP_CHECK(pop, site, callee) do { if (esp != espm_ + (pop)) RT_StackFault(site, callee, espm_ + (pop), esp); } while (0)
void RT_StackFault(uint32_t site, uint32_t callee, uint32_t expected, uint32_t actual);
#else
#define RT_ESP_MARK() ((void)0)
#define RT_ESP_CHECK(pop, site, callee) ((void)0)
#endif

// Indirect calls and jumps to other functions: looked up by original address.
void RT_CallAddr(uint32_t addr);
// Unrecoverable situations (unknown call target, division by zero, ...).
#ifdef __cplusplus
extern "C"
#endif
void RT_Fatal(const char *what, uint32_t addr);
#ifdef __cplusplus
extern "C"
#endif
int rt_skip(uint32_t addr);         // LIFT_SKIPPABLE functions: switched off now? (RECOMP_SKIP)

// Test builds record which 4 KB pages were written (verify.py compares only those).
#ifdef RT_TRACK_WRITES
extern uint8_t RT_DIRTY[1u << 20];
#define RT_MARK(a, n) (RT_DIRTY[(uint32_t)(a) >> 12] = 1, RT_DIRTY[(uint32_t)((a) + (n) - 1) >> 12] = 1)
#else
#define RT_MARK(a, n) ((void)0)
#endif

// VU0 helpers used by translator intrinsics (host/d3dx.cpp)
#ifdef __cplusplus
extern "C" {
#endif
void rt_vu0_apply(uint32_t out, uint32_t m, uint32_t v);
void rt_vu0_mul(uint32_t out, uint32_t a, uint32_t b);
#ifdef __cplusplus
}
#endif

static inline uint8_t RD8(RtAddr a) { return MEM[a]; }
static inline uint16_t RD16(RtAddr a) { uint16_t v; memcpy(&v, MEM + a, 2); return v; }
static inline uint32_t RD32(RtAddr a) { uint32_t v; memcpy(&v, MEM + a, 4); return v; }
static inline uint64_t RD64(RtAddr a) { uint64_t v; memcpy(&v, MEM + a, 8); return v; }
static inline float RDF32(RtAddr a) { float v; memcpy(&v, MEM + a, 4); return v; }
static inline double RDF64(RtAddr a) { double v; memcpy(&v, MEM + a, 8); return v; }
static inline void WR8(RtAddr a, uint32_t v) { RT_MARK(a, 1); MEM[a] = (uint8_t)v; }
static inline void WR16(RtAddr a, uint32_t v) { uint16_t t = (uint16_t)v; RT_MARK(a, 2); memcpy(MEM + a, &t, 2); }
static inline void WR32(RtAddr a, uint32_t v) { RT_MARK(a, 4); memcpy(MEM + a, &v, 4); }
static inline void WR64(RtAddr a, uint64_t v) { RT_MARK(a, 8); memcpy(MEM + a, &v, 8); }
// x87 produces the negative "indefinite" NaN for invalid operations (0*inf, inf-inf, ...);
// ARM produces a positive default NaN. Stores turn the latter into the former.
static inline void WRF32(RtAddr a, double v)
{
    float t = (float)v;
    uint32_t b;
    memcpy(&b, &t, 4);
    if (b == 0x7fc00000u) b = 0xffc00000u;
    RT_MARK(a, 4);
    memcpy(MEM + a, &b, 4);
}
static inline void WRF64(RtAddr a, double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    if (b == 0x7ff8000000000000ull) b = 0xfff8000000000000ull;
    RT_MARK(a, 8);
    memcpy(MEM + a, &b, 8);
}

// 80-bit extended values are kept as doubles.
static inline double RDF80(RtAddr a)
{
    uint64_t mant; uint16_t se;
    memcpy(&mant, MEM + a, 8); memcpy(&se, MEM + a + 8, 2);
    int exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
static inline void WRF80(RtAddr a, double v)
{
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e; double f = frexp(fabs(v), &e);          // f in [0.5, 1)
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(MEM + a, &mant, 8); memcpy(MEM + a + 8, &se, 2);
#ifdef RT_TRACK_WRITES
    RT_MARK(a, 10);
#endif
}

// PF: set when the low byte has an even number of 1 bits.
static inline uint32_t PARITY(uint32_t v) { return !__builtin_parity(v & 0xff); }

// x87 stack
#define ST(i) R.st[(R.top + (i)) & 7]
static inline void FPUSH(double v) { R.top = (R.top - 1) & 7; R.st[R.top] = v; }
static inline void FPOP(void) { R.top = (R.top + 1) & 7; }
// FCOM: C3 C2 C0 = 000 (>), 001 (<), 100 (=), 111 (unordered)
static inline void FCOM(double a, double b)
{
    uint32_t c = (a > b) ? 0 : (a < b) ? 0x0100 : (a == b) ? 0x4000 : 0x4500;
    R.sw = (R.sw & ~0x4500u) | c;
}
static inline uint32_t FNSTSW(void) { return (R.sw & ~0x3800u) | ((R.top & 7) << 11); }

// Integer conversion with the current x87 rounding mode (control word bits 10-11).
static inline double FROUND(double v)
{
    switch ((R.cw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

// String instructions (direction flag assumed clear, as MSVC code guarantees)
static inline void REP_MOVSD(uint32_t *esi, uint32_t *edi, uint32_t *ecx)
{
    for (; *ecx; (*ecx)--, *esi += 4, *edi += 4) WR32(*edi, RD32(*esi));
}
static inline void REP_MOVSB(uint32_t *esi, uint32_t *edi, uint32_t *ecx)
{
    for (; *ecx; (*ecx)--, (*esi)++, (*edi)++) WR8(*edi, RD8(*esi));
}
static inline void REP_STOSD(uint32_t *edi, uint32_t *ecx, uint32_t eax)
{
    for (; *ecx; (*ecx)--, *edi += 4) WR32(*edi, eax);
}
static inline void REP_STOSB(uint32_t *edi, uint32_t *ecx, uint32_t eax)
{
    for (; *ecx; (*ecx)--, (*edi)++) WR8(*edi, eax);
}
