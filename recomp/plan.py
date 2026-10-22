"""Partition of the program for the recompiled build.

translate: everything reachable from the CRT entry, except inside NATIVE ranges.
native:    functions in NATIVE ranges that translated code calls (or takes the address of);
           each needs a hand-written replacement in host/.
"""
import collections, os, struct, sys
NOTES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')   # function lists exported from Ghidra

# Modules replaced by native code (port or stubs): [start, end) -> name
NATIVE = [
    (0x402110, 0x4040c0, 'Graphic'),     # CGraphic -> port (CLock at 0x4040c0-0x404110 is translated)
    (0x404110, 0x4055a0, 'Graphic'),
    (0x4055a0, 0x405f90, 'Input'),       # DirectInput -> SDL keyboard/gamepad (0x405f90 is translated)
    (0x405fe0, 0x406320, 'Input'),
    (0x406320, 0x406a00, 'Movie'),       # DirectShow -> stub
    (0x40a2e0, 0x40ca70, 'Texture'),     # CTexture -> port
    (0x40d1b0, 0x40f0f0, 'GS'),          # GS emulator, image upload, VU1, frame hooks -> port
    (0x411150, 0x412d10, 'Vertex'),      # CVertex -> port; 0x411030-0x411150 are small getters, translated
    # CSound/CSample (0x407ea0-0x40a2e0) and the CWave reader (0x412d10-0x413600) are translated;
    # DirectSound and mmio underneath are host/dsound.cpp.
    (0x413900, 0x4145b0, 'DMA'),         # GIF/VIF/DMA walker -> port
    (0x41ab00, 0x41af70, 'Tasks'),       # PS2 task scheduler -> host/tasks.cpp (fibers)
]
# Single functions replaced natively (window/message loop/WinMain).
NATIVE_FUNCS = {
    0x407260: 'WinMain', 0x4074a0: 'MessageLoop', 0x406f70: 'CreateMainWindow',
    0x4145b0: 'sceDmaSend', 0x414c60: 'sceGsSwapDBuffDc', 0x4075a0: 'FrameLimiter',
    # D3DX x87 math (host/d3dx.cpp)
    0x490b6b: 'D3DXVec2Transform', 0x490bd4: 'D3DXVec2TransformNormal', 0x490c09: 'D3DXVec3Transform',
    0x490c94: 'D3DXVec3TransformNormal', 0x490cf3: 'D3DXVec4Transform', 0x490d8c: 'D3DXVec3Normalize',
    0x490e56: 'D3DXMatrixMultiply', 0x490f39: 'D3DXMatrixMultiplyTranspose',
    0x48a909: 'D3DXMatrixRotationX', 0x48a9a5: 'D3DXMatrixRotationY', 0x48aa42: 'D3DXMatrixRotationZ',
    # VU0 math wrappers over D3DX (host/d3dx.cpp)
    0x415700: 'Vu0CopyVector', 0x415810: 'Vu0CopyMatrix', 0x415740: 'Vu0ApplyMatrix',
    0x415760: 'Vu0MulMatrix', 0x4158c0: 'Vu0RotMatrixY',
    # CRT helpers (host/d3dx.cpp)
    0x4a3340: '__ftol', 0x4a3850: '__allshl',
}


def native_name(a):
    if a in NATIVE_FUNCS:
        return NATIVE_FUNCS[a]
    for lo, hi, n in NATIVE:
        if lo <= a < hi:
            return n
    return None


def load():
    F = {}
    for l in open(os.path.join(NOTES, 'funcs.tsv')).read().splitlines()[1:]:
        p = l.split('\t')
        try:
            e = int(p[0], 16)
        except ValueError:
            continue
        F[e] = dict(size=int(p[1]), callees=[int(x, 16) for x in p[11].split(',') if x and not x.startswith('E')],
                    dref=int(p[10]))
    return F


def partition(F, roots, ptrs):
    translate, native = set(), collections.defaultdict(set)
    st = list(roots) + list(ptrs)
    while st:
        f = st.pop()
        if f in translate or f not in F:
            continue
        if native_name(f):
            continue
        translate.add(f)
        for c in F[f]['callees']:
            if native_name(c):
                native[c].add(f)
            else:
                st.append(c)
    return translate, native


if __name__ == '__main__':
    F = load()
    reach = [int(x, 16) for x in open(os.path.join(NOTES, 'reach_all.txt')).read().split()]
    # function pointers found anywhere in the image (see notes/game-structure.md)
    roots = [0x4a45af]
    ptrs = [f for f in reach if F.get(f, {}).get('dref')]
    T, N = partition(F, roots, [p for p in ptrs if not native_name(p)])
    # pointers to native functions also need replacements
    for p in ptrs:
        if native_name(p):
            N.setdefault(p, set()).add(0)
    print('translate: %d functions, %d bytes' % (len(T), sum(F[f]['size'] for f in T)))
    by = collections.defaultdict(list)
    for f, callers in sorted(N.items()):
        by[native_name(f)].append((f, len(callers)))
    print('native boundary: %d functions' % len(N))
    for k, v in by.items():
        print('  %-10s %3d: %s' % (k, len(v), ' '.join('%x(%d)' % x for x in v)))


# Runtime settings page in emulated memory, filled by the host at startup (host/native.cpp).
SETTINGS_BASE = 0x7ffc0000
SETTING_ASPECT_X = SETTINGS_BASE + 0      # float: 3D projection X aspect (1.0 = 4:3)
SETTING_ASPECT_Y = SETTINGS_BASE + 4      # float: 3D projection Y aspect (0.5 originally)
SETTING_DD_FIXED = SETTINGS_BASE + 8      # int: 1 = draw distance fixed by the player
SETTING_OKIDIST = SETTINGS_BASE + 12      # float: OkiDistArg with a fixed draw distance
SETTING_SCRATCH = SETTINGS_BASE + 16      # float: where the route's draw distance goes then
SETTING_DD_AUTO = SETTINGS_BASE + 20      # int: 1 = automatic draw distance fed by the host's load
SETTING_DD_LOAD = SETTINGS_BASE + 24      # int: load of the last frame, 525 = one frame period
SETTING_DD_BUDGET = SETTINGS_BASE + 28    # int: load the draw distance is steered to (same units)
SETTING_DD_MIN = SETTINGS_BASE + 32       # float: smallest automatic draw distance (original 0.45)
SETTING_DD_MAX = SETTINGS_BASE + 36       # float: largest automatic draw distance (original 1.0)
SETTING_DD_DOWN = SETTINGS_BASE + 40      # float: step down per tick far over the budget (0.02)
SETTING_DD_UP = SETTINGS_BASE + 44        # float: step up per tick far under it (0.02)
SETTING_DD_UP2 = SETTINGS_BASE + 48       # float: step up per tick under it (0.008)

# Immediates replaced by settings. These are the sites DDGModernizer patches for widescreen
# (data/ddgmodernizer-final.txt, [Aspect]); exepatch.py sets them to 1.0 / 0.5 before translation.
IMM_OVERRIDES = {
    0x4178eb: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x4178f0: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x417a5f: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x417a64: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x482e08: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x482e0d: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x48308e: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x483093: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x447112: 'RD32(0x%xu)' % SETTING_ASPECT_X,   # mov dword [esp+4], AspectX
    0x447125: 'RD32(0x%xu)' % SETTING_ASPECT_Y,   # mov dword [esp], AspectY
    # OkiDistArg the route set-up passes on: the player's okidist with a fixed draw distance,
    # the original's 0.4 otherwise (drawdistance = auto)
    0x47e50a: 'RD32(0x%xu) ? RD32(0x%xu) : 0x3ecccccdu' % (SETTING_DD_FIXED, SETTING_OKIDIST),
}
# Absolute memory operands replaced by settings. 0x47e504 stores the draw distance the game
# computes from the train's speed every tick (0x47e1d0) into RendPow; with a fixed draw distance
# it goes to a scratch slot instead (DDGModernizer's "Fix magnification").
#
# 0x47e1d0 is a load-driven controller (as in the PSP version, which steers to 30 frames/s): it
# compares max(cor, dra) (0x75841c, 0x758420: times the frame driver measures, in NTSC lines) with a
# budget (0x4078c0: the "rendpow" registry value's table entry, 155 by default) and moves the draw
# distance by 0.02 / 0.008 / a speed-dependent step per tick within 0.45-1.0, averaged over 60
# ticks. On the PC "cor" spans the whole tick including the frame limiter's wait (~525 at 60
# frames/s), so it only ever shrinks: 0.45 always. With SETTING_DD_AUTO the controller reads the
# host's measured load and budget and its limits come from the settings page instead.
_DD_AUTO = 'RD32(0x%xu)' % SETTING_DD_AUTO
MEM_OVERRIDES = {
    0x47e504: 'RD32(0x%xu) ? 0x%xu : 0x5db0f8u' % (SETTING_DD_FIXED, SETTING_SCRATCH),
    0x47e28c: '%s ? 0x%xu : 0x75841cu' % (_DD_AUTO, SETTING_DD_LOAD),       # cor
    0x47e292: '%s ? 0x%xu : 0x758420u' % (_DD_AUTO, SETTING_DD_LOAD),       # dra
    0x4078c0: '%s ? 0x%xu : 0x4c8c00u' % (_DD_AUTO, SETTING_DD_BUDGET),     # budget getter
    0x47e2b8: '%s ? 0x%xu : 0x4b2ed4u' % (_DD_AUTO, SETTING_DD_MIN),        # floor 0.45
    0x47e2c7: '%s ? 0x%xu : 0x4b2ed4u' % (_DD_AUTO, SETTING_DD_MIN),
    0x47e318: '%s ? 0x%xu : 0x4b2374u' % (_DD_AUTO, SETTING_DD_MAX),        # ceiling 1.0
    # steps: the original's 0.02 per tick crosses the whole range within a second, which with the
    # host's noisier load made the draw distance swing (scenery popping in and out)
    0x47e2b2: '%s ? 0x%xu : 0x4b25f0u' % (_DD_AUTO, SETTING_DD_DOWN),       # -0.02
    0x47e2ed: '%s ? 0x%xu : 0x4b25f0u' % (_DD_AUTO, SETTING_DD_UP),         # +0.02
    0x47e2fe: '%s ? 0x%xu : 0x4b2f8cu' % (_DD_AUTO, SETTING_DD_UP2),        # +0.008
    0x47e327: '%s ? 0x%xu : 0x4b2374u' % (_DD_AUTO, SETTING_DD_MAX),
}
# Conditional jumps also taken when the expression is true: 0x423621 (object fade distance)
# uses the cab view's rule in every view with a fixed draw distance (DDGModernizer's "Fix for
# outer view").
#
# 0x47e3d2: after its 60-tick average, 0x47e1d0 sets a hold (0x730c60) the first time the average
# falls and from then on caps the result at the hold, which only ever falls further (reset only at
# a route's start, 0x455254): the draw distance can only shrink during a run, after one loading
# spike for good. The PSP version (Pocket, 0x3ef84) has no hold; with SETTING_DD_AUTO the hold is
# never set either, so the draw distance follows the load both ways.
BRANCH_OVERRIDES = {
    0x423621: 'RD32(0x%xu)' % SETTING_DD_FIXED,
    0x47e3d2: _DD_AUTO,
}
