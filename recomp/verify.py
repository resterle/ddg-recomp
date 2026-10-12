"""Differential test: run original functions in Unicorn and their translation in C from the
same random state, compare registers and memory.

    verify.py [--leaf] [--max-size N] [--limit N] [--trials N] [--only HEX,...]
"""
import argparse, ctypes, os, random, struct, subprocess, sys, collections
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_WRITE, UC_PROT_ALL
from unicorn.x86_const import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_REG_FS
from pe import PE
from gen import generate
import exepatch

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get('PERFECT_EXE', os.path.join(HERE, 'perfect.exe'))   # the exe the game will run with
NOTES = os.path.join(HERE, 'data')
GAME = (0x41d000, 0x483800)

STACK_LO, STACK_HI = 0x00200000, 0x00300000
SCRATCH, SCRATCH_SIZE = 0x01000000, 0x00100000
SENTINEL = 0x00dead00
PAGE = 0x1000

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
UREG = dict(eax=UC_X86_REG_EAX, ecx=UC_X86_REG_ECX, edx=UC_X86_REG_EDX, ebx=UC_X86_REG_EBX,
            esp=UC_X86_REG_ESP, ebp=UC_X86_REG_EBP, esi=UC_X86_REG_ESI, edi=UC_X86_REG_EDI)


class CPU(ctypes.Structure):
    _fields_ = [(r, ctypes.c_uint32) for r in REGS] + [('st', ctypes.c_double * 8), ('top', ctypes.c_uint32),
                                                       ('sw', ctypes.c_uint32), ('cw', ctypes.c_uint32)]


def load_funcs():
    F = {}
    for l in open(os.path.join(NOTES, 'funcs.tsv')).read().splitlines()[1:]:
        p = l.split('\t')
        try:
            e = int(p[0], 16)
        except ValueError:
            continue
        callees = [int(x, 16) for x in p[11].split(',') if x and not x.startswith('EXTERNAL')]
        F[e] = dict(size=int(p[1]), callees=callees, icalls=int(p[6]), dref=int(p[10]))
    return F


def true_entries(F):
    """Real function entries: Ghidra's original functions, call targets and data-referenced
    functions. The code-recovery pass also created functions at bare branch targets (switch
    cases Ghidra had not followed); those are local code, not entries."""
    ent = set()
    for l in open(os.path.join(NOTES, 'functions.tsv')).read().splitlines()[1:]:
        ent.add(int(l.split('\t')[0], 16))
    for f, d in F.items():
        ent.update(d['callees'])
        if d.get('dref'):
            ent.add(f)
    return ent


def closure(F, root):
    seen, st = set(), [root]
    while st:
        f = st.pop()
        if f in seen:
            continue
        seen.add(f)
        st += F.get(f, {}).get('callees', [])
    return seen


def uses_fs(pe, F, f):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for ins in md.disasm(pe.read(f, F[f]['size']), f):
        if any(o.type == 3 and o.mem.segment == X86_REG_FS for o in ins.operands):
            return True
    return False


def ext80_to_float(b):
    mant = int.from_bytes(b[:8], 'little')
    se = int.from_bytes(b[8:10], 'little')
    sign, exp = se >> 15, se & 0x7fff
    if exp == 0 and mant == 0:
        return -0.0 if sign else 0.0
    if exp == 0x7fff:
        return float('nan') if mant << 1 & ((1 << 64) - 1) else (float('-inf') if sign else float('inf'))
    v = mant / (1 << 63) * 2.0 ** (exp - 16383)
    return -v if sign else v


class Harness:
    def __init__(self, pe, lib_path):
        self.pe = pe
        self.lib = ctypes.CDLL(lib_path)
        self.lib.rt_init.restype = ctypes.c_void_p
        self.lib.rt_cpu.restype = ctypes.POINTER(CPU)
        self.lib.rt_error.restype = ctypes.c_char_p
        self.lib.rt_run.argtypes = [ctypes.c_uint32]
        self.mem = self.lib.rt_init()
        self.cpu = self.lib.rt_cpu().contents
        self.dirty = set()

    def write(self, addr, data):
        ctypes.memmove(self.mem + addr, data, len(data))

    def read(self, addr, n):
        return ctypes.string_at(self.mem + addr, n)

    def image(self):
        if not hasattr(self, '_img'):
            self._img = []
            for s in self.pe.sections:
                size = (max(s['vsize'], s['rsize']) + PAGE - 1) & ~(PAGE - 1)
                self._img.append((s['va'], size, s['raw'] + b'\0' * (size - len(s['raw']))))
        return self._img


def pristine_page(h, p):
    """Initial content of page p (page number) for a trial: image, scratch or zero."""
    a = p << 12
    if not hasattr(h, '_pages'):
        h._pages = {}
        for va, size, data in h.image():
            for o in range(0, size, PAGE):
                h._pages[(va + o) >> 12] = data[o:o + PAGE]
        for o in range(0, SCRATCH_SIZE, PAGE):
            h._pages[(SCRATCH + o) >> 12] = h._scr[o:o + PAGE]
    return h._pages.get(p, b'\0' * PAGE)


def setup_unicorn(h):
    h.uc = Uc(UC_ARCH_X86, UC_MODE_32)
    for va, size, data in h.image():
        h.uc.mem_map(va, size, UC_PROT_ALL)
        h.uc.mem_write(va, data)
    h.uc.mem_map(STACK_LO, STACK_HI - STACK_LO)
    h.uc.mem_map(SCRATCH, SCRATCH_SIZE)
    h.uc.mem_write(SCRATCH, h._scr)
    h.extra = []
    h.uwritten = set()

    def on_unmapped(uc_, access, address, size, value, user):
        if len(h.extra) > 4096:
            return False
        try:
            for b in {address & ~(PAGE - 1), (address + size - 1) & ~(PAGE - 1)}:
                uc_.mem_map(b, PAGE)
                h.extra.append(b)
            return True
        except UcError:
            return False

    def on_write(uc_, access, address, size, value, user):
        h.uwritten.add(address >> 12)
        h.uwritten.add((address + size - 1) >> 12)
    h.uc.hook_add(UC_HOOK_MEM_UNMAPPED, on_unmapped)
    h.uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    h.cdirty = (ctypes.c_uint32 * 65536)()
    h.lib.rt_dirty.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
    # C side starts from the same image
    for va, size, data in h.image():
        h.write(va, data)
    h.write(SCRATCH, h._scr)
    h.lib.rt_dirty(h.cdirty, 0)
    h.reset_pages = set()


def trial(h, f, rng, F, limit):
    if not hasattr(h, '_scr'):
        h._scr = bytes(random.Random(1234).getrandbits(8) for _ in range(SCRATCH_SIZE))
    if not hasattr(h, 'uc'):
        setup_unicorn(h)
    uc = h.uc

    # restore every page either side touched in the previous trial
    for p in h.reset_pages:
        if p >= 1 << 20:            # address + size crossed 4 GB: no such page
            continue
        data = pristine_page(h, p)
        h.write(p << 12, data)
        try:
            uc.mem_write(p << 12, data)
        except UcError:
            pass
    for b in h.extra:
        uc.mem_unmap(b, PAGE)
    h.extra.clear()
    h.uwritten.clear()

    def val():
        k = rng.random()
        if k < 0.35:
            return rng.randrange(0, 256)
        if k < 0.75:
            return SCRATCH + rng.randrange(0, SCRATCH_SIZE - 0x1000) & ~3
        return rng.getrandbits(32)

    regs = {r: val() for r in REGS}
    esp = STACK_HI - 0x2000
    regs['esp'] = esp
    stack = struct.pack('<I', SENTINEL) + b''.join(struct.pack('<I', val()) for _ in range(16))
    uc.mem_write(esp, stack)
    h.write(esp, stack)
    stack_pages = {esp >> 12, (esp + len(stack)) >> 12}

    for r in REGS:
        uc.reg_write(UREG[r], regs[r])
    uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    uc.reg_write(UC_X86_REG_FPSW, 0)
    skip = None
    try:
        uc.emu_start(f, SENTINEL, count=limit)
        if uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
            skip = 'instruction limit'
    except UcError as e:
        skip = 'unicorn: %s' % e

    for r in REGS:
        setattr(h.cpu, r, regs[r])
    h.cpu.top = 0
    h.cpu.sw = 0
    h.cpu.cw = 0x027f
    if skip:
        h.reset_pages = h.uwritten | stack_pages | {b >> 12 for b in h.extra}
        return 'skip', skip
    trapped = h.lib.rt_run(f)
    n = h.lib.rt_dirty(h.cdirty, 65536)
    cpages = set(h.cdirty[i] for i in range(min(n, 65536)))
    pages = h.uwritten | cpages | {b >> 12 for b in h.extra}
    h.reset_pages = pages | stack_pages
    if trapped:
        return 'fail', 'C trapped: %s' % h.lib.rt_error().decode()

    for r in REGS:
        a, b = uc.reg_read(UREG[r]), getattr(h.cpu, r)
        if a != b:
            return 'fail', '%s: unicorn %08x C %08x' % (r, a, b)
    fpsw = uc.reg_read(UC_X86_REG_FPSW)
    utop = (fpsw >> 11) & 7
    if utop != h.cpu.top & 7:
        return 'fail', 'x87 top: unicorn %d C %d' % (utop, h.cpu.top & 7)
    if utop != 0:
        raw = uc.reg_read(UC_X86_REG_FP0 + utop)
        st0 = ext80_to_float(raw[0].to_bytes(8, 'little') + raw[1].to_bytes(2, 'little'))
        c = h.cpu.st[h.cpu.top & 7]
        if not (st0 == c or (st0 != st0 and c != c) or abs(st0 - c) <= 1e-9 * max(1.0, abs(st0))):
            return 'fail', 'ST0: unicorn %r C %r' % (st0, c)
    for p in sorted(pages):
        try:
            u = bytes(uc.mem_read(p << 12, PAGE))
        except UcError:
            u = b'\0' * PAGE
        c = h.read(p << 12, PAGE)
        if u != c:
            i = next(k for k in range(PAGE) if u[k] != c[k])
            return 'fail', 'memory %08x: unicorn %s C %s' % ((p << 12) + i, u[i:i + 8].hex(), c[i:i + 8].hex())
    return 'pass', ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--leaf', action='store_true', help='only functions without calls')
    ap.add_argument('--max-size', type=int, default=4000)
    ap.add_argument('--limit', type=int, default=0, help='number of functions')
    ap.add_argument('--trials', type=int, default=20)
    ap.add_argument('--only', default='')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--insn-limit', type=int, default=200000)
    args = ap.parse_args()

    pe = PE(exepatch.load(EXE))
    F = load_funcs()
    reach = set(int(x, 16) for x in open(os.path.join(NOTES, 'reach_all.txt')).read().split())
    game = [f for f in sorted(reach) if GAME[0] <= f < GAME[1] and f in F]
    if args.only:
        cands = [int(x, 16) for x in args.only.split(',')]
    else:
        cands = []
        for f in game:
            c = closure(F, f)
            if any(not (GAME[0] <= x < GAME[1]) for x in c):
                continue                    # reaches the SDK/PC layer: needs HLE, later
            if args.leaf and len(c) > 1:
                continue
            if F[f]['size'] > args.max_size or F[f]['icalls']:
                continue
            if uses_fs(pe, F, f):
                continue
            cands.append(f)
        if args.limit:
            cands = cands[:args.limit]
    entries = true_entries(F)
    cands = [f for f in cands if f in entries]
    todo = set()
    for f in cands:
        todo |= closure(F, f)
    src = os.path.join(HERE, 'build', 'verify_funcs_%d.c' % os.getpid())
    os.makedirs(os.path.dirname(src), exist_ok=True)
    # Grow the set with whatever the translation references (tail calls, fall-through) until
    # no game function is left as a stub.
    while True:
        ok, failed, stubs = generate(pe, todo, entries, src)
        more = {x for x in stubs if GAME[0] <= x < GAME[1] and x not in todo}
        if not more:
            break
        todo |= more
    lib = os.path.join(HERE, 'build', 'verify_%d.dylib' % os.getpid())
    if not os.environ.get('VERIFY_KEEP'):           # VERIFY_KEEP=1 keeps the generated C and dylib
        import atexit
        atexit.register(lambda: [os.remove(p) for p in (src, lib) if os.path.exists(p)])
    r = subprocess.run(['clang', '-O1', '-w', '-shared', '-fPIC', '-fno-strict-aliasing', '-DRT_TRACK_WRITES', '-I', HERE,
                        '-o', lib, src, os.path.join(HERE, 'rt_test.c')], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000])
        sys.exit(1)
    # which candidates can reach a non-game stub through the translated code
    import re as _re
    text = open(src).read()
    refs = {}
    for m in _re.finditer(r'RtRet F_([0-9a-f]{8})\(RT_PARAMS\)\n\{(.*?)\n\}', text, _re.S):
        refs[int(m.group(1), 16)] = {int(x, 16) for x in _re.findall(r'\bF_([0-9a-f]{8})\b', m.group(2))}
    nongame = {x for x in stubs if not (GAME[0] <= x < GAME[1])}
    bad_cache = {}

    def reaches_stub(root):
        seen, st = set(), [root]
        while st:
            x = st.pop()
            if x in seen:
                continue
            seen.add(x)
            if x in nongame:
                return {root}
            st += refs.get(x, ())
        return set()
    h = Harness(pe, lib)
    # set up the emulator and scratch data once; every forked child inherits them
    h._scr = bytes(random.Random(1234).getrandbits(8) for _ in range(SCRATCH_SIZE))
    setup_unicorn(h)
    rng = random.Random(args.seed)
    stats = collections.Counter()
    skipwhy = collections.Counter()
    fails = []
    for n, f in enumerate(cands):
        print('[%d/%d] %08x' % (n + 1, len(cands), f), end='\r', flush=True)
        if any(not (GAME[0] <= x < GAME[1]) for x in stubs) and f in reaches_stub(f):
            stats['needs-hle'] += 1
            continue
        if f in failed or any(x in failed for x in closure(F, f)):
            stats['lift-error'] += 1
            fails.append((f, 'lift', failed.get(f) or next(failed[x] for x in closure(F, f) if x in failed)))
            continue
        # The trials run in a forked child: random inputs can make code (or the harness) crash,
        # which then counts as a failure of this function instead of ending the whole run.
        seed = rng.getrandbits(32)
        rd, wr = os.pipe()
        pid = os.fork()
        if pid == 0:
            os.close(rd)
            h.lib.rt_after_fork()           # alias view of the guest memory over the child's copy
            import signal
            signal.alarm(60)                # a translated function that never returns fails, not hangs
            crng = random.Random(seed)
            out = []
            for _ in range(args.trials):
                out.append(trial(h, f, crng, F, args.insn_limit))
            os.write(wr, repr(out).encode())
            os._exit(0)
        os.close(wr)
        data = b''
        while True:
            chunk = os.read(rd, 65536)
            if not chunk:
                break
            data += chunk
        os.close(rd)
        _, status = os.waitpid(pid, 0)
        res = collections.Counter()
        msg = ''
        if os.WIFSIGNALED(status) or not data:
            res['fail'] += 1
            msg = 'crashed (signal %d)' % (os.WTERMSIG(status) if os.WIFSIGNALED(status) else 0)
        else:
            import ast
            for st, m in ast.literal_eval(data.decode()):
                if st == 'skip': skipwhy[m.split(':')[0] if m.startswith('unicorn') else m] += 1
                res[st] += 1
                if st == 'fail' and not msg:
                    msg = m
        if res['fail']:
            stats['fail'] += 1
            fails.append((f, 'fail', msg))
        elif res['pass']:
            stats['pass'] += 1
        else:
            stats['all-skipped'] += 1
    print('functions %d: %s' % (len(cands), dict(stats)))
    print('skip reasons (trials):', dict(skipwhy.most_common(8)))
    errs = collections.Counter(m.split(' at ')[0] for f, k, m in fails if k == 'lift')
    if errs:
        print('lift errors:', dict(errs.most_common(12)))
    for f, k, m in fails[:60]:
        if k == 'fail':
            print('  FAIL %08x: %s' % (f, m))


if __name__ == '__main__':
    main()
