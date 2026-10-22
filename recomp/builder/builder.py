"""ddg-recomp builder: makes the game package from the user's own perfect.exe.

Runs inside the builder kit (builder/make-kit.sh), which contains everything except the game:
a portable Python with capstone, the zig toolchain (clang and lld for any target), the translator
(recomp/*.py and its data) and, per target, the project's own code already compiled (host and port
objects, SDL3). Here the exe is translated into C (build_game.py), that C is compiled for the target
and linked with the kit's objects, and the result is packed like the manual build's package
(linux/package.sh, linux/build-desktop.sh). The game's code thus only ever exists on the user's
machine.

    ddg-builder [handheld|desktop] /path/to/perfect.exe [--out <folder>] [--jobs <n>] [--keep]
"""
import argparse, json, os, shutil, subprocess, sys, tarfile, time, zipfile
from concurrent.futures import ThreadPoolExecutor

KIT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RECOMP = os.path.join(KIT, 'recomp')
EXE_SUFFIX = '.exe' if os.name == 'nt' else ''


def fail(msg):
    print('\nerror: ' + msg, file=sys.stderr)
    sys.exit(1)


def zig():
    z = os.path.join(KIT, 'zig', 'zig' + EXE_SUFFIX)
    if not os.path.isfile(z):
        fail('zig not found at %s (incomplete builder folder?)' % z)
    return z


def expand(args, values):
    return [a.format(**values) for a in args]


def run(cmd, env):
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    return r.returncode, r.stdout


def translate(exe, game, env):
    print('== translating %s' % exe)
    e = dict(env, PERFECT_EXE=exe, RECOMP_GAME_DIR=game)
    e['PYTHONPATH'] = os.path.join(RECOMP, 'pylib')
    r = subprocess.run([sys.executable, os.path.join(RECOMP, 'build_game.py')], env=e)
    if r.returncode:
        fail('the translation failed (see the message above)')


def compile_game(t, game, objdir, values, env, jobs):
    srcs = sorted(f for f in os.listdir(game) if f.endswith('.c'))
    os.makedirs(objdir, exist_ok=True)
    print('== compiling the game code (%d files, %d at a time; the first run also prepares the C '
          'library for the target)' % (len(srcs), jobs))
    done = [0]
    t0 = time.time()

    def one(name):
        o = os.path.join(objdir, name[:-2] + '.o')
        cmd = [zig(), 'cc', '-target', t['zig_target']] + expand(t['game_cflags'], values) + \
              ['-I', RECOMP, '-I', game, '-c', os.path.join(game, name), '-o', o]
        rc, out = run(cmd, env)
        done[0] += 1
        print('   %2d/%d %s%s' % (done[0], len(srcs), name, '' if rc == 0 else '  FAILED'), flush=True)
        return o, rc, out

    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(one, srcs))
    for o, rc, out in results:
        if rc:
            print(out, file=sys.stderr)
            fail('compiling failed')
    print('   %.0f s' % (time.time() - t0))
    return [o for o, _, _ in results]


def link(t, objs, tdir, out, values, env):
    print('== linking')
    kit_objs = [os.path.join(tdir, 'obj', o) for o in t['objects']]
    cmd = [zig(), 'c++', '-target', t['zig_target']] + expand(t['ldflags'], values) + objs + kit_objs + \
          [os.path.join(tdir, 'libs', l) for l in t['libs']] + expand(t['ldlibs'], values) + ['-o', out]
    rc, text = run(cmd, env)
    if rc:
        print(text, file=sys.stderr)
        fail('linking failed')


def package(t, tdir, binary, outdir):
    """Writes t['package'] (zip or tar.gz) into outdir: entries [path, source, mode]; source is
    "@binary", a file of the target folder, or null for an empty file."""
    p = t['package']
    path = os.path.join(outdir, p['name'])
    entries = []
    for dst, src, mode in p['files']:
        if src == '@binary':
            data = open(binary, 'rb').read()
        elif src is None:
            data = b''
        else:
            data = open(os.path.join(tdir, src), 'rb').read()
        entries.append((dst, data, int(mode, 8)))
    if path.endswith('.zip'):
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            for dst, data, mode in entries:
                zi = zipfile.ZipInfo(dst, time.localtime()[:6])
                zi.compress_type = zipfile.ZIP_DEFLATED
                zi.create_system = 3                    # unix: keep the modes (executable scripts)
                zi.external_attr = (0o100000 | mode) << 16
                z.writestr(zi, data)
    else:
        import io
        with tarfile.open(path, 'w:gz') as tf:
            for dst, data, mode in entries:
                ti = tarfile.TarInfo(dst)
                ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
                tf.addfile(ti, io.BytesIO(data))
    return path


def main():
    sys.stdout.reconfigure(line_buffering=True)     # in order with the translator's messages
    targets = sorted(d for d in os.listdir(os.path.join(KIT, 'targets'))
                     if os.path.isfile(os.path.join(KIT, 'targets', d, 'target.json')))
    ap = argparse.ArgumentParser(prog='ddg-builder', description='Builds Densha de GO! FINAL (recompiled) from your perfect.exe.')
    default = 'handheld' if 'handheld' in targets else targets[0]
    ap.add_argument('target', nargs='?', help='what to build: %s (default: %s)' % (', '.join(targets), default))
    ap.add_argument('exe', nargs='?', help='your unprotected perfect.exe')
    ap.add_argument('--out', help='folder for the package (default: next to the builder)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2, help='files compiled at a time')
    ap.add_argument('--keep', action='store_true', help='keep the work folder (translated code, objects)')
    a = ap.parse_args()
    if a.target and a.target not in targets:       # only the exe given (e.g. dropped onto the launcher)
        if a.exe:
            fail('unknown target %s (%s)' % (a.target, ', '.join(targets)))
        a.target, a.exe = None, a.target
    a.target = a.target or default
    exe = a.exe
    if not exe:                                     # started without arguments (double-click)
        exe = input('Path of your perfect.exe (you can drag the file into this window): ').strip().strip('"\'')
    if not exe or not os.path.isfile(exe):
        fail('perfect.exe not found: %s' % exe)
    exe = os.path.abspath(exe)
    tdir = os.path.join(KIT, 'targets', a.target)
    t = json.load(open(os.path.join(tdir, 'target.json')))
    outdir = os.path.abspath(a.out or KIT)
    work = os.path.join(outdir, 'ddg-recomp-work-' + a.target)
    if os.path.isdir(work):
        shutil.rmtree(work)
    game, objdir = os.path.join(work, 'game'), os.path.join(work, 'obj')
    os.makedirs(game)
    env = dict(os.environ)
    # zig's cache (the target's C library, built on the first run) stays in the builder folder
    env['ZIG_GLOBAL_CACHE_DIR'] = os.path.join(KIT, 'cache', 'zig')
    env['ZIG_LOCAL_CACHE_DIR'] = os.path.join(work, 'zig-cache')
    values = {'target': tdir}
    print('ddg-recomp builder: %s (%s)' % (t['description'], a.target))
    t0 = time.time()
    try:
        translate(exe, game, env)
        objs = compile_game(t, game, objdir, values, env, max(1, a.jobs))
        binary = os.path.join(work, t['binary'])
        link(t, objs, tdir, binary, values, env)
        path = package(t, tdir, binary, outdir)
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)
    print('\ndone in %.0f s: %s' % (time.time() - t0, path))
    print(t['next_steps'])
    print('This package contains code translated from your perfect.exe: do not share it.')


if __name__ == '__main__':
    try:
        main()
    finally:
        if os.name == 'nt' and len(sys.argv) < 3 and sys.stdin.isatty():
            input('\nPress Enter to close.')   # double-clicked: keep the window open
