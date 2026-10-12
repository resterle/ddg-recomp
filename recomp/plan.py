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

# Immediates replaced by settings. These are the sites DDGModernizer patches for widescreen
# (data/ddgmodernizer-final.txt, [Aspect]); exepatch.py sets them to 1.0 / 0.5 before translation.
IMM_OVERRIDES = {
    0x4178eb: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x4178f0: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x417a5f: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x417a64: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x482e08: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x482e0d: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x48308e: 'RD32(0x%xu)' % SETTING_ASPECT_Y, 0x483093: 'RD32(0x%xu)' % SETTING_ASPECT_X,
    0x447112: 'RD32(0x%xu)' % SETTING_ASPECT_X,   # mov dword [esp+4], AspectX
    0x447125: 'RD32(0x%xu)' % SETTING_ASPECT_Y,   # mov dword [esp], AspectY
}
