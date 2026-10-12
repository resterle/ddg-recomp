#!/usr/bin/env python3
"""Reads a RECOMP_PROFILE file (host/runtime.c) and prints the hottest functions per thread.

usage: tools/profile.py <profile.txt> <perfect_recomp binary of that run> [top N] [thread filter]
       PROFILE_LIBS=<dir>: copies of the device's shared libraries (same file names) to symbolise
       their samples too (exported symbols only, e.g. libc's memcpy)
       PROFILE_FUNCS=1: list translated game functions (F_xxxxxxxx) by name instead of as one entry
       PROFILE_CALLERS=1: samples in shared libraries are listed with their caller (from the link
       register, arm64 profiles; exact for leaf functions such as memcpy)

Samples in the executable are symbolised with llvm-symbolizer (llvm-symbolizer, llvm-symbolizer-16,
or the one in the ddg-recomp-cross Docker image); other samples are counted per library.
"""
import collections, os, shutil, struct, subprocess, sys


def load_segments(path):
    """PT_LOAD segments of an ELF64 file: (offset, vaddr, filesz)."""
    with open(path, 'rb') as f:
        d = f.read(1 << 16)
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    segs = []
    for i in range(phnum):
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz = struct.unpack_from('<IIQQQQ', d, phoff + i * phentsize)
        if p_type == 1:
            segs.append((p_offset, p_vaddr, p_filesz))
    return segs


def symbolizer(binary, addrs):
    if not os.path.exists(binary):
        return {}
    cmd = None
    for name in ('llvm-symbolizer', 'llvm-symbolizer-16'):
        if shutil.which(name):
            cmd = [name, '--obj=' + binary, '--functions=linkage', '--no-inlines']
            break
    if cmd is None:     # the cross-build image has one
        d = os.path.dirname(os.path.abspath(binary))
        cmd = ['docker', 'run', '--rm', '-i', '-v', d + ':/b', 'ddg-recomp-cross', 'llvm-symbolizer-16',
               '--obj=/b/' + os.path.basename(binary), '--functions=linkage', '--no-inlines']
    out = subprocess.run(cmd, input=''.join('0x%x\n' % a for a in addrs), capture_output=True, text=True).stdout
    names = [blk.split('\n')[0] for blk in out.strip().split('\n\n')]
    return dict(zip(addrs, names))


def main():
    prof, binary = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    only = sys.argv[4] if len(sys.argv) > 4 else None
    maps, threads, samples, main_tid = [], {}, [], None
    for line in open(prof):
        k, _, rest = line.partition(' ')
        if k == 'map':
            f = rest.split()
            lo, hi = (int(x, 16) for x in f[0].split('-'))
            maps.append((lo, hi, int(f[2], 16), f[5] if len(f) > 5 else '?'))
        elif k == 'thread':
            tid, _, name = rest.strip().partition(' ')
            threads[int(tid)] = name
        elif k == 'main':
            main_tid = int(rest)
        elif k == 's':
            f = rest.split()
            samples.append((int(f[0]), int(f[1], 16), int(f[2], 16) if len(f) > 2 else 0))
    if main_tid in threads:
        threads[main_tid] = 'game (main)'
    libdir = os.environ.get('PROFILE_LIBS')
    files = {}                                       # mapped file name -> local ELF to symbolise with
    for _, _, _, path in maps:
        base = os.path.basename(path)
        if base.startswith('perfect_recomp'):
            files[base] = binary
        elif libdir and os.path.exists(os.path.join(libdir, base)):
            files[base] = os.path.join(libdir, base)
    segs = {b: load_segments(f) for b, f in files.items()}

    def where(pc):
        for lo, hi, off, path in maps:
            if lo <= pc < hi:
                base = os.path.basename(path)
                for s_off, s_vaddr, s_size in segs.get(base, []):
                    if s_off <= off + (pc - lo) < s_off + s_size:
                        return (base, s_vaddr + off + (pc - lo) - s_off)
                return (base, None)
        return ('?', None)

    callers = os.environ.get('PROFILE_CALLERS') == '1'
    funcs = os.environ.get('PROFILE_FUNCS') == '1'
    located = [(tid, where(pc), where(lr) if lr else ('?', None)) for tid, pc, lr in samples]
    names = {}
    for base, f in files.items():
        addrs = sorted({v for _, (b, v), _ in located if b == base and v is not None} |
                       {v for _, _, (b, v) in located if callers and b == base and v is not None})
        if addrs:
            names[base] = symbolizer(f, addrs)

    def label(base, v):
        sym = names.get(base, {}).get(v) if v is not None else None
        if sym and sym != '??':
            name = sym if base.startswith('perfect_recomp') else '%s [%s]' % (sym, base)
        else:
            name = '[' + base + ']'
        if not funcs and (name.startswith('F_') or name.startswith('A_')):
            name = '(translated game code)'
        return name

    per_thread = collections.defaultdict(collections.Counter)
    for tid, (base, v), (lbase, lv) in located:
        name = label(base, v)
        if callers and not base.startswith('perfect_recomp') and lbase.startswith('perfect_recomp'):
            name += '  <- ' + label(lbase, lv)
        tname = threads.get(tid, 'thread %d' % tid)
        if tname == 'perfect_recomp':
            tname = 'other threads'
        per_thread[tname][name] += 1
    total = len(samples)
    print('%d samples (1 per ms of CPU time)' % total)
    for tname, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        if only and only not in tname:
            continue
        n = sum(c.values())
        print('\n%s: %d samples (%.1f%%)' % (tname, n, 100.0 * n / total))
        for name, k in c.most_common(top):
            print('  %6.1f%%  %s' % (100.0 * k / n, name))


if __name__ == '__main__':
    main()
