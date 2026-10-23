"""Builds the translator runtime the RECOMP_JIT program carries (host/pyembed.cpp).

    <python> host/mkpyruntime.py <python-build-standalone "full" dir> <capstone wheel> <out dir>

Run it with that distribution's own python (install/bin/python3), so the standard library modules
it finds are the ones of the libpython the program links. Writes to <out dir>:

  pyconfig.c    the built-in module table (_PyImport_Inittab): the modules the start-up of Python
                needs plus the extension modules the translator imports, so that libpython3.12.a
                links without the others (OpenSSL, SQLite, Tk, ...)
  link.txt      what linking libpython3.12.a with those modules needs (its static libraries,
                system libraries and frameworks), from the distribution's PYTHON.json
  runtime.bin   records (<u32 name length> <name> <u32 data length> <data>, little endian) with
                stdlib.zip (the pure Python modules the translator imports), the translator
                (recomp/*.py and the data it reads) and capstone (its Python package and
                libcapstone.so); pyembed.cpp unpacks them into the cache on the first start
"""
import io, json, modulefinder, os, re, struct, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(HERE)

TRANSLATOR = ['build_game.py', 'exepatch.py', 'pe.py', 'plan.py', 'x86lift.py', 'recomp_rt.h',
              'data/funcs.tsv', 'data/functions.tsv', 'data/reach_all.txt', 'data/ddgmodernizer-final.txt',
              'build/discovered.txt']

# built-in modules the interpreter's own start-up needs (CPython 3.12 Modules/Setup.bootstrap and
# the fixed entries of config.c)
BOOTSTRAP = {'posix', 'errno', 'pwd', '_sre', '_codecs', '_weakref', '_functools', '_operator', '_collections',
             '_abc', 'itertools', 'atexit', '_signal', '_stat', 'time', '_thread', '_locale', '_io',
             'faulthandler', '_tracemalloc', '_symtable', '_typing', 'marshal', '_imp', '_ast', '_tokenize',
             'builtins', 'sys', 'gc', '_warnings', '_string'}
# built-in (C) modules the translator imports, measured: `python3 -X importtime build_game.py`
# (modulefinder also finds imports that never run, e.g. _ssl and _tkinter, which need OpenSSL and Tk)
USED = {'_abc', '_ast', '_bisect', '_bz2', '_codecs', '_collections', '_ctypes', '_functools', '_heapq', '_io',
        '_locale', '_lzma', '_opcode', '_operator', '_posixsubprocess', '_random', '_sha2', '_signal', '_sre',
        '_stat', '_struct', '_tokenize', '_typing', 'binascii', 'errno', 'fcntl', 'itertools', 'marshal', 'math',
        'posix', 'select', 'time', 'zlib'}
# what runs the translator (pyembed.cpp) on top of what build_game.py imports
DRIVER = 'import runpy, sys, os\n'


def main():
    pbs, wheel, out = sys.argv[1:]
    os.makedirs(out, exist_ok=True)
    pylib = os.path.join(out, 'pylib')
    with zipfile.ZipFile(wheel) as z:
        z.extractall(pylib)

    # the modules: modulefinder over the translator and the driver (finds conditional imports too)
    driver = os.path.join(out, 'driver.py')
    open(driver, 'w').write(DRIVER + 'import build_game\n')
    mf = modulefinder.ModuleFinder(path=[RECOMP, pylib] + sys.path[1:])
    mf.run_script(driver)
    builtin_avail = set(sys.builtin_module_names)
    builtins = BOOTSTRAP | USED
    stdlib = {}
    stdroot = os.path.dirname(os.__file__)
    for name, mod in mf.modules.items():
        f = getattr(mod, '__file__', None)
        if name in builtin_avail:
            continue
        if f and f.startswith(stdroot + os.sep) and f.endswith('.py') and 'site-packages' not in f:
            stdlib[os.path.relpath(f, stdroot)] = f
    for f in os.listdir(os.path.join(stdroot, 'encodings')):          # decoders are looked up by name
        if f.endswith('.py'):
            stdlib['encodings/' + f] = os.path.join(stdroot, 'encodings', f)
    for f in os.listdir(stdroot):                                       # sysconfig reads this by name
        if f.startswith('_sysconfigdata_') and f.endswith('.py'):
            stdlib[f] = os.path.join(stdroot, f)

    # the module table: the distribution's config.c with the other modules left out
    src = open(os.path.join(pbs, 'build', 'Modules', 'config.c')).read()
    keep = []
    for line in src.splitlines():
        m = re.match(r'\s*\{"(\w+)", (\w+)\},', line)
        e = re.match(r'extern PyObject\* (PyInit_\w+)\(void\);', line)
        if m and m.group(1) not in builtins:
            continue
        if e and e.group(1)[len('PyInit_'):] not in builtins:
            continue
        keep.append(line)
    open(os.path.join(out, 'pyconfig.c'), 'w').write('\n'.join(keep) + '\n')

    # the link flags: libpython itself, then what the core and the modules kept need
    info = json.load(open(os.path.join(pbs, 'PYTHON.json')))['build_info']
    flags = [os.path.join(pbs, 'install', 'lib', 'libpython3.12.a')]
    links = list(info['core'].get('links', []))
    for name in sorted(builtins):
        for variant in info['extensions'].get(name, [])[:1]:
            links += variant.get('links', [])
    for l in links:
        if l.get('framework'):
            f = ['-framework', l['name']]
        elif l.get('path_static'):
            f = [os.path.join(pbs, l['path_static'])]
        elif l.get('system'):
            f = ['-l' + l['name']]
        else:
            continue
        if f[-1] not in flags:
            flags += f
    open(os.path.join(out, 'link.txt'), 'w').write(' '.join(flags) + '\n')

    # the archive
    records = []
    zbuf = io.BytesIO()
    with zipfile.ZipFile(zbuf, 'w', zipfile.ZIP_DEFLATED) as z:
        for rel in sorted(stdlib):
            z.write(stdlib[rel], rel.replace(os.sep, '/'))
    records.append(('stdlib.zip', zbuf.getvalue()))
    for f in TRANSLATOR:
        records.append(('translator/' + f, open(os.path.join(RECOMP, f), 'rb').read()))
    for d, _, files in os.walk(os.path.join(pylib, 'capstone')):
        for f in files:
            if f.endswith(('.py', '.so', '.dylib')) or '.so.' in f:     # the library: .so, macOS .dylib
                p = os.path.join(d, f)
                records.append(('pylib/' + os.path.relpath(p, pylib).replace(os.sep, '/'), open(p, 'rb').read()))
    with open(os.path.join(out, 'runtime.bin'), 'wb') as f:
        for name, data in records:
            n = name.encode()
            f.write(struct.pack('<I', len(n)) + n + struct.pack('<I', len(data)) + data)
    print('python runtime: %d built-in modules, %d stdlib files (%.1f MB zipped), %d records, %.1f MB' % (
        len([l for l in keep if l.strip().startswith('{"')]), len(stdlib), len(zbuf.getvalue()) / 1e6, len(records),
        os.path.getsize(os.path.join(out, 'runtime.bin')) / 1e6))


if __name__ == '__main__':
    main()
