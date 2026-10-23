// jit.cpp - game code compiled at startup with the built-in clang/LLVM (RECOMP_JIT builds; prototype).
//
// The program contains no game code. At startup the translated C (build_game.py's output, made by
// the built-in translator, host/pyembed.cpp; RECOMP_GAME_SRC=<folder> uses another) is compiled in memory with clang at -O2, the same options as
// the Makefile's rule for build/game/*.c, on all cores, and LLVM's JIT links the objects into the
// running program; host symbols are found in the program (-rdynamic). The objects are cached in
// ~/.cache/ddg-recomp/jit-<hash of the sources and the compiler>/, so later starts only link.
// No system headers are needed: the few C library declarations the code uses are built in.
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/thread.h"
#include "llvm/TargetParser/Host.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <dirent.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" const char rt_jit_header[];      // recomp_rt.h (rt_header.c, Makefile)
std::string rt_cache_dir();                 // host/pyembed.cpp
std::string rt_game_dir(const std::string &exe);
bool rt_translated(const std::string &exe);
std::string rt_translate(const std::string &exe, std::string &err);

// Progress for the settings window (rt_jit_start / rt_jit_poll): what is being done for which exe
static std::mutex g_progLock;
static std::string g_progExe, g_progText, g_pending;
static int g_progState;                     // 0 working, 1 ready, -1 failed (for g_progExe)
static bool g_working;
// clang and the translator recurse deeply: their threads get 16 MB of stack (secondary threads have
// 512 KB on macOS, and compiling crashed with a stack overflow there; Linux gives 8 MB)
static constexpr unsigned kStack = 16u << 20;
static llvm::thread g_worker;

static void progress(const std::string &text, int state = 0)
{
    std::lock_guard<std::mutex> lk(g_progLock);
    g_progText = text;
    g_progState = state;
}

// the tables build_game.py generates, in the order runtime.c reads them
static const char *const kSyms[] = {"RT_TABLE", "RT_TABLE_N", "RT_IMPORTS", "RT_IMPORTS_N",
                                    "rt_exe_patches", "rt_exe_npatches", "rt_exe_hash"};
extern "C" const void *rt_jit_sym[sizeof kSyms / sizeof kSyms[0]];
const void *rt_jit_sym[sizeof kSyms / sizeof kSyms[0]];

#define STR2(x) #x
#define STR(x) STR2(x)

// The C library as far as the translated code uses it (declarations only; the program provides it)
static const char *const kHeaders[][2] = {
    {"stdint.h",
     "#pragma once\n"
     "typedef __INT8_TYPE__ int8_t; typedef __UINT8_TYPE__ uint8_t; typedef __INT16_TYPE__ int16_t;\n"
     "typedef __UINT16_TYPE__ uint16_t; typedef __INT32_TYPE__ int32_t; typedef __UINT32_TYPE__ uint32_t;\n"
     "typedef __INT64_TYPE__ int64_t; typedef __UINT64_TYPE__ uint64_t;\n"
     "typedef __INTPTR_TYPE__ intptr_t; typedef __UINTPTR_TYPE__ uintptr_t;\n"
     "#define INT64_MIN (-__INT64_MAX__ - 1)\n#define INT64_MAX __INT64_MAX__\n"
     "#define INT32_MIN (-__INT32_MAX__ - 1)\n#define INT32_MAX __INT32_MAX__\n#define UINT32_MAX 0xffffffffu\n"},
    {"string.h",
     "#pragma once\ntypedef __SIZE_TYPE__ size_t;\n#define NULL ((void *)0)\n"
     "void *memcpy(void *, const void *, size_t); void *memmove(void *, const void *, size_t);\n"
     "void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);\n"
     "size_t strlen(const char *);\n"},
    {"math.h",
     "#pragma once\n#define NAN __builtin_nan(\"\")\n#define INFINITY __builtin_inf()\n#define M_PI 3.14159265358979323846\n"
     "#define isnan(x) __builtin_isnan(x)\n#define isinf(x) __builtin_isinf(x)\n#define signbit(x) __builtin_signbit(x)\n"
     "double sqrt(double), sin(double), cos(double), tan(double), atan(double), atan2(double, double);\n"
     "double floor(double), ceil(double), trunc(double), nearbyint(double), rint(double), round(double);\n"
     "double fabs(double), fmod(double, double), ldexp(double, int), frexp(double, int *);\n"
     "double log(double), log2(double), log10(double), exp(double), pow(double, double);\n"
     "float sqrtf(float), floorf(float), ceilf(float), fabsf(float);\n"},
    {"stdio.h",
#if defined(_WIN32)
     "#pragma once\ntypedef struct _iobuf FILE;\nFILE *__acrt_iob_func(unsigned);\n#define stderr (__acrt_iob_func(2))\n"
#elif defined(__APPLE__)
     "#pragma once\ntypedef struct __sFILE FILE;\nextern FILE *__stderrp;\n#define stderr __stderrp\n"
#else
     "#pragma once\ntypedef struct _IO_FILE FILE;\nextern FILE *stderr;\n"
#endif
     "int fprintf(FILE *, const char *, ...); int printf(const char *, ...);\n"},
    {"stdlib.h",
     "#pragma once\ntypedef __SIZE_TYPE__ size_t;\nchar *getenv(const char *); void abort(void); void exit(int);\n"
     "void *malloc(size_t); void free(void *);\n"},
};

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

static bool readFile(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static void mkdirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

// The CPU baseline the normal builds use (the Makefile's defaults): x86-64 and Apple's arm64 ABI,
// which wants frame pointers
#if defined(__x86_64__)
#define kTargetCpu "x86-64"
#define kFramePointer "none"
#elif defined(__APPLE__)
#define kTargetCpu "apple-m1"
#define kFramePointer "non-leaf"
#else
#define kTargetCpu "generic"
#define kFramePointer "non-leaf"
#endif

#ifdef _WIN32
// Windows: the C library is linked into the program statically and not exported, so the JIT gets
// the functions the translated code calls from this table (the runtime's own interface it finds
// among the program's exports). The code is placed within 2 GB of the program (it calls and reads
// the program with 32-bit offsets) and its unwind tables are registered (longjmp unwinds across
// it on a guest thread's exit). host/os_win.c does the Windows side.
extern "C" {
void *rt_win_exe_symbol(const char *name);
void *rt_win_jit_alloc(size_t size);
int rt_win_jit_protect(void *p, size_t size, unsigned prot);
int rt_win_jit_unwind(void *pdata, size_t size, uint64_t base);
void __chkstk(void);
extern int _fltused;
}

static int jitFprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

static int jitPrintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

static void addCLibrary(llvm::orc::LLJIT &J, llvm::orc::SymbolMap &m)
{
    using D1 = double (*)(double);
    using D2 = double (*)(double, double);
    using F1 = float (*)(float);
    const std::pair<const char *, void *> table[] = {
        {"memcpy", (void *)memcpy}, {"memmove", (void *)memmove}, {"memset", (void *)memset},
        {"memcmp", (void *)memcmp}, {"strlen", (void *)strlen},
        {"sqrt", (void *)(D1)sqrt}, {"sin", (void *)(D1)sin}, {"cos", (void *)(D1)cos}, {"tan", (void *)(D1)tan},
        {"atan", (void *)(D1)atan}, {"atan2", (void *)(D2)atan2}, {"floor", (void *)(D1)floor},
        {"ceil", (void *)(D1)ceil}, {"trunc", (void *)(D1)trunc}, {"nearbyint", (void *)(D1)nearbyint},
        {"rint", (void *)(D1)rint}, {"round", (void *)(D1)round}, {"fabs", (void *)(D1)fabs},
        {"fmod", (void *)(D2)fmod}, {"ldexp", (void *)(double (*)(double, int))ldexp},
        {"frexp", (void *)(double (*)(double, int *))frexp}, {"log", (void *)(D1)log}, {"log2", (void *)(D1)log2},
        {"log10", (void *)(D1)log10}, {"exp", (void *)(D1)exp}, {"pow", (void *)(D2)pow},
        {"sqrtf", (void *)(F1)sqrtf}, {"floorf", (void *)(F1)floorf}, {"ceilf", (void *)(F1)ceilf},
        {"fabsf", (void *)(F1)fabsf},
        {"fprintf", (void *)jitFprintf}, {"printf", (void *)jitPrintf}, {"__acrt_iob_func", (void *)__acrt_iob_func},
        {"getenv", (void *)getenv}, {"abort", (void *)abort}, {"exit", (void *)exit},
        {"malloc", (void *)malloc}, {"free", (void *)free},
        {"__chkstk", (void *)__chkstk}, {"_fltused", (void *)&_fltused},
    };
    for (auto &e : table)
        m[J.getExecutionSession().intern(e.first)] = {llvm::orc::ExecutorAddr::fromPtr(e.second), llvm::JITSymbolFlags::Exported};
}

// SectionMemoryManager on memory near the program, registering each object's .pdata
class NearMapper : public llvm::SectionMemoryManager::MemoryMapper {
public:
    llvm::sys::MemoryBlock allocateMappedMemory(llvm::SectionMemoryManager::AllocationPurpose, size_t n,
                                                const llvm::sys::MemoryBlock *, unsigned, std::error_code &ec) override
    {
        void *p = rt_win_jit_alloc(n);
        if (!p) { ec = std::make_error_code(std::errc::not_enough_memory); return {}; }
        ec = {};
        return llvm::sys::MemoryBlock(p, (n + 0xffff) & ~(size_t)0xffff);
    }
    std::error_code protectMappedMemory(const llvm::sys::MemoryBlock &b, unsigned flags) override
    {
        unsigned prot = ((flags & llvm::sys::Memory::MF_READ) ? 1 : 0) | ((flags & llvm::sys::Memory::MF_WRITE) ? 2 : 0) |
                        ((flags & llvm::sys::Memory::MF_EXEC) ? 4 : 0);
        return rt_win_jit_protect(b.base(), b.allocatedSize(), prot) ? std::make_error_code(std::errc::permission_denied) : std::error_code();
    }
    std::error_code releaseMappedMemory(llvm::sys::MemoryBlock &) override { return {}; }    // kept for the program's life
};

class NearMemoryManager : public llvm::SectionMemoryManager {
public:
    NearMemoryManager() : llvm::SectionMemoryManager(&mapper()) {}
    uint8_t *allocateCodeSection(uintptr_t size, unsigned align, unsigned id, llvm::StringRef name) override
    {
        return note(llvm::SectionMemoryManager::allocateCodeSection(size, align, id, name), size, name, "code");
    }
    uint8_t *allocateDataSection(uintptr_t size, unsigned align, unsigned id, llvm::StringRef name, bool ro) override
    {
        return note(llvm::SectionMemoryManager::allocateDataSection(size, align, id, name, ro), size, name, ro ? "ro" : "rw");
    }
    // RuntimeDyld passes the object's .pdata here; its entries are relative to the object's lowest
    // section (RuntimeDyldCOFFX86_64::getImageBase)
    void registerEHFrames(uint8_t *addr, uint64_t, size_t size) override
    {
        if (getenv("RECOMP_JIT_TRACE")) fprintf(stderr, "jit %p pdata %p %zx base %llx\n", (void *)this, addr, size, (unsigned long long)base_);
        if (rt_win_jit_unwind(addr, size, base_)) fprintf(stderr, "game code: unwind table not registered\n");
    }
    void deregisterEHFrames() override {}

private:
    static NearMapper &mapper() { static NearMapper m; return m; }
    uint8_t *note(uint8_t *p, uintptr_t size, llvm::StringRef name, const char *kind)
    {
        if (getenv("RECOMP_JIT_TRACE")) fprintf(stderr, "jit %p %p %8llx %s %s\n", (void *)this, p, (unsigned long long)size, kind, name.str().c_str());
        if (p && (uint64_t)(uintptr_t)p < base_) base_ = (uint64_t)(uintptr_t)p;
        return p;
    }
    uint64_t base_ = ~0ull;
};

static void *hostSymbol(const char *name) { return rt_win_exe_symbol(name); }
#else
static void *hostSymbol(const char *name) { return dlsym(RTLD_DEFAULT, name); }
#endif

// The built-in headers' (virtual) folder: an absolute path (Windows: with a drive)
#ifdef _WIN32
static const char kIncDir[] = "C:/ddg-include";
#else
static const char kIncDir[] = "/ddg-include";
#endif

// One translated file -> an object (in memory). Thread-safe: each call has its own compiler.
static bool compileOne(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs, const std::string &src,
                       const std::string &srcDir, llvm::SmallVectorImpl<char> &obj, std::string &diag)
{
    static const std::string triple = llvm::sys::getProcessTriple();
    std::vector<const char *> args = {
        "-triple", triple.c_str(), "-emit-obj", "-O2", "-mrelocation-model", "pic", "-pic-level", "2",
        "-mframe-pointer=" kFramePointer, "-relaxed-aliasing", "-fmath-errno", "-ffp-contract=on", "-fno-rounding-math",
        "-target-cpu", kTargetCpu, "-vectorize-loops", "-vectorize-slp",
        "-D", "RT_MEM_BASE=" STR(RT_MEM_BASE), "-nostdsysteminc", "-nobuiltininc",
        "-isystem", kIncDir, "-I", srcDir.c_str(), "-w",
#ifdef _WIN32
        // unwind tables (.pdata/.xdata) as clang's driver adds them for Windows; R through its address
        "-funwind-tables=2", "-D", "RT_HOST_DATA=__attribute__((dllimport))",
#endif
        "-x", "c", src.c_str()};
    llvm::raw_string_ostream ds(diag);
    clang::DiagnosticOptions dopts;
    auto *printer = new clang::TextDiagnosticPrinter(ds, dopts);
    auto diags = clang::CompilerInstance::createDiagnostics(*vfs, dopts, printer, true);
    auto inv = std::make_shared<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*inv, args, *diags)) return false;
    clang::CompilerInstance ci(inv);
    ci.setVirtualFileSystem(vfs);
    ci.createDiagnostics(new clang::TextDiagnosticPrinter(ds, ci.getDiagnosticOpts()), true);
    ci.setOutputStream(std::make_unique<llvm::raw_svector_ostream>(obj));
    clang::EmitObjAction act;
    return ci.ExecuteAction(act) && !ci.getDiagnostics().hasErrorOccurred();
}

static int load(const std::string &srcDir)
{
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    llvm::InitializeNativeTarget();             // code generation for clang and the JIT
    llvm::InitializeNativeTargetAsmPrinter();
    std::vector<std::string> files;
    if (DIR *d = opendir(srcDir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 2 && n.compare(n.size() - 2, 2, ".c") == 0) files.push_back(n);
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { fprintf(stderr, "game code: no .c files in %s\n", srcDir.c_str()); return 0; }

    // cache key: the sources, decls.h, the runtime header and the compiler
    uint64_t key = 0xcbf29ce484222325ull;
    std::string text;
    for (const std::string &n : files) {
        if (!readFile(srcDir + "/" + n, text)) return 0;
        key = fnv(fnv(key, n.data(), n.size()), text.data(), text.size());
    }
    if (readFile(srcDir + "/decls.h", text)) key = fnv(key, text.data(), text.size());
    key = fnv(key, rt_jit_header, strlen(rt_jit_header));
    key = fnv(key, LLVM_VERSION_STRING, strlen(LLVM_VERSION_STRING));
    char keyHex[17];
    snprintf(keyHex, sizeof keyHex, "%016llx", (unsigned long long)key);
    std::string cache = rt_cache_dir() + "/jit-" + keyHex;

    std::vector<std::unique_ptr<llvm::MemoryBuffer>> objs(files.size());
    bool cached = true;
    for (size_t i = 0; i < files.size() && cached; i++) {
        auto b = llvm::MemoryBuffer::getFile(cache + "/" + files[i] + ".o");
        if (b) objs[i] = std::move(*b);
        else cached = false;
    }
    if (!cached) {
        // the built-in headers over the real file system (the sources)
        auto mem = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
        for (auto &h : kHeaders) mem->addFile(std::string(kIncDir) + "/" + h[0], 0, llvm::MemoryBuffer::getMemBuffer(h[1]));
        mem->addFile(std::string(kIncDir) + "/recomp_rt.h", 0, llvm::MemoryBuffer::getMemBuffer(rt_jit_header));
        auto vfs = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(llvm::vfs::getRealFileSystem());
        vfs->pushOverlay(mem);
        // biggest files first, so the threads finish together
        std::vector<size_t> order(files.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::vector<off_t> sizes(files.size());
        for (size_t i = 0; i < files.size(); i++) {
            struct stat st;
            sizes[i] = stat((srcDir + "/" + files[i]).c_str(), &st) == 0 ? st.st_size : 0;
        }
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sizes[a] > sizes[b]; });
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        std::mutex outLock;
        unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
        fprintf(stderr, "game code: compiling %zu files on %u threads (first start)\n", files.size(), jobs);
        std::atomic<size_t> done{0};
        auto show = [&] {
            char t[96];
            snprintf(t, sizeof t, "Compiling the game code: %zu of %zu files (first start)", (size_t)done, files.size());
            progress(t);
        };
        show();
        std::vector<llvm::thread> pool;
        for (unsigned t = 0; t < std::min<size_t>(jobs, files.size()); t++)
            pool.emplace_back(std::optional<unsigned>(kStack), [&] {
                for (size_t k; (k = next++) < order.size() && !failed;) {
                    size_t i = order[k];
                    llvm::SmallVector<char, 0> obj;
                    std::string diag;
                    if (!compileOne(vfs, srcDir + "/" + files[i], srcDir, obj, diag)) {
                        std::lock_guard<std::mutex> lk(outLock);
                        fprintf(stderr, "game code: compiling %s failed\n%s", files[i].c_str(), diag.c_str());
                        progress("Compiling " + files[i] + " failed (see the terminal output)", -1);
                        failed = true;
                        continue;
                    }
                    objs[i] = llvm::MemoryBuffer::getMemBufferCopy(llvm::StringRef(obj.data(), obj.size()), files[i] + ".o");
                    done++;
                    show();
                }
            });
        for (auto &t : pool) t.join();
        if (failed) return 0;
        mkdirs(cache);
        for (size_t i = 0; i < files.size(); i++) {
            std::ofstream f(cache + "/" + files[i] + ".o", std::ios::binary);
            f.write(objs[i]->getBufferStart(), (std::streamsize)objs[i]->getBufferSize());
        }
    }
    auto t1 = clock::now();
    progress("Loading the game code");

    llvm::orc::LLJITBuilder builder;
#ifdef _WIN32
    builder.setObjectLinkingLayerCreator([](llvm::orc::ExecutionSession &es, llvm::jitlink::JITLinkMemoryManager &) -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
        auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(es, [](const llvm::MemoryBuffer &) {
            return std::make_unique<NearMemoryManager>();
        });
        layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
        layer->setAutoClaimResponsibilityForObjectSymbols(true);
        return std::move(layer);
    });
#endif
    auto jit = builder.create();
    if (!jit) { fprintf(stderr, "game code: %s\n", llvm::toString(jit.takeError()).c_str()); return 0; }
    auto &J = **jit;
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(J.getDataLayout().getGlobalPrefix());
    if (!gen) { fprintf(stderr, "game code: %s\n", llvm::toString(gen.takeError()).c_str()); return 0; }
    J.getMainJITDylib().addGenerator(std::move(*gen));
    // The translated code defines every native weakly (a trap, native_info.c), for the linker to
    // take the program's own instead. The JIT keeps the first definition: define the program's
    // ones first, and the weak ones in the objects are dropped.
    llvm::orc::SymbolMap natives;
#ifdef _WIN32
    addCLibrary(J, natives);
#endif
    for (auto &o : objs) {
        auto file = llvm::object::ObjectFile::createObjectFile(o->getMemBufferRef());
        if (!file) { llvm::consumeError(file.takeError()); continue; }
        for (const llvm::object::SymbolRef &sym : (*file)->symbols()) {
            auto flags = sym.getFlags();
            auto name = sym.getName();
            // (COFF: a weak definition is an undefined "weak external" with a default)
            bool undef = flags && (*flags & llvm::object::SymbolRef::SF_Undefined) && !(*file)->isCOFF();
            if (!flags || !name || !(*flags & llvm::object::SymbolRef::SF_Weak) || undef) {
                if (!flags) llvm::consumeError(flags.takeError());
                if (!name) llvm::consumeError(name.takeError());
                continue;
            }
            // the object's names carry the platform's prefix (Mach-O: "_"), dlsym's do not
            llvm::StringRef plain = *name;
            char prefix = J.getDataLayout().getGlobalPrefix();
            if (prefix && plain.starts_with(llvm::StringRef(&prefix, 1))) plain = plain.drop_front();
            if (void *addr = hostSymbol(plain.str().c_str()))
                natives[J.getExecutionSession().intern(*name)] = {llvm::orc::ExecutorAddr::fromPtr(addr), llvm::JITSymbolFlags::Exported};
        }
    }
    if (auto e = J.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(natives)))) {
        fprintf(stderr, "game code: %s\n", llvm::toString(std::move(e)).c_str());
        return 0;
    }
    for (auto &o : objs)
        if (auto e = J.addObjectFile(std::move(o))) { fprintf(stderr, "game code: %s\n", llvm::toString(std::move(e)).c_str()); return 0; }
    const void *syms[sizeof kSyms / sizeof kSyms[0]];
    for (size_t i = 0; i < sizeof kSyms / sizeof kSyms[0]; i++) {
        auto a = J.lookup(kSyms[i]);
        if (!a) { fprintf(stderr, "game code: %s\n", llvm::toString(a.takeError()).c_str()); return 0; }
        syms[i] = a->toPtr<const void *>();
    }
    // the settings window checks the exe meanwhile (runtime.c read_exe), taking rt_jit_sym[0] for
    // "the game code is there": set it last (it crashed reading rt_jit_sym[4] before it was set)
    for (size_t i = 1; i < sizeof kSyms / sizeof kSyms[0]; i++) rt_jit_sym[i] = syms[i];
    __atomic_store_n(&rt_jit_sym[0], syms[0], __ATOMIC_RELEASE);
    jit->release();                             // the code lives as long as the program
    auto t2 = clock::now();
    auto ms = [](auto a, auto b) { return (double)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
    fprintf(stderr, "game code: %s in %.0f ms, linked in %.0f ms (%s)\n", cached ? "from the cache" : "compiled",
            ms(t0, t1), ms(t1, t2), cache.c_str());
    return 1;
}

// The game code for exe: translated (once per exe) and compiled (once per translation), then linked.
extern "C" int rt_jit_load(const char *exe)
{
    const char *dirEnv = getenv("RECOMP_GAME_SRC");
    std::string src = dirEnv && *dirEnv ? std::string(dirEnv) : "";
    if (src.empty()) {
        progress(rt_translated(exe) ? "Loading the game code" : "Translating perfect.exe (first start, about a minute)");
        std::string err;
        src = rt_translate(exe, err);
        if (src.empty()) { progress(err, -1); return 0; }
    }
    if (!load(src)) {
        std::lock_guard<std::mutex> lk(g_progLock);
        if (g_progState >= 0) { g_progText = "The game code could not be loaded (see the terminal output)"; g_progState = -1; }
        return 0;
    }
    if (!dirEnv || !*dirEnv) std::ofstream(rt_game_dir(exe) + "/.ready") << "ok\n";
    progress("Ready", 1);
    return 1;
}

// Whether exe's game code is ready to load from the cache (the settings window opens otherwise).
extern "C" int rt_jit_cached(const char *exe)
{
    std::string g = rt_game_dir(exe);
    struct stat st;
    return !g.empty() && stat((g + "/.ready").c_str(), &st) == 0;
}

// Prepares exe's game code in the background (the settings window); a request while another runs
// waits for it.
static void work()
{
    for (;;) {
        std::string exe;
        {
            std::lock_guard<std::mutex> lk(g_progLock);
            if (g_pending.empty()) { g_working = false; return; }
            exe.swap(g_pending);
            g_progExe = exe;
            g_progState = 0;
            g_progText = "Preparing";
        }
        rt_jit_load(exe.c_str());
    }
}

extern "C" void rt_jit_start(const char *exe)
{
    std::lock_guard<std::mutex> lk(g_progLock);
    if (g_progExe == exe && !g_working && g_progState == 1) return;     // ready already
    if (g_working && g_progExe == exe && g_pending.empty()) return;      // being prepared
    g_pending = exe;
    if (!g_working) {
        if (g_worker.joinable()) g_worker.join();
        g_working = true;
        g_worker = llvm::thread(std::optional<unsigned>(kStack), work);
    }
}

extern "C" int rt_jit_poll(const char *exe, char *text, size_t size)
{
    std::lock_guard<std::mutex> lk(g_progLock);
    bool mine = g_progExe == exe && g_pending.empty();
    snprintf(text, size, "%s", mine ? g_progText.c_str() : "Preparing");
    return mine ? g_progState : 0;
}

// The game code for exe, loaded: waits for the background preparation, or loads it now.
extern "C" int rt_jit_ensure(const char *exe)
{
    {
        std::unique_lock<std::mutex> lk(g_progLock);
        bool busy = g_working;
        lk.unlock();
        if (busy || g_worker.joinable()) g_worker.join();
        lk.lock();
        if (g_progExe == exe && g_progState == 1) return 1;
    }
    return rt_jit_load(exe);
}
