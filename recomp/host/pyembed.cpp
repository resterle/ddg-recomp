// pyembed.cpp - the translator (build_game.py) on the built-in Python (RECOMP_JIT builds; prototype).
//
// The program carries a static libpython (python-build-standalone, with the module table from
// host/mkpyruntime.py) and runtime.bin: the standard library modules the translator uses
// (stdlib.zip), the translator itself and capstone. On the first start with an exe they are
// unpacked into ~/.cache/ddg-recomp/runtime-<hash>/ and build_game.py translates the exe into
// game-<hash of the exe and the runtime>/src/, which jit.cpp then compiles.
#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// runtime.bin, included as is (RT_PY_RUNTIME: its path, from the Makefile)
extern "C" const char rt_py_runtime[], rt_py_runtime_end[];
#ifdef __APPLE__
__asm__(".section __DATA,__const\n.p2align 4\n.globl _rt_py_runtime\n_rt_py_runtime:\n.incbin \"" RT_PY_RUNTIME "\"\n"
        ".globl _rt_py_runtime_end\n_rt_py_runtime_end:\n.byte 0\n.text\n");
#else
__asm__(".section .rodata\n.balign 16\n.global rt_py_runtime\nrt_py_runtime:\n.incbin \"" RT_PY_RUNTIME "\"\n"
        ".global rt_py_runtime_end\nrt_py_runtime_end:\n.byte 0\n.text\n");
#endif

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

static void mkdirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// ~/.cache/ddg-recomp ($XDG_CACHE_HOME); macOS: ~/Library/Caches/ddg-recomp
std::string rt_cache_dir()
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    std::string h = home ? home : "/tmp";
#ifdef __APPLE__
    if (!(xdg && *xdg)) return h + "/Library/Caches/ddg-recomp";
#endif
    return (xdg && *xdg ? std::string(xdg) : h + "/.cache") + "/ddg-recomp";
}

static uint64_t runtimeHash()
{
    static uint64_t h = fnv(0xcbf29ce484222325ull, rt_py_runtime, (size_t)(rt_py_runtime_end - rt_py_runtime));
    return h;
}

// Unpacks runtime.bin once (a stamp file marks a complete copy).
static bool unpackRuntime(const std::string &dir)
{
    if (exists(dir + "/.complete")) return true;
    const char *p = rt_py_runtime, *end = rt_py_runtime_end;
    while (p + 8 <= end) {
        uint32_t n, len;
        memcpy(&n, p, 4);
        std::string name(p + 4, n);
        p += 4 + n;
        memcpy(&len, p, 4);
        const char *data = p + 4;
        p += 4 + len;
        if (p > end) return false;
        std::string path = dir + "/" + name;
        mkdirs(path.substr(0, path.rfind('/')));
        std::ofstream f(path, std::ios::binary);
        f.write(data, len);
        if (!f) return false;
    }
    std::ofstream(dir + "/.complete") << "ok\n";
    return true;
}

static void setPath(PyConfig &cfg, const std::string &dir)
{
    for (const char *sub : {"/stdlib.zip", "/translator", "/pylib"}) {
        wchar_t *w = Py_DecodeLocale((dir + sub).c_str(), nullptr);
        PyWideStringList_Append(&cfg.module_search_paths, w);
        PyMem_RawFree(w);
    }
    cfg.module_search_paths_set = 1;
}

// The cache folder for exe's game code (game-<hash of the exe and the runtime>), or "".
std::string rt_game_dir(const std::string &exe)
{
    std::ifstream f(exe, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    if (data.empty()) return "";
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)fnv(runtimeHash(), data.data(), data.size()));
    return rt_cache_dir() + "/game-" + hex;
}

bool rt_translated(const std::string &exe)
{
    std::string game = rt_game_dir(exe);
    return !game.empty() && exists(game + "/.complete");
}

// Translates exe into a cached source folder; returns it, or "" with the reason in err.
std::string rt_translate(const std::string &exe, std::string &err)
{
    std::string game = rt_game_dir(exe);
    if (game.empty()) { err = "Cannot read " + exe; return ""; }
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)runtimeHash());
    std::string rt = rt_cache_dir() + "/runtime-" + hex;
    std::string src = game + "/src";
    if (exists(game + "/.complete")) return src;

    if (!unpackRuntime(rt)) { err = "Cannot unpack the translator into " + rt; return ""; }
    mkdirs(src);
    fprintf(stderr, "game code: translating %s (first start with this exe)\n", exe.c_str());

    PyConfig cfg;
    PyConfig_InitIsolatedConfig(&cfg);
    cfg.install_signal_handlers = 0;
    cfg.site_import = 0;
    cfg.write_bytecode = 0;
    cfg.parse_argv = 0;
    PyConfig_SetBytesString(&cfg, &cfg.home, rt.c_str());
    setPath(cfg, rt);
    PyStatus st = Py_InitializeFromConfig(&cfg);
    PyConfig_Clear(&cfg);
    if (PyStatus_Exception(st)) { err = std::string("Python: ") + (st.err_msg ? st.err_msg : "init failed"); return ""; }
    // The paths go in as variables of __main__ and build_game.py sees them in a copy of the
    // environment: setenv() would race with getenv() on the other threads (SDL, the window).
    PyObject *main = PyImport_AddModule("__main__");
    auto setStr = [&](const char *name, const std::string &v) {
        PyObject *o = PyUnicode_DecodeFSDefault(v.c_str());
        PyObject_SetAttrString(main, name, o);
        Py_XDECREF(o);
    };
    setStr("rt_exe", exe);
    setStr("rt_out", src);
    setStr("rt_translator", rt + "/translator");
    const char *code =
        "import os, runpy, sys\n"
        "os.environ = dict(os.environ, PERFECT_EXE=rt_exe, RECOMP_GAME_DIR=rt_out)\n"
        "sys.argv = ['build_game.py']\n"
        "rt_status = 0\n"
        "rt_message = ''\n"
        "try:\n"
        "    runpy.run_path(os.path.join(rt_translator, 'build_game.py'), run_name='__main__')\n"
        "except SystemExit as e:\n"
        "    if e.code not in (None, 0):\n"
        "        rt_status = 1\n"
        "        if not isinstance(e.code, int):\n"
        "            print(e.code, file=sys.stderr)\n"
        "            rt_message = str(e.code)\n"
        "except BaseException as e:\n"
        "    import traceback; traceback.print_exc(); rt_status = 1; rt_message = 'Translation failed: %r' % e\n";
    int rc = PyRun_SimpleString(code);
    PyObject *status = main ? PyObject_GetAttrString(main, "rt_status") : nullptr;
    bool ok = rc == 0 && status && PyLong_AsLong(status) == 0;
    Py_XDECREF(status);
    if (!ok) {
        PyObject *msg = main ? PyObject_GetAttrString(main, "rt_message") : nullptr;
        const char *m = msg ? PyUnicode_AsUTF8(msg) : nullptr;
        err = m && *m ? m : "The translation failed (see the terminal output)";
        Py_XDECREF(msg);
    }
    Py_FinalizeEx();
    if (!ok) return "";
    std::ofstream(game + "/.complete") << "ok\n";
    return src;
}
