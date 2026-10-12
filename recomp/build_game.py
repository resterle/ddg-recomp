"""Translate the whole program (minus the native modules) into C for the host build.

Output in build/game/: game_NN.c (translated functions), dispatch.c (address table),
native_stubs.c (one trap per native boundary function, to be replaced in host/native.c),
imports.c (IAT layout for the Win32 shim).
"""
import os, re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from pe import PE
from x86lift import Lifter, LiftError
import plan
import exepatch

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get('PERFECT_EXE', os.path.join(HERE, 'perfect.exe'))   # the exe the game will run with
OUT = os.path.join(HERE, 'build', 'game')
CHUNKS = 16


def true_entries(F):
    ent = set()
    for l in open(os.path.join(HERE, 'data', 'functions.tsv')).read().splitlines()[1:]:
        ent.add(int(l.split('\t')[0], 16))
    for f, d in F.items():
        ent.update(d['callees'])
        if d['dref']:
            ent.add(f)
    return ent


def callee_pop(pe, addr, size):
    """Bytes a function pops on return (ret N), from its own code."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for ins in md.disasm(pe.read(addr, max(size, 16)), addr):
        if ins.mnemonic == 'ret':
            return int(ins.op_str, 16) if ins.op_str else 0
    return 0


def main():
    try:
        image = exepatch.load(EXE)          # unprotected perfect.exe with DDGModernizer's patches
    except exepatch.ExeError as e:
        sys.exit(str(e))
    pe = PE(image)
    F = plan.load()
    reach = [int(x, 16) for x in open(os.path.join(plan.NOTES, 'reach_all.txt')).read().split()]
    # function pointers: Ghidra's data references plus any aligned 32-bit value in the data
    # sections that equals a function entry (raw tables Ghidra did not type, e.g. D3DX's
    # CPU-dispatch table)
    raw = set()
    for sec in pe.sections:
        b = sec['raw']
        step = 1 if sec['name'] == '.text' else 4      # code: immediates at any offset
        for o in range(0, len(b) - 3, step):
            v = struct.unpack_from('<I', b, o)[0]
            if v in F:
                raw.add(v)
    ptrs = [f for f in set(reach) | raw if (F.get(f, {}).get('dref') or f in raw) and not plan.native_name(f)]
    # Pointers in data to code Ghidra never made a function: accept plausible function starts
    # (16-byte aligned, preceded by padding or a return). Plus addresses the runtime reported as
    # unknown call targets (build/discovered.txt).
    text = next(x for x in pe.sections if x['name'] == '.text')
    tlo, thi = text['va'], text['va'] + len(text['raw'])
    extra = set()
    for sec in pe.sections:
        if sec['name'] == '.text':
            continue
        b = sec['raw']
        for o in range(0, len(b) - 3, 4):
            v = struct.unpack_from('<I', b, o)[0]
            if tlo < v < thi and v not in F and v % 16 == 0 and pe.read(v - 1, 1)[0] in (0xcc, 0x90, 0xc3):
                extra.add(v)
    disc = os.path.join(HERE, 'build', 'discovered.txt')
    if os.path.exists(disc):
        extra |= {v for v in (int(x, 16) for x in open(disc).read().split() if x.strip()) if tlo <= v < thi}
    extra = {x for x in extra if not plan.native_name(x)}
    print('extra roots (raw pointers / discovered): %d' % len(extra))
    # roots: CRT entry, plus what the native WinMain / message loop call into
    T, N = plan.partition(F, [0x4a45af, 0x407170, 0x401430, 0x478ed0, 0x41af10, 0x409060], ptrs)
    native = {f: 'N_%08x' % f for f in N}
    # functions in native ranges that are only reached through pointers (static initialisers,
    # callbacks): they need a native entry too
    for f in set(reach) | raw:
        if (F.get(f, {}).get('dref') or f in raw) and plan.native_name(f):
            native.setdefault(f, 'N_%08x' % f)
    for f in plan.NATIVE_FUNCS:
        native[f] = 'N_%08x' % f
    entries = true_entries(F) | set(native) | extra
    lifter = Lifter(pe, entries, native)
    lifter.imm_overrides = dict(plan.IMM_OVERRIDES)
    lifter.native_pred = lambda a: plan.native_name(a) is not None

    os.makedirs(OUT, exist_ok=True)
    ok, failed = [], {}
    todo = sorted(T | extra)
    done = set()
    chunks = [[] for _ in range(CHUNKS)]
    while todo:
        f = todo.pop()
        if f in done or f in native:
            continue
        done.add(f)
        try:
            src, _ = lifter.function(f)
        except LiftError as e:
            failed[f] = str(e)
            continue
        except Exception as e:
            failed[f] = 'internal %s: %s' % (type(e).__name__, e)
            continue
        ok.append(f)
        chunks[(f >> 12) % CHUNKS].append(src)
        # anything the translation calls that is not yet known (tail calls, fall-through); inlined
        # callees too, so that they stay reachable through the dispatch table (indirect calls)
        for t in re.findall(r'\bF_([0-9a-f]{8})\b|inlined ([0-9a-f]{8})', src):
            t = t[0] or t[1]
            t = int(t, 16)
            if t not in done and t not in native and not plan.native_name(t):
                todo.append(t)

    referenced_all = set()
    for c in chunks:
        for src in c:
            referenced_all |= {int(t, 16) for t in re.findall(r'\bF_([0-9a-f]{8})\b', src)}
    names = sorted(set(ok) | set(failed) | (referenced_all - set(native)))
    decl = ''.join('RtRet F_%08x(RT_PARAMS);\n' % f for f in names) + ''.join('void %s(void);\n' % n for n in sorted(native.values()))
    open(os.path.join(OUT, 'decls.h'), 'w').write('#pragma once\n#include "recomp_rt.h"\n' + decl)
    for i, c in enumerate(chunks):
        open(os.path.join(OUT, 'game_%02d.c' % i), 'w').write('#include "decls.h"\n\n' + '\n'.join(c))
    # functions that failed translation: trap with the reason
    fails = ''.join('RtRet F_%08x(RT_PARAMS) { RT_Fatal("not translated: %s", 0x%08xu); RT_RET(); }\n'
                    % (f, failed[f].replace('"', "'").replace('%', '%%')[:80], f) for f in sorted(failed))
    referenced = set()
    for c in chunks:
        for src in c:
            referenced |= {int(t, 16) for t in re.findall(r'\bF_([0-9a-f]{8})\b', src)}
    missing = sorted(referenced - set(ok) - set(failed))
    fails += ''.join('RtRet F_%08x(RT_PARAMS) { RT_Fatal("not translated", 0x%08xu); RT_RET(); }\n' % (f, f) for f in missing)
    open(os.path.join(OUT, 'untranslated.c'), 'w').write('#include "decls.h"\n' +
                                                          fails)
    # dispatch table (sorted by address)
    translated = sorted(set(ok) | set(failed) | set(missing))
    table = sorted([(f, 'A_%08x' % f) for f in translated] + [(f, n) for f, n in native.items()])
    # adapters from the R-based void(void) convention (indirect calls, rt_call) to RtRet F_x(...)
    adapters = ''.join(lifter.adapter(f, f in ok) + '\n' for f in translated)
    lines = ['#include "decls.h"', adapters,
             'typedef struct { uint32_t addr; RecompFn fn; } RtEntry;', 'const RtEntry RT_TABLE[] = {']
    lines += ['    {0x%08xu, %s},' % (a, n) for a, n in table]
    lines += ['};', 'const int RT_TABLE_N = %d;' % len(table)]
    open(os.path.join(OUT, 'dispatch.c'), 'w').write('\n'.join(lines) + '\n')
    # native boundary: name, callee-pop bytes, which module
    nat = ['// Native boundary functions: address, bytes popped by the original (ret N), module.',
           '#include "decls.h"', 'typedef struct { uint32_t addr; uint32_t pop; const char *module; } NativeInfo;',
           'const NativeInfo RT_NATIVE[] = {']
    for f in sorted(native):
        nat.append('    {0x%08xu, %d, "%s"},' % (f, callee_pop(pe, f, F.get(f, {}).get('size', 64)), plan.native_name(f)))
    nat += ['};', 'const int RT_NATIVE_N = %d;' % len(native)]
    # weak defaults, overridden by host/native.cpp
    nat.append('#include <stdio.h>')
    nat.append('#include <stdlib.h>')
    nat.append('#define ARG(i) RD32(R.esp + 4 + 4 * (i))')
    for f in sorted(native):
        mod = plan.native_name(f)
        pop = callee_pop(pe, f, F.get(f, {}).get('size', 64))
        if mod in ('Sound', 'Movie', 'Input', 'Wave'):
            nat.append('__attribute__((weak)) void N_%08x(void) { static unsigned n; n++; if (n == 1 || (getenv("RECOMP_TRACE_STUBS") && !(n & (n - 1)))) '
                       'fprintf(stderr, "stub %s %08x (call %%u from %%08x)\\n", n, RD32(R.esp)); '
                       'R.eax = 1; R.esp += 4 + %d; }' % (f, mod, f, pop))
        else:
            nat.append('__attribute__((weak)) void N_%08x(void) { RT_Fatal("native not implemented (%s)", 0x%08xu); }' % (f, mod, f))
    open(os.path.join(OUT, 'native_info.c'), 'w').write('\n'.join(nat) + '\n')
    # imports: IAT slot -> name
    imp = ['#include <stdint.h>', 'typedef struct { uint32_t iat; const char *name; } ImportSlot;', 'const ImportSlot RT_IMPORTS[] = {']
    imp += ['    {0x%08xu, "%s"},' % (a, n) for a, n in sorted(pe.imports.items())]
    imp += ['};', 'const int RT_IMPORTS_N = %d;' % len(pe.imports)]
    open(os.path.join(OUT, 'exe_info.c'), 'w').write(exepatch.c_table(image))
    print('exe image hash %016x' % exepatch.image_hash(image))
    open(os.path.join(OUT, 'imports.c'), 'w').write('\n'.join(imp) + '\n')
    print('translated %d functions, failed %d, missing %d, native %d' % (len(ok), len(failed), len(missing), len(native)))
    import collections
    print('failures:', collections.Counter(v.split(' at ')[0] for v in failed.values()).most_common(10))


if __name__ == '__main__':
    main()
