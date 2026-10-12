"""Static helpers for reverse engineering perfect.exe (run from recomp/, needs capstone).

    tools/retool.py dis <addr>[:len] ...     disassemble (stops at the first ret unless --all)
    tools/retool.py xref <addr> ...          instructions that reference the addresses
    tools/retool.py calls <callee>           the 8 instructions before every call to callee (arguments)
    tools/retool.py profile <addr> ...       direct callees, imports within two levels, strings, globals
    tools/retool.py reach <addr> ...         PC-layer modules and named functions reachable by direct calls

The exe comes from PERFECT_EXE (default recomp/perfect.exe) and is patched like the build does
(exepatch.py). Function sizes and call lists come from data/funcs.tsv; names, when present, from
the file in RE_NAMES (address<TAB>name, as written by scripts/ExportNames.java).
"""
import bisect, collections, os, re, struct, sys
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)
import exepatch, plan
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = os.environ.get('PERFECT_EXE', os.path.join(HERE, 'perfect.exe'))
pe = PE(exepatch.load(EXE))
md = Cs(CS_ARCH_X86, CS_MODE_32)

SIZE, CALLEES = {}, {}
for l in open(os.path.join(HERE, 'data', 'funcs.tsv')).read().splitlines()[1:]:
    p = l.split('\t')
    try:
        f = int(p[0], 16)
    except ValueError:
        continue
    SIZE[f] = int(p[1])
    CALLEES[f] = [int(x, 16) for x in p[11].split(',') if x and not x.startswith('EXT')]
ENTRIES = sorted(SIZE)
NAMES = {}
if os.environ.get('RE_NAMES') and os.path.exists(os.environ['RE_NAMES']):
    for l in open(os.environ['RE_NAMES']).read().splitlines()[1:]:
        a, n = l.split('\t')
        if 'switchD' not in n and 'caseD' not in n:
            NAMES[int(a, 16)] = n
# PC-layer and SDK regions (plan.NATIVE plus the translated parts of the PC layer)
MODULES = [(a, b, n) for a, b, n in plan.NATIVE] + [
    (0x401000, 0x402110, 'CFile/registry'), (0x407ea0, 0x40a2e0, 'Sound'), (0x412d10, 0x413600, 'CWave'),
    (0x413600, 0x415000, 'PS2 DMA/GS/VIF emulation'), (0x415000, 0x416000, 'VU0 maths'),
    (0x419000, 0x41a000, 'sceGs/vsync'), (0x41ab00, 0x41b000, 'task scheduler'), (0x41b000, 0x41d000, 'file I/O')]


def containing(a):
    return ENTRIES[bisect.bisect_right(ENTRIES, a) - 1]


def insns(a, n):
    return list(md.disasm(pe.read(a, n), a))


def text():
    t = next(s for s in pe.sections if s['name'] == '.text')
    d = Cs(CS_ARCH_X86, CS_MODE_32)
    d.skipdata = True
    return d.disasm(t['raw'], t['va'])


def fmt(i):
    c = i.op_str
    if i.mnemonic == 'call' and c.startswith('dword ptr [0x'):
        c += '  ; ' + pe.imports.get(int(c[11:-1], 16), '')
    elif i.mnemonic == 'call' and c.startswith('0x') and int(c, 16) in NAMES:
        c += '  ; ' + NAMES[int(c, 16)]
    return '  %x  %s %s' % (i.address, i.mnemonic, c)


def cstr(a):
    try:
        b = pe.read(a, 64).split(b'\0')[0]
    except ValueError:
        return None
    if len(b) >= 4 and all(32 <= c < 127 or c >= 0x80 for c in b):
        return b.decode('cp932', 'replace')
    return None


def summary(f):
    calls, imps, strs, glob = [], set(), [], collections.Counter()
    for i in insns(f, SIZE.get(f) or 0x200):
        if i.mnemonic == 'call':
            if i.op_str.startswith('0x'):
                calls.append(int(i.op_str, 16))
            elif i.op_str.startswith('dword ptr [0x'):
                imps.add(pe.imports.get(int(i.op_str[11:-1], 16), '?'))
        for m in re.findall(r'0x([0-9a-f]{6,7})\b', i.op_str):
            a = int(m, 16)
            if 0x4b2000 <= a < 0x860000:
                s = cstr(a)
                if s and i.mnemonic in ('push', 'mov', 'lea'):
                    strs.append(s)
                elif a >= 0x4c7000:
                    glob[a] += 1
    return calls, imps, strs, glob


def main():
    cmd, args = sys.argv[1], [a for a in sys.argv[2:] if not a.startswith('--')]
    if cmd == 'dis':
        for arg in args:
            a, _, n = arg.partition(':')
            a = int(a, 16)
            print('== %x %s' % (a, NAMES.get(a, '')))
            for i in insns(a, int(n, 16) if n else (SIZE.get(a) or 0x400)):
                print(fmt(i))
                if i.mnemonic == 'ret' and '--all' not in sys.argv and not n:
                    break
    elif cmd == 'xref':
        want = ['0x%x' % int(a, 16) for a in args]
        for i in text():
            if any(re.search(re.escape(w) + r'\b', i.op_str) for w in want):
                print('%08x  in %08x  %s %s' % (i.address, containing(i.address), i.mnemonic, i.op_str))
    elif cmd == 'calls':
        callee, win = '0x%x' % int(args[0], 16), []
        for i in text():
            win = (win + [i])[-9:]
            if i.mnemonic == 'call' and i.op_str == callee:
                print('-- call at %x (in %x)' % (i.address, containing(i.address)))
                for j in win[:-1]:
                    print('   ' + fmt(j))
    elif cmd == 'profile':
        for arg in args:
            f = int(arg, 16)
            calls, imps, strs, glob = summary(f)
            deep = set(imps)
            for c in dict.fromkeys(calls):
                if c in SIZE:
                    _, ci, cs, _ = summary(c)
                    deep |= ci
                    strs += cs
            print('== %08x %s size %d' % (f, NAMES.get(f, ''), SIZE.get(f, 0)))
            print('   calls:', ' '.join('%x%s' % (c, '=' + NAMES[c] if c in NAMES else '') for c in dict.fromkeys(calls)))
            print('   imports:', ', '.join(sorted(x.split('!')[1] for x in deep if '!' in x)))
            print('   strings:', strs[:8])
            print('   globals:', ' '.join('%x' % a for a, _ in glob.most_common(10)))
    elif cmd == 'reach':
        stop = {0x41af20, 0x41ab70, 0x41ab00}          # the scheduler: every task reaches all others
        for arg in args:
            root = int(arg, 16)
            seen, todo = {root}, [root]
            while todo:
                for c in CALLEES.get(todo.pop(), []):
                    if c not in seen and c not in stop:
                        seen.add(c)
                        todo.append(c)
            mods = collections.Counter(n for f in seen for lo, hi, n in MODULES if lo <= f < hi)
            print('== %08x: %d functions (%d game)' % (root, len(seen), sum(0x41d000 <= f < 0x483800 for f in seen)))
            print('   modules:', ', '.join('%s %d' % kv for kv in mods.most_common()))
            print('   named:', ', '.join(sorted(NAMES[f] for f in seen if f in NAMES))[:400])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
