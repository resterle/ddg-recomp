// scene.glsl - Vulkan GLSL version of kSceneShaderMSL and kPresentShaderMSL (GraphicShaders.h),
// compiled to SPIR-V by port/tools/build_spirv.sh into GraphicShadersSPV.h. Keep the two in step.
//
// One stage per compile, chosen with -D: VS_MAIN, FS_MAIN (2D), VS_MESH, FS_MESH (models),
// VS_PRESENT, FS_PRESENT; FLAT_COLOR for D3DSHADE_FLAT (the MSL's COLOR_INTERP [[flat]]).
// Resource sets follow SDL3's SPIR-V rules: vertex stage set 0 = samplers, storage buffers;
// set 1 = uniform buffers. Fragment stage set 2 = samplers, then storage buffers; set 3 = uniform
// buffers. SDL's Vulkan backend flips the viewport, so clip space is y-up as on Metal.
#version 450

#ifdef FLAT_COLOR
#define COLOR_INTERP flat
#else
#define COLOR_INTERP
#endif

// ---- shared fragment code ----------------------------------------------------------------------
#if defined(FS_MAIN) || defined(FS_MESH)

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

layout(set = 2, binding = 0) uniform sampler2D tex;
layout(set = 2, binding = 1) uniform sampler2D pal;

int wrapCoord(int c, int size, uint mode)
{
    if (mode == 3u) return clamp(c, 0, size - 1);               // CLAMP
    if (mode == 2u) {                                           // MIRROR
        int period = size * 2;
        int m = ((c % period) + period) % period;
        return m < size ? m : period - 1 - m;
    }
    return ((c % size) + size) % size;                          // WRAP
}

vec4 paletteTexel(ivec2 c, FSParams p)
{
    int x = wrapCoord(c.x, int(p.texW), p.addrU);
    int y = wrapCoord(c.y, int(p.texH), p.addrV);
    uint i = uint(texelFetch(tex, ivec2(x, y), 0).r * 255.0 + 0.5);
    return texelFetch(pal, ivec2(int(p.palX + i), int(p.palY)), 0);
}

vec4 samplePaletted(vec2 uv, FSParams p)
{
    vec2 t = uv * vec2(p.texW, p.texH);
    if (p.linear == 0u) return paletteTexel(ivec2(floor(t)), p);
    // Filter after the palette lookup, as hardware P8 filtering did.
    t -= 0.5;
    ivec2 c = ivec2(floor(t));
    vec2 f = fract(t);
    vec4 a = paletteTexel(c, p);
    vec4 b = paletteTexel(c + ivec2(1, 0), p);
    vec4 d = paletteTexel(c + ivec2(0, 1), p);
    vec4 e = paletteTexel(c + ivec2(1, 1), p);
    return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

vec4 combine(uint op, vec4 t, vec4 diffuse)
{
    switch (op) {
    case 2u: return t;                                          // SELECTARG1 (texture)
    case 3u: return diffuse;                                    // SELECTARG2 (diffuse)
    case 4u: return t * diffuse;                                // MODULATE
    case 5u: return clamp(t * diffuse * 2.0, 0.0, 1.0);         // MODULATE2X
    default: return diffuse;                                    // DISABLE
    }
}

bool alphaPasses(uint func, float a, float ref)
{
    switch (func) {
    case 1u: return false;
    case 2u: return a <  ref;
    case 3u: return a == ref;
    case 4u: return a <= ref;
    case 5u: return a >  ref;
    case 6u: return a != ref;
    case 7u: return a >= ref;
    default: return true;
    }
}

// false: the fragment fails the alpha test
// uvDx, uvDy: derivatives of uv, taken by the caller in uniform control flow (they are undefined
// in this per-draw branch); they select the mip level of texture pack replacements, the game's
// own textures have only level 0
bool shade(vec4 color, vec2 uv, vec2 uvDx, vec2 uvDy, FSParams p, out vec4 c)
{
    c = color;
    if (p.mode != 0u) {
        vec4 t = p.mode == 2u ? samplePaletted(uv, p) : textureGrad(tex, uv, uvDx, uvDy);
        c = vec4(combine(p.colorOp, t, color).rgb, combine(p.alphaOp, t, color).a);
    }
    return p.alphaTest == 0u || alphaPasses(p.alphaFunc, floor(c.a * 255.0 + 0.5), p.alphaRef * 255.0);
}
#endif

// ---- 2D (pre-transformed vertices) -------------------------------------------------------------
#ifdef VS_MAIN
layout(location = 0) in vec4 inPos;      // x, y in pixels; z in 0..1; rhw
layout(location = 1) in vec4 inColor;    // D3DCOLOR bytes B, G, R, A
layout(location = 2) in vec2 inUV;
layout(location = 0) COLOR_INTERP out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(set = 1, binding = 0) uniform VSParams { float targetW; float targetH; };

void main()
{
    // D3D8 puts pixel centres on integer coordinates, modern APIs on half-integers
    float w = inPos.w != 0.0 ? 1.0 / inPos.w : 1.0;
    float x = (inPos.x + 0.5) / targetW * 2.0 - 1.0;
    float y = 1.0 - (inPos.y + 0.5) / targetH * 2.0;
    gl_Position = vec4(x * w, y * w, inPos.z * w, w);
    gl_PointSize = 1.0;
    vColor = inColor.bgra;
    vUV = inUV;
}
#endif

#ifdef FS_MAIN
layout(location = 0) COLOR_INTERP in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 3, binding = 0) uniform FSBlock { FSParams p; };

void main()
{
    vec2 dx = dFdx(vUV), dy = dFdy(vUV);
    vec4 c;
    if (!shade(vColor, vUV, dx, dy, p, c)) discard;
    fragColor = c;
}
#endif

// ---- 3D models: the four vertex shaders of the original (cddata/vsh) in one function -----------
#ifdef VS_MESH
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 0) COLOR_INTERP out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vFog;
layout(location = 3) flat out uint vParam;     // index of this draw's parameters

// Must match MeshVSParams in Graphic.cpp.
struct MeshVS {
    vec4 fog;             // c1
    vec4 lightColor[3];   // c2-c4
    vec4 m[4];            // c5-c8 as rows of the row-vector matrix
    vec4 lightLocal[3];   // c9-c11
    vec4 base;            // c12
    vec4 stz;             // c13
    uint kind;            // 0 light, 1 diffuse, 2 stzofs, 3 specular
    uint pad0, pad1, pad2;
};
// per-draw parameters, indexed by the instance (the draw's first instance)
layout(std430, set = 0, binding = 0) readonly buffer MeshVSBuf { MeshVS params[]; };

void main()
{
    MeshVS c = params[gl_InstanceIndex];
    vec4 p = inPos.x * c.m[0] + inPos.y * c.m[1] + inPos.z * c.m[2] + c.m[3];
    if (c.kind == 2u || c.kind == 3u) p.z += c.stz.z;              // z offset

    // Three directional lights: clamp(n . L) per light, then the colour matrix adds ambient.
    vec4 d = vec4(max(vec3(dot(inNormal, c.lightLocal[0].xyz), dot(inNormal, c.lightLocal[1].xyz),
                           dot(inNormal, c.lightLocal[2].xyz)), 0.0), 1.0);
    vec3 lit = vec3(dot(d, c.lightColor[0]), dot(d, c.lightColor[1]), dot(d, c.lightColor[2]));

    gl_Position = p;
    vParam = uint(gl_InstanceIndex);
    bool lighting = c.kind == 0u || c.kind == 2u;
    vColor = clamp(lighting ? vec4(lit, 1.0) * c.base : c.base, 0.0, 1.0);
    if (c.kind == 2u) vUV = inUV + c.stz.xy;
    // specular: a highlight lookup texture addressed by the light intensity (green row)
    else if (c.kind == 3u) vUV = min(vec2(c.stz.x, dot(d, c.lightColor[1])), 1.0);
    else vUV = inUV;
    vFog = c.kind == 2u ? 1.0 : clamp((c.fog.y - p.w) * c.fog.z, 0.0, 1.0);   // stzofs writes no fog
}
#endif

#ifdef FS_MESH
layout(location = 0) COLOR_INTERP in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vFog;
layout(location = 3) flat in uint vParam;
layout(location = 0) out vec4 fragColor;

// Must match MeshFSParams in Graphic.cpp.
struct MeshFS {
    FSParams p;
    float fogR, fogG, fogB;
    uint  fogOn;
};
layout(std430, set = 2, binding = 2) readonly buffer MeshFSBuf { MeshFS params[]; };

void main()
{
    vec2 dx = dFdx(vUV), dy = dFdy(vUV);
    MeshFS f = params[vParam];
    vec4 c;
    if (!shade(vColor, vUV, dx, dy, f.p, c)) discard;
    if (f.fogOn != 0u) c.rgb = mix(vec3(f.fogR, f.fogG, f.fogB), c.rgb, clamp(vFog, 0.0, 1.0));
    fragColor = c;
}
#endif

// ---- final pass: scales the off-screen scene into the window and applies gamma -----------------
#ifdef VS_PRESENT
layout(location = 0) out vec2 vUV;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
    vUV = uv;
}
#endif

#ifdef FS_PRESENT
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 2, binding = 0) uniform sampler2D scene;
// Must match PresentParams in Graphic.cpp. uvOffset/uvScale select the shown part of the scene.
layout(set = 3, binding = 0) uniform PresentParams { float invGamma, uvOffsetX, uvOffsetY, uvScaleX, uvScaleY, uvMinX, uvMinY, uvMaxX, uvMaxY; };

void main()
{
    vec2 uv = clamp(vec2(uvOffsetX, uvOffsetY) + vUV * vec2(uvScaleX, uvScaleY), vec2(uvMinX, uvMinY), vec2(uvMaxX, uvMaxY));
    vec4 c = texture(scene, uv);
    fragColor = vec4(pow(c.rgb, vec3(invGamma)), 1.0);
}
#endif
