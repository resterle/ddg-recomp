#!/usr/bin/env python3
"""Makes the builder kit: a download that builds the game package from the user's perfect.exe
without Docker, Python or a compiler on their machine, and without containing anything of the
game (builder/builder.py does the translation and the game's compile on the user's machine).

    builder/make-kit.py [linux-x64] [windows-x64] [macos-arm64] [macos-x64] [linux-arm64]

Run on an x86-64 Linux machine with Docker (the handheld's SDL3 comes from the ddg-recomp-arm64
image, linux/sysroot-arm64.sh; the desktop's from ddg-recomp-desktop). Downloads zig and a
portable CPython (python-build-standalone) per host into build/kit-cache, compiles the host and
port objects once per target with zig (make hostobjs), and writes
build/kit/ddg-recomp-builder-<host>.(tar.xz|zip).
"""
import glob, json, os, shutil, subprocess, sys, tarfile, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(HERE)
ROOT = os.path.dirname(RECOMP)
CACHE = os.path.join(RECOMP, 'build', 'kit-cache')
OUT = os.path.join(RECOMP, 'build', 'kit')

ZIG_VERSION = '0.14.1'
PY_RELEASE, PY_VERSION = '20250612', '3.12.11'
CAPSTONE = '5.0.9'          # requirements.txt

# host: (zig archive name part, python-build-standalone triple, pip platforms for capstone, archive)
HOSTS = {
    'linux-x64': ('x86_64-linux', 'x86_64-unknown-linux-gnu', ['manylinux2014_x86_64', 'manylinux1_x86_64'], 'tar.xz'),
    'linux-arm64': ('aarch64-linux', 'aarch64-unknown-linux-gnu', ['manylinux2014_aarch64'], 'tar.xz'),
    'windows-x64': ('x86_64-windows', 'x86_64-pc-windows-msvc', ['win_amd64'], 'zip'),
    'macos-arm64': ('aarch64-macos', 'aarch64-apple-darwin', ['macosx_11_0_arm64'], 'tar.xz'),
    'macos-x64': ('x86_64-macos', 'x86_64-apple-darwin', ['macosx_10_9_x86_64'], 'tar.xz'),
}

PGO = ['-fprofile-use={target}/perfect.profdata', '-Wno-profile-instr-unprofiled',
       '-Wno-profile-instr-out-of-date', '-Wno-backend-plugin']

# Per target: how the kit's objects are made (make hostobjs with these variables) and the
# target.json builder.py reads. game_cflags follow the Makefile's rule for build/game/*.c.
TARGETS = {
    'handheld': {
        'description': 'R36H/R36S and other ArkOS-style handhelds (aarch64 Linux, OpenGL ES)',
        'zig_target': 'aarch64-linux-gnu.2.28',
        'renderer': 'gl', 'tune': '-mcpu=cortex_a35', 'membase': '0x400000000', 'pgo': True,
        'sdl': 'build/sysroot-arm64/opt/sdl3',
        'binary': 'perfect_recomp',
        'files': {'ddg-recomp.sh': 'linux/ddg-recomp.sh', 'graphics.ini': 'linux/graphics.ini',
                  'README.txt': 'linux/README.txt', 'perfect.profdata': 'linux/perfect.profdata'},
        'package': {'name': 'ddg-recomp-arm64.zip', 'files': [
            ['Densha de GO! FINAL.sh', 'ddg-recomp.sh', '755'],
            ['ddg-recomp/perfect_recomp', '@binary', '755'],
            ['ddg-recomp/libs/libSDL3.so.0', 'libs/libSDL3.so.0', '644'],
            ['ddg-recomp/conf/graphics.ini', 'graphics.ini', '644'],
            ['ddg-recomp/README.txt', 'README.txt', '644'],
            ['ddg-recomp/gamedata/PUT_cddata_AND_perfect.exe_HERE', None, '644'],
        ]},
        'next_steps': 'Copy the contents of the zip into the SD card\'s "ports" folder, put cddata and '
                      'perfect.exe into ports/ddg-recomp/gamedata/ and restart EmulationStation.',
    },
    'desktop': {
        'description': 'x86-64 Linux desktop (Vulkan)',
        'zig_target': 'x86_64-linux-gnu.2.35',
        'renderer': 'gpu', 'tune': '', 'membase': '0x400000000', 'pgo': False,
        'sdl': 'build/sysroot-x64/opt/sdl3',
        'binary': 'perfect_recomp',
        'files': {'README.txt': 'linux/README-desktop.txt'},
        'package': {'name': 'ddg-recomp-linux-x64.tar.gz', 'files': [
            ['ddg-recomp/perfect_recomp', '@binary', '755'],
            ['ddg-recomp/libs/libSDL3.so.0', 'libs/libSDL3.so.0', '644'],
            ['ddg-recomp/README.txt', 'README.txt', '644'],
        ]},
        'next_steps': 'Unpack it and run ddg-recomp/perfect_recomp "/path/to/the game folder" (the folder '
                      'with cddata and perfect.exe).',
    },
}

# the translator: what build_game.py imports and reads
TRANSLATOR = ['build_game.py', 'exepatch.py', 'pe.py', 'plan.py', 'x86lift.py', 'recomp_rt.h',
              'data/funcs.tsv', 'data/functions.tsv', 'data/reach_all.txt', 'data/ddgmodernizer-final.txt',
              'data/ddgmodernizer-LICENSE.md', 'build/discovered.txt', 'builder/builder.py']


def fetch(url, name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        print('downloading', url)
        tmp = path + '.part'
        with urllib.request.urlopen(url) as r, open(tmp, 'wb') as f:
            shutil.copyfileobj(r, f)
        os.rename(tmp, path)
    return path


def unpack(archive, dest):
    """Unpacks into dest (once); returns dest."""
    if os.path.isdir(dest):
        return dest
    tmp = dest + '.part'
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    if archive.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
    else:
        subprocess.run(['tar', '-xf', archive, '-C', tmp], check=True)
    os.rename(tmp, dest)
    return dest


def get_zig(host):
    part = HOSTS[host][0]
    ext = 'zip' if 'windows' in part else 'tar.xz'
    name = 'zig-%s-%s' % (part, ZIG_VERSION)
    a = fetch('https://ziglang.org/download/%s/%s.%s' % (ZIG_VERSION, name, ext), name + '.' + ext)
    return os.path.join(unpack(a, os.path.join(CACHE, 'unpacked', name)), name)


def get_python(host):
    triple = HOSTS[host][1]
    name = 'cpython-%s+%s-%s-install_only.tar.gz' % (PY_VERSION, PY_RELEASE, triple)
    a = fetch('https://github.com/astral-sh/python-build-standalone/releases/download/%s/%s' % (PY_RELEASE, name.replace('+', '%2B')), name)
    return os.path.join(unpack(a, os.path.join(CACHE, 'unpacked', name[:-7])), 'python')


def get_capstone(host):
    d = os.path.join(CACHE, 'capstone-' + host)
    if not glob.glob(os.path.join(d, '*.whl')):
        cmd = [sys.executable, '-m', 'pip', 'download', 'capstone==' + CAPSTONE, '--only-binary=:all:', '--no-deps',
               '--python-version', '3.12', '-d', d, '-q']
        for p in HOSTS[host][2]:
            cmd += ['--platform', p]
        subprocess.run(cmd, check=True)
    return glob.glob(os.path.join(d, '*.whl'))[0]


def sdl_desktop():
    dest = os.path.join(RECOMP, 'build', 'sysroot-x64', 'opt')
    if os.path.isdir(os.path.join(dest, 'sdl3')):
        return
    image = 'ddg-recomp-desktop'
    if subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode:
        subprocess.run(['docker', 'build', '-t', image, os.path.join(RECOMP, 'linux', 'desktop')], check=True)
    os.makedirs(dest, exist_ok=True)
    cid = subprocess.run(['docker', 'create', image, 'true'], capture_output=True, text=True, check=True).stdout.strip()
    try:
        subprocess.run(['docker', 'cp', cid + ':/opt/sdl3', dest], check=True)
    finally:
        subprocess.run(['docker', 'rm', cid], capture_output=True)


def build_target(name, t, zig):
    """Compiles the host and port objects for a target; returns the target folder for the kit."""
    if name == 'handheld':
        subprocess.run(['sh', os.path.join(RECOMP, 'linux', 'sysroot-arm64.sh')], check=True)
    else:
        sdl_desktop()
    sdl = os.path.join(RECOMP, t['sdl'])
    obj = os.path.join('build', 'kit-obj', name)
    shutil.rmtree(os.path.join(RECOMP, obj), ignore_errors=True)
    cc = '%s cc -target %s' % (zig, t['zig_target'])
    pgo = ' '.join(PGO).format(target=os.path.join(RECOMP, 'linux')) if t['pgo'] else ''
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(CACHE, 'zig-global'))
    print('== %s: host and port objects' % name)
    subprocess.run(['make', '-C', RECOMP, '-j%d' % (os.cpu_count() or 4), 'hostobjs', 'RENDERER=' + t['renderer'],
                    'OBJ=' + obj, 'CC=' + cc, 'CXX=' + cc.replace(' cc ', ' c++ '), 'TUNE=' + t['tune'],
                    'MEMBASE=' + t['membase'], 'PGOFLAGS=' + pgo, 'SDL_PREFIX=' + sdl], check=True, env=env)
    tdir = os.path.join(OUT, 'targets', name)
    shutil.rmtree(tdir, ignore_errors=True)
    objs = []
    for o in sorted(glob.glob(os.path.join(RECOMP, obj, '*', '*.o'))):
        rel = os.path.relpath(o, os.path.join(RECOMP, obj)).replace(os.sep, '/')
        os.makedirs(os.path.join(tdir, 'obj', os.path.dirname(rel)), exist_ok=True)
        shutil.copy(o, os.path.join(tdir, 'obj', rel))
        objs.append(rel)
    os.makedirs(os.path.join(tdir, 'libs'))
    shutil.copy(os.path.realpath(os.path.join(sdl, 'lib', 'libSDL3.so.0')), os.path.join(tdir, 'libs', 'libSDL3.so.0'))
    for dst, src in t['files'].items():
        shutil.copy(os.path.join(RECOMP, src), os.path.join(tdir, dst))
    tune = [t['tune']] if t['tune'] else []
    pgo = PGO if t['pgo'] else []
    # the game code: the Makefile's rule for build/game/*.c (OPT -O2, LTO, -g0 -w -fno-strict-aliasing)
    cfg = {
        'description': t['description'],
        'zig_target': t['zig_target'],
        'game_cflags': pgo + tune + ['-O2', '-DRT_MEM_BASE=' + t['membase'], '-flto=thin', '-g0', '-w', '-fno-strict-aliasing'],
        'ldflags': pgo + tune + ['-flto=thin'],
        'objects': objs,
        'libs': ['libSDL3.so.0'],
        'ldlibs': ['-pthread', '-ldl', '-Wl,-rpath,$ORIGIN/libs'],
        'binary': t['binary'],
        'package': t['package'],
        'next_steps': t['next_steps'],
    }
    json.dump(cfg, open(os.path.join(tdir, 'target.json'), 'w'), indent=1)
    return tdir


LAUNCH_SH = '''#!/bin/sh
# Builds Densha de GO! FINAL (recompiled) from your perfect.exe; see README.txt.
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/python/bin/python3" "$here/recomp/builder/builder.py" "$@"
'''
LAUNCH_BAT = '''@echo off\r
rem Builds Densha de GO! FINAL (recompiled) from your perfect.exe; see README.txt.\r
rem Drag perfect.exe onto this file, or run: ddg-builder [handheld^|desktop] C:\\path\\to\\perfect.exe\r
"%~dp0python\\python.exe" "%~dp0recomp\\builder\\builder.py" %*\r
'''


# zig's C headers needed for the targets (aarch64 and x86-64 glibc); it ships them for ~60
ZIG_INCLUDE = {'aarch64-linux-any', 'aarch64-linux-gnu', 'x86-linux-any', 'x86_64-linux-gnu', 'any-linux-any', 'generic-glibc'}


def trim(kit, host):
    """Leaves out what the builder never uses: other targets' C libraries and headers, Tcl/Tk,
    IDLE, pip; strips the Linux Python's debug symbols (most of its size)."""
    libc = os.path.join(kit, 'zig', 'lib', 'libc')
    for d in os.listdir(os.path.join(libc, 'include')):
        if d not in ZIG_INCLUDE:
            shutil.rmtree(os.path.join(libc, 'include', d))
    for d in ('musl', 'mingw', 'wasi', 'darwin'):
        shutil.rmtree(os.path.join(libc, d), ignore_errors=True)
    py = os.path.join(kit, 'python')
    lib = os.path.join(py, 'Lib') if host.startswith('windows') else os.path.join(py, 'lib', 'python3.12')
    for d in ('idlelib', 'ensurepip', 'tkinter', 'turtledemo', 'test', 'pydoc_data'):
        shutil.rmtree(os.path.join(lib, d), ignore_errors=True)
    for d in glob.glob(os.path.join(py, 'lib', 't[ck]*8.6')) + glob.glob(os.path.join(py, 'lib', 'Tix*')) + \
             glob.glob(os.path.join(py, 'tcl')) + [os.path.join(py, 'share')]:
        shutil.rmtree(d, ignore_errors=True)
    if host.startswith('linux') and shutil.which('strip'):
        # only the library: the stripped python3.12 executable no longer resolves its symbols
        for f in glob.glob(os.path.join(py, 'lib', 'libpython*.so*')):
            if not os.path.islink(f) and open(f, 'rb').read(4) == b'\x7fELF':
                subprocess.run(['strip', '--strip-debug', f], check=True)


def assemble(host, targets):
    windows = host.startswith('windows')
    name = 'ddg-recomp-builder-' + host
    kit = os.path.join(OUT, name)
    shutil.rmtree(kit, ignore_errors=True)
    os.makedirs(kit)
    print('== kit', name)
    shutil.copytree(get_python(host), os.path.join(kit, 'python'), symlinks=True)
    shutil.copytree(get_zig(host), os.path.join(kit, 'zig'), symlinks=True)
    trim(kit, host)
    for f in TRANSLATOR:
        os.makedirs(os.path.join(kit, 'recomp', os.path.dirname(f)), exist_ok=True)
        shutil.copy(os.path.join(RECOMP, f), os.path.join(kit, 'recomp', f))
    with zipfile.ZipFile(get_capstone(host)) as z:        # a wheel is a zip of the package folder
        z.extractall(os.path.join(kit, 'recomp', 'pylib'))
    for tname, tdir in targets.items():
        shutil.copytree(tdir, os.path.join(kit, 'targets', tname))
    shutil.copy(os.path.join(HERE, 'README.txt'), os.path.join(kit, 'README.txt'))
    if windows:
        open(os.path.join(kit, 'ddg-builder.bat'), 'w', newline='').write(LAUNCH_BAT)
    else:
        p = os.path.join(kit, 'ddg-builder')
        open(p, 'w').write(LAUNCH_SH)
        os.chmod(p, 0o755)
    archive = os.path.join(OUT, name + '.' + HOSTS[host][3])
    if os.path.exists(archive):
        os.remove(archive)
    if windows:
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for d, _, files in os.walk(kit):
                for f in files:
                    p = os.path.join(d, f)
                    z.write(p, os.path.relpath(p, OUT))
    else:
        subprocess.run(['tar', '-C', OUT, '-cJf', archive, name], check=True, env=dict(os.environ, XZ_OPT='-T0'))
    print('   %s (%.0f MB)' % (archive, os.path.getsize(archive) / 1e6))


def main():
    hosts = sys.argv[1:] or ['linux-x64']
    for h in hosts:
        if h not in HOSTS:
            sys.exit('unknown host %s (%s)' % (h, ', '.join(HOSTS)))
    zig = os.path.join(get_zig('linux-x64'), 'zig')         # this machine's zig: builds the kit's objects
    targets = {n: build_target(n, t, zig) for n, t in TARGETS.items()}
    for h in hosts:
        assemble(h, targets)


if __name__ == '__main__':
    main()
