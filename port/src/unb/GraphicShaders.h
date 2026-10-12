// GraphicShaders.h - Metal Shading Language sources for CGraphic.
//
// SDL3 GPU compiles MSL source at runtime on macOS, so no offline shader compiler is needed.
// Vulkan uses SPIR-V builds of the same shaders: shaders/scene.glsl, compiled into
// GraphicShadersSPV.h by port/tools/build_spirv.sh; change both together. D3D12 (DXIL) is missing.
//
// Binding layout follows SDL3's MSL rules: uniforms at [[buffer(0)]], textures and samplers in
// pairs from index 0, vertex input through [[stage_in]].

#pragma once

// Scene shader. Compiled twice: with COLOR_INTERP defined as [[flat]] for D3DSHADE_FLAT, and empty
// for Gouraud shading.
static const char kSceneShaderMSL[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

#ifndef COLOR_INTERP
#define COLOR_INTERP
#endif

struct VSIn {
    float4 pos   [[attribute(0)]];   // x, y in pixels; z in 0..1; rhw
    float4 color [[attribute(1)]];   // D3DCOLOR bytes B, G, R, A
    float2 uv    [[attribute(2)]];
};

struct VSOut {
    float4 pos [[position]];
    float  psize [[point_size]];
    float4 color COLOR_INTERP;
    float2 uv;
};

struct VSParams { float targetW; float targetH; };

vertex VSOut vs_main(VSIn in [[stage_in]], constant VSParams &p [[buffer(0)]])
{
    // Pre-transformed vertices: D3D8 puts pixel centres on integer coordinates, modern APIs on
    // half-integers, so shift by half a pixel to keep the same coverage and texel alignment.
    float w = in.pos.w != 0.0 ? 1.0 / in.pos.w : 1.0;
    float x = (in.pos.x + 0.5) / p.targetW * 2.0 - 1.0;
    float y = 1.0 - (in.pos.y + 0.5) / p.targetH * 2.0;
    VSOut out;
    out.pos = float4(x * w, y * w, in.pos.z * w, w);
    out.psize = 1.0;
    out.color = in.color.bgra;
    out.uv = in.uv;
    return out;
}

// Must match FSParams in Graphic.cpp (all 4-byte scalars, no padding).
struct FSParams {
    uint  mode;        // 0 untextured, 1 direct colour texture, 2 paletted (P8)
    uint  colorOp;     // D3DTOP_*
    uint  alphaOp;
    uint  alphaTest;
    uint  alphaFunc;   // D3DCMP_*
    float alphaRef;    // 0..1
    uint  addrU;       // D3DTADDRESS_* (used for paletted lookups, which bypass the sampler)
    uint  addrV;
    uint  linear;      // bilinear filtering for paletted lookups
    uint  texW;
    uint  texH;
    uint  palX;        // palette position in the atlas
    uint  palY;
};

static int wrapCoord(int c, int size, uint mode)
{
    if (mode == 3) return clamp(c, 0, size - 1);                 // CLAMP
    if (mode == 2) {                                             // MIRROR
        int period = size * 2;
        int m = ((c % period) + period) % period;
        return m < size ? m : period - 1 - m;
    }
    return ((c % size) + size) % size;                           // WRAP
}

static float4 paletteTexel(texture2d<float> idx, texture2d<float> pal, int2 c, const thread FSParams &p)
{
    uint x = (uint)wrapCoord(c.x, (int)p.texW, p.addrU);
    uint y = (uint)wrapCoord(c.y, (int)p.texH, p.addrV);
    uint i = (uint)(idx.read(uint2(x, y)).r * 255.0 + 0.5);
    return pal.read(uint2(p.palX + i, p.palY));
}

static float4 samplePaletted(texture2d<float> idx, texture2d<float> pal, float2 uv, const thread FSParams &p)
{
    float2 t = uv * float2(p.texW, p.texH);
    if (p.linear == 0) return paletteTexel(idx, pal, int2(floor(t)), p);
    // Filter after the palette lookup, as hardware P8 filtering did.
    t -= 0.5;
    int2 c = int2(floor(t));
    float2 f = fract(t);
    float4 a = paletteTexel(idx, pal, c, p);
    float4 b = paletteTexel(idx, pal, c + int2(1, 0), p);
    float4 d = paletteTexel(idx, pal, c + int2(0, 1), p);
    float4 e = paletteTexel(idx, pal, c + int2(1, 1), p);
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

static float4 combine(uint op, float4 tex, float4 diffuse)
{
    switch (op) {
    case 2:  return tex;                                // SELECTARG1 (texture)
    case 3:  return diffuse;                            // SELECTARG2 (diffuse)
    case 4:  return tex * diffuse;                      // MODULATE
    case 5:  return saturate(tex * diffuse * 2.0);      // MODULATE2X
    default: return diffuse;                            // DISABLE
    }
}

static bool alphaPasses(uint func, float a, float ref)
{
    switch (func) {
    case 1:  return false;
    case 2:  return a <  ref;
    case 3:  return a == ref;
    case 4:  return a <= ref;
    case 5:  return a >  ref;
    case 6:  return a != ref;
    case 7:  return a >= ref;
    default: return true;
    }
}

static float4 shade(float4 color, float2 uv, const thread FSParams &p, texture2d<float> tex, texture2d<float> pal,
                    sampler smp)
{
    float4 c = color;
    if (p.mode != 0) {
        // Explicit LOD: every texture has one mip level, and implicit derivatives are undefined
        // in this per-draw (non-uniform) branch; on Apple GPUs that gave garbage texels along
        // 16/32-pixel rows (magenta lines in the sky).
        float4 t = p.mode == 2 ? samplePaletted(tex, pal, uv, p) : tex.sample(smp, uv, level(0.0));
        c = float4(combine(p.colorOp, t, color).rgb, combine(p.alphaOp, t, color).a);
    }
    if (p.alphaTest != 0 && !alphaPasses(p.alphaFunc, floor(c.a * 255.0 + 0.5), p.alphaRef * 255.0)) {
#ifdef DEBUG_MESH_SHADE
        return float4(0.0, 1.0, 0.0, 2.0 + c.a);
#endif
        discard_fragment();
    }
    return c;
}

fragment float4 fs_main(VSOut in [[stage_in]], constant FSParams &p [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]], texture2d<float> pal [[texture(1)]],
                        sampler smp [[sampler(0)]], sampler palSmp [[sampler(1)]])
{
    FSParams lp = p;
    return shade(in.color, in.uv, lp, tex, pal, smp);
}

// ---- 3D models: the four vertex shaders of the original (cddata/vsh) in one function ----

struct MeshIn {
    float3 pos    [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv     [[attribute(2)]];
};

struct MeshOut {
    float4 pos [[position]];
    float4 color COLOR_INTERP;
    float2 uv;
    float  fog;
    uint   param [[flat]];      // index of this draw's parameters
};

// Must match MeshVSParams in Graphic.cpp.
struct MeshVS {
    float4 fog;             // c1
    float4 lightColor[3];   // c2-c4
    float4 m[4];            // c5-c8 as rows of the row-vector matrix
    float4 lightLocal[3];   // c9-c11
    float4 base;            // c12
    float4 stz;             // c13
    uint   kind;            // 0 light, 1 diffuse, 2 stzofs, 3 specular
    uint   pad0, pad1, pad2;
};

// Per-draw parameters come from storage buffers indexed by the instance (the draw's base
// instance), not from uniform pushes.
vertex MeshOut vs_mesh(MeshIn in [[stage_in]], const device MeshVS *params [[buffer(0)]],
                       uint iid [[instance_id]])
{
    const MeshVS c = params[iid];
    float4 p = in.pos.x * c.m[0] + in.pos.y * c.m[1] + in.pos.z * c.m[2] + c.m[3];
    if (c.kind == 2 || c.kind == 3) p.z += c.stz.z;          // z offset

    // Three directional lights: clamp(n . L) per light, then the colour matrix adds ambient.
    float4 d = float4(max(float3(dot(in.normal, c.lightLocal[0].xyz), dot(in.normal, c.lightLocal[1].xyz),
                                 dot(in.normal, c.lightLocal[2].xyz)), 0.0), 1.0);
    float3 lit = float3(dot(d, c.lightColor[0]), dot(d, c.lightColor[1]), dot(d, c.lightColor[2]));

    MeshOut out;
    out.pos = p;
    out.param = iid;
    bool lighting = c.kind == 0 || c.kind == 2;
    out.color = saturate(lighting ? float4(lit, 1.0) * c.base : c.base);
    if (c.kind == 2) out.uv = in.uv + c.stz.xy;
    // specular: a highlight lookup texture addressed by the light intensity (green row)
    else if (c.kind == 3) out.uv = min(float2(c.stz.x, dot(d, c.lightColor[1])), 1.0);
    else out.uv = in.uv;
    out.fog = c.kind == 2 ? 1.0 : saturate((c.fog.y - p.w) * c.fog.z);   // stzofs writes no fog
    return out;
}

// Must match MeshFSParams in Graphic.cpp.
struct MeshFS {
    FSParams p;
    float fogR, fogG, fogB;
    uint  fogOn;
};

fragment float4 fs_mesh(MeshOut in [[stage_in]], const device MeshFS *params [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]], texture2d<float> pal [[texture(1)]],
                        sampler smp [[sampler(0)]], sampler palSmp [[sampler(1)]])
{
#ifdef DEBUG_MESH_TEX
    return float4(tex.sample(smp, in.uv).rgb, 1.0);
#endif
#ifdef DEBUG_MESH_READ
    {
        uint2 sz = uint2(tex.get_width(), tex.get_height());
        float2 t = fract(in.uv) * float2(sz);
        return float4(tex.read(min(uint2(t), sz - 1)).rgb, 1.0);
    }
#endif
#ifdef DEBUG_MESH_COLOR
    return float4(in.color.rgb * 0.5, 1.0);
#endif
#ifdef DEBUG_MESH_ID
    return float4((float)(in.param & 255) / 255.0, (float)((in.param >> 8) & 255) / 255.0, 0.0, 1.0);
#endif
#ifdef DEBUG_MESH_UV
    if (any(isnan(in.uv)) || any(isinf(in.uv))) return float4(1.0, 0.0, 0.0, 1.0);
    return float4(fract(in.uv.x) * 0.5, fract(in.uv.y) * 0.5, abs(in.uv.y) > 4.0 ? 1.0 : 0.0, 1.0);
#endif
    const MeshFS f = params[in.param];
    FSParams fp = f.p;
#ifdef DEBUG_MESH_PARAMS
    return float4(float(fp.mode) / 4.0, float(fp.colorOp) / 8.0, float(fp.alphaOp) / 8.0, 1.0);
#endif
    float4 c = shade(in.color, in.uv, fp, tex, pal, smp);
#ifdef DEBUG_MESH_SHADE
    return c.a > 1.5 ? float4(0.0, 1.0, (c.a - 2.0), 1.0) : float4(c.rgb, 1.0);
#endif
    if (f.fogOn != 0) c.rgb = mix(float3(f.fogR, f.fogG, f.fogB), c.rgb, saturate(in.fog));
    return c;
}
)MSL";

// Final pass: scales the off-screen scene into the window and applies gamma
// (the original used IDirect3DDevice8::SetGammaRamp with pow(i/255, 1/gamma)).
static const char kPresentShaderMSL[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSOut { float4 pos [[position]]; float2 uv; };

vertex VSOut vs_present(uint vid [[vertex_id]])
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut out;
    out.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}

// Must match PresentParams in Graphic.cpp. uvOffset/uvScale select the shown part of the scene.
struct PresentParams { float invGamma, uvOffsetX, uvOffsetY, uvScaleX, uvScaleY; };

fragment float4 fs_present(VSOut in [[stage_in]], constant PresentParams &p [[buffer(0)]],
                           texture2d<float> scene [[texture(0)]], sampler smp [[sampler(0)]])
{
    float4 c = scene.sample(smp, float2(p.uvOffsetX, p.uvOffsetY) + in.uv * float2(p.uvScaleX, p.uvScaleY));
    return float4(pow(c.rgb, float3(p.invGamma)), 1.0);
}
)MSL";
