// d3dx.cpp - native versions of the D3DX 8 x87 math routines
//
// D3DX picks CPU-specific implementations at startup and stores them in a table (0x490fc7 fills
// it with the x87 set, since the runtime reports a CPU without SSE). The game calls these all the
// time through its VU0 math wrappers (0x4155c0-0x415850); translated, every x87 instruction goes
// through the emulated FPU stack, which made the matrix multiply the single most expensive
// function of the game. These natives compute in double precision with the same operation order,
// like the translated x87 code, and round to float on store.
//
// All are stdcall and return the output pointer in EAX.

#include "runtime.h"
#include <cmath>
#include <cstring>

#define NATIVE extern "C" void

static inline double F(uint32_t a) { return (double)RDF32(a); }

// 0x490b6b D3DXVec2Transform(out vec4, v vec2, m): out[i] = x*m[i] + y*m[4+i] + m[12+i]
NATIVE N_00490b6b(void)
{
    uint32_t out = ARG(0), v = ARG(1), m = ARG(2);
    double x = F(v), y = F(v + 4), r[4];
    for (int i = 0; i < 4; i++) r[i] = (x * F(m + 4 * i) + y * F(m + 16 + 4 * i)) + F(m + 48 + 4 * i);
    for (int i = 0; i < 4; i++) WRF32(out + 4 * i, r[i]);
    RT_RETURN(out, 12);
}

// 0x490bd4 D3DXVec2TransformNormal(out vec2, v vec2, m): out[i] = x*m[i] + y*m[4+i]
NATIVE N_00490bd4(void)
{
    uint32_t out = ARG(0), v = ARG(1), m = ARG(2);
    double x = F(v), y = F(v + 4), r[2];
    for (int i = 0; i < 2; i++) r[i] = x * F(m + 4 * i) + y * F(m + 16 + 4 * i);
    for (int i = 0; i < 2; i++) WRF32(out + 4 * i, r[i]);
    RT_RETURN(out, 12);
}

// 0x490c09 D3DXVec3Transform(out vec4, v vec3, m): out[i] = x*m[i] + y*m[4+i] + z*m[8+i] + m[12+i]
NATIVE N_00490c09(void)
{
    uint32_t out = ARG(0), v = ARG(1), m = ARG(2);
    double x = F(v), y = F(v + 4), z = F(v + 8), r[4];
    for (int i = 0; i < 4; i++)
        r[i] = ((x * F(m + 4 * i) + y * F(m + 16 + 4 * i)) + z * F(m + 32 + 4 * i)) + F(m + 48 + 4 * i);
    for (int i = 0; i < 4; i++) WRF32(out + 4 * i, r[i]);
    RT_RETURN(out, 12);
}

// 0x490c94 D3DXVec3TransformNormal(out vec3, v vec3, m): out[i] = x*m[i] + y*m[4+i] + z*m[8+i]
NATIVE N_00490c94(void)
{
    uint32_t out = ARG(0), v = ARG(1), m = ARG(2);
    double x = F(v), y = F(v + 4), z = F(v + 8), r[3];
    for (int i = 0; i < 3; i++) r[i] = (x * F(m + 4 * i) + y * F(m + 16 + 4 * i)) + z * F(m + 32 + 4 * i);
    for (int i = 0; i < 3; i++) WRF32(out + 4 * i, r[i]);
    RT_RETURN(out, 12);
}

// 0x490cf3 D3DXVec4Transform(out vec4, v vec4, m)
NATIVE N_00490cf3(void)
{
    uint32_t out = ARG(0), v = ARG(1), m = ARG(2);
    double x = F(v), y = F(v + 4), z = F(v + 8), w = F(v + 12), r[4];
    for (int i = 0; i < 4; i++)
        r[i] = ((x * F(m + 4 * i) + y * F(m + 16 + 4 * i)) + z * F(m + 32 + 4 * i)) + w * F(m + 48 + 4 * i);
    for (int i = 0; i < 4; i++) WRF32(out + 4 * i, r[i]);
    RT_RETURN(out, 12);
}

// 0x490d8c D3DXVec3Normalize(out, v): reciprocal square root from a table (0x5e4f00: pairs of
// slope/offset indexed by the top mantissa bits) times an exponent estimate, as the original.
NATIVE N_00490d8c(void)
{
    uint32_t out = ARG(0), v = ARG(1);
    float x = RDF32(v), y = RDF32(v + 4), z = RDF32(v + 8);
    double len2 = (double)x * x + (double)y * y + (double)z * z;
    float l = (float)len2;
    if (l == 0.0f) {
        WR32(out, 0); WR32(out + 4, 0); WR32(out + 8, 0);
    } else {
        float d = (float)(len2 - 1.0);
        uint32_t db, lb;
        memcpy(&db, &d, 4);
        memcpy(&lb, &l, 4);
        if ((db & 0x7fffffffu) < 0x3727c5adu) {                 // already unit length
            if (out != v) { WRF32(out, x); WRF32(out + 4, y); WRF32(out + 8, z); }
        } else {
            uint32_t idx = (lb >> 12) & 0xff8, mb = (lb & 0xffffff) | 0x3f000000u,
                     eb = ((0xbeffffffu - lb) >> 1) & 0xff800000u;
            float mant, expo;
            memcpy(&mant, &mb, 4);
            memcpy(&expo, &eb, 4);
            float r = (float)((double)mant * RDF32(0x5e4f00 + idx) + RDF32(0x5e4f04 + idx)) * expo;
            WRF32(out, (double)x * r); WRF32(out + 4, (double)y * r); WRF32(out + 8, (double)z * r);
        }
    }
    RT_RETURN(out, 8);
}

// out[i][j] = a[i][3]*b[3][j] + a[i][1]*b[1][j] + a[i][0]*b[0][j] + a[i][2]*b[2][j] (x87 order)
static void Multiply(float r[16], uint32_t a, uint32_t b)
{
    float A[16], B[16];
    memcpy(A, MEM + a, 64);
    memcpy(B, MEM + b, 64);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = (double)A[i * 4 + 3] * B[12 + j] + (double)A[i * 4 + 1] * B[4 + j];
            s += (double)A[i * 4] * B[j];
            s += (double)A[i * 4 + 2] * B[8 + j];
            r[i * 4 + j] = (float)s;
        }
}

// 0x490e56 D3DXMatrixMultiply(out, a, b): out = a * b (out may alias a or b)
NATIVE N_00490e56(void)
{
    uint32_t out = ARG(0), a = ARG(1), b = ARG(2);
    float r[16];
    Multiply(r, a, b);
    for (int i = 0; i < 16; i++) WRF32(out + 4 * i, r[i]);
    RT_RETURN(out, 12);
}

// 0x490f39 D3DXMatrixMultiplyTranspose(out, a, b): out = transpose(a * b)
NATIVE N_00490f39(void)
{
    uint32_t out = ARG(0), a = ARG(1), b = ARG(2);
    float r[16];
    Multiply(r, a, b);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) WRF32(out + 4 * (j * 4 + i), r[i * 4 + j]);
    RT_RETURN(out, 12);
}

// sin and cos of a float angle in double precision, as floats. One sincos call (the same
// kernels as sin and cos in glibc: identical results, checked for 10^8 angles on x86-64; libm
// was ~14% of the thread that builds the train's matrices).
static void SinCos(float angle, float *s, float *c)
{
    double sd, cd;
#ifdef __APPLE__
    __sincos((double)angle, &sd, &cd);
#else
    sincos((double)angle, &sd, &cd);
#endif
    *s = (float)sd;
    *c = (float)cd;
}

// 0x48a909 D3DXMatrixRotationX(out, angle), 0x48a9a5 ...RotationY, 0x48aa42 ...RotationZ.
// fsincos of the float angle in extended precision, stored as floats.
static void Rotation(uint32_t out, int axis, float angle)
{
    float c, s;
    SinCos(angle, &s, &c);
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (axis == 0) { m[5] = c; m[6] = s; m[9] = -s; m[10] = c; }
    else if (axis == 1) { m[0] = c; m[2] = -s; m[8] = s; m[10] = c; }
    else { m[0] = c; m[1] = s; m[4] = -s; m[5] = c; }
    memcpy(MEM + out, m, 64);
}
NATIVE N_0048a909(void) { Rotation(ARG(0), 0, RDF32(R.esp + 8)); RT_RETURN(ARG(0), 8); }
NATIVE N_0048a9a5(void) { Rotation(ARG(0), 1, RDF32(R.esp + 8)); RT_RETURN(ARG(0), 8); }
NATIVE N_0048aa42(void) { Rotation(ARG(0), 2, RDF32(R.esp + 8)); RT_RETURN(ARG(0), 8); }

// ---------------------------------------------------------------------------------------------
// MSVC CRT helpers called from compiled code all over the game.

// 0x4a3340 __ftol: ST(0) truncated to a 64-bit integer in EDX:EAX, ST(0) popped. Out of range
// or NaN gives the x87 integer indefinite (0x8000000000000000).
NATIVE N_004a3340(void)
{
    double v = ST(0);
    FPOP();
    int64_t r = (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
    R.edx = (uint32_t)((uint64_t)r >> 32);
    RT_RETURN((uint32_t)r, 0);
}

// 0x4a3850 __allshl: EDX:EAX << CL (0 for shifts of 64 and more)
NATIVE N_004a3850(void)
{
    uint64_t v = (uint64_t)R.edx << 32 | R.eax;
    uint32_t n = R.ecx & 0xff;
    v = n >= 64 ? 0 : v << n;
    R.edx = (uint32_t)(v >> 32);
    RT_RETURN((uint32_t)v, 0);
}

// ---------------------------------------------------------------------------------------------
// The game's VU0 math wrappers (PS2 SDK replacements, 0x415700-0x4158c0). They call D3DX through
// thunks that jump via a slot table (0x5e3218, filled at start-up with the CPU-specific
// routines). Each wrapper layer cost a full register save/restore and an indirect dispatch;
// these natives compute directly when the slot holds the routine above, and otherwise call
// whatever the slot holds.

static void Vec3Transform(uint32_t out, uint32_t v, uint32_t m)
{
    double x = F(v), y = F(v + 4), z = F(v + 8), r[4];
    for (int i = 0; i < 4; i++)
        r[i] = ((x * F(m + 4 * i) + y * F(m + 16 + 4 * i)) + z * F(m + 32 + 4 * i)) + F(m + 48 + 4 * i);
    for (int i = 0; i < 4; i++) WRF32(out + 4 * i, r[i]);
}

static void MatrixMultiply(uint32_t out, uint32_t a, uint32_t b)
{
    float r[16];
    Multiply(r, a, b);
    for (int i = 0; i < 16; i++) WRF32(out + 4 * i, r[i]);
}

// Calls slot `slot` with three stdcall arguments; fast path when it holds `expected`.
static void CallSlot3(uint32_t slot, uint32_t expected, void (*fast)(uint32_t, uint32_t, uint32_t),
                      uint32_t a0, uint32_t a1, uint32_t a2)
{
    uint32_t fn = RD32(slot);
    if (fn == expected) { fast(a0, a1, a2); return; }
    uint32_t args[3] = {a0, a1, a2};
    rt_call(fn, 3, args);
}

// Intrinsic versions of 0x415740 / 0x415760 (x86lift.INTRINSICS): the D3DX slot is expected to
// hold the function the fast path implements; anything else would need the native boundary.
extern "C" void rt_vu0_apply(uint32_t out, uint32_t m, uint32_t v)
{
    if (RD32(0x5e321c) != 0x490c09) RT_Fatal("Vu0ApplyMatrix: unexpected D3DX slot", RD32(0x5e321c));
    Vec3Transform(out, v, m);
}

extern "C" void rt_vu0_mul(uint32_t out, uint32_t a, uint32_t b)
{
    if (RD32(0x5e3224) != 0x490e56) RT_Fatal("Vu0MulMatrix: unexpected D3DX slot", RD32(0x5e3224));
    MatrixMultiply(out, b, a);
}

// 0x415700 copy a 4-float vector (dst, src)
NATIVE N_00415700(void)
{
    uint32_t dst = ARG(0), src = ARG(1);
    memmove(MEM + dst, MEM + src, 16);
    R.ecx = dst;
    R.edx = RD32(src + 8);
    RT_RETURN(RD32(src + 12), 0);
}

// 0x415810 copy a 4x4 matrix (dst, src), row by row like the original
NATIVE N_00415810(void)
{
    uint32_t dst = ARG(0), src = ARG(1);
    for (int i = 0; i < 4; i++) memmove(MEM + dst + 16 * i, MEM + src + 16 * i, 16);
    R.ecx = dst + 0x30;
    R.edx = RD32(src + 0x38);
    RT_RETURN(RD32(src + 0x3c), 0);
}

// 0x415740 (out, m, v): D3DXVec3Transform(out, v, m) through slot 1
NATIVE N_00415740(void)
{
    uint32_t out = ARG(0), m = ARG(1), v = ARG(2);
    CallSlot3(0x5e321c, 0x490c09, Vec3Transform, out, v, m);
    RT_RETURN(out, 0);
}

// 0x415760 (out, a, b): D3DXMatrixMultiply(out, b, a) through slot 3
NATIVE N_00415760(void)
{
    uint32_t out = ARG(0), a = ARG(1), b = ARG(2);
    CallSlot3(0x5e3224, 0x490e56, MatrixMultiply, out, b, a);
    RT_RETURN(out, 0);
}

// 0x4158c0 (out, m, angle): out = m * RotationY(angle), via slots 31 and 3
NATIVE N_004158c0(void)
{
    uint32_t out = ARG(0), m = ARG(1), angleBits = ARG(2);
    if (RD32(0x5e3294) != 0x48a9a5 || RD32(0x5e3224) != 0x490e56) {
        // unexpected table: run the original through the translated code's path
        uint32_t tmp = R.esp - 0x80;                // scratch below the caller's frame
        uint32_t args2[2] = {tmp, angleBits};
        rt_call(RD32(0x5e3294), 2, args2);
        uint32_t args3[3] = {out, m, tmp};
        rt_call(RD32(0x5e3224), 3, args3);
        RT_RETURN(out, 0);
    }
    float angle;
    memcpy(&angle, &angleBits, 4);
    float rot[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float c, s;
    SinCos(angle, &s, &c);
    rot[0] = c; rot[2] = -s; rot[8] = s; rot[10] = c;
    uint32_t tmp = R.esp - 0x80;
    memcpy(MEM + tmp, rot, 64);
    MatrixMultiply(out, m, tmp);
    RT_RETURN(out, 0);
}
