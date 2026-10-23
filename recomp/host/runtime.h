// runtime.h - host runtime for the recompiled game
//
// Memory layout of the emulated 32-bit address space (MEM + address):
//   0x00010000-0x003fffff  free
//   0x00400000-0x00857fff  the original image, mapped at its own addresses
//   0x10000000-0x3fffffff  heap (HeapAlloc / GlobalAlloc / LocalAlloc)
//   0x40000000-0x5fffffff  VirtualAlloc regions
//   0x60000000-0x6fffffff  thread stacks (1 MB each)
//   0x7ffd0000             thread information block (fs:), swapped per thread
//   0xfe000000+            fake addresses for imported functions (one per IAT slot)
//
// Threads: every emulated thread is a host thread, but only the thread holding the global lock
// runs translated code, so the single CPU state R stays valid. Threads switch only inside
// blocking calls (waits, sleeps, contended critical sections, message waits).
#pragma once
#include "../recomp_rt.h"
#ifdef __cplusplus
extern "C" {
#endif

#define RT_HEAP_BASE   0x10000000u
#define RT_HEAP_END    0x40000000u
#define RT_VIRT_BASE   0x40000000u
#define RT_VIRT_END    0x60000000u
#define RT_STACK_BASE  0x60000000u
#define RT_STACK_SIZE  0x00100000u
#define RT_IMPORT_BASE 0xfe000000u
#define RT_HOSTFN_BASE 0xfd000000u      // host functions handed out as code addresses (COM vtables)

// Arguments of the function being called natively (stack: [ret][arg0][arg1]...)
#define ARG(i) RD32(R.esp + 4 + 4 * (i))
// Return from a natively implemented function: set EAX, pop return address and `pop` bytes.
#define RT_RETURN(val, pop) do { R.eax = (uint32_t)(val); R.esp += 4 + (pop); return; } while (0)

// Thread management
typedef struct RtThread RtThread;
RtThread *rt_self(void);
uint32_t rt_thread_id(RtThread *t);
void rt_leave(void);            // release the global lock (before blocking)
void rt_enter(void);            // re-acquire it (after blocking)
// Run translated code at `addr` on the current thread with a fresh call frame. Pushes the given
// stack arguments (stdcall) and a sentinel return address; returns EAX.
uint32_t rt_call(uint32_t addr, int nargs, const uint32_t *args);
void rt_install_crash_handler(void); // log host crashes (signal, backtrace, guest registers)
void rt_profile_start(void);         // RECOMP_PROFILE=<file>: CPU sampling profiler (Linux)
void rt_profile_dump(void);          // writes the samples (called at exit)
int rt_reserve_guest_memory(void);  // fixed-base builds: reserve the guest range (call before SDL)
// Checks an exe against the build (exepatch.py): the unprotected perfect.exe, with or without
// DDGModernizer's patches, whose patched image has the hash recorded when the code was generated.
enum { RT_EXE_OK, RT_EXE_MISSING, RT_EXE_NOT_PE, RT_EXE_SAFEDISC, RT_EXE_UNKNOWN, RT_EXE_OTHER_BUILD };
int rt_check_exe(const char *exe);
// Start a new emulated thread running `start(param)`; returns a handle.
uint32_t rt_create_thread(uint32_t start, uint32_t param, int suspended, uint32_t *tid);

// Handles
enum { H_NONE, H_FILE, H_EVENT, H_SEMAPHORE, H_THREAD, H_MAPPING, H_HEAP, H_FIND, H_STD };
uint32_t rt_handle_new(int kind, void *obj);
void *rt_handle_get(uint32_t h, int kind);
uint64_t rt_thread_cpu_ns(uint32_t handle);   // guest thread CPU time (Linux), 0 if unknown
// Fibers: guest threads that run on the host thread switching to them (host/tasks.cpp)
RtThread *rt_fiber_create(void (*fn)(void *), void *arg);
void rt_fiber_switch(RtThread *to);
void rt_fiber_destroy(RtThread *t);
RtThread *rt_thread_from_handle(uint32_t h);
uint32_t rt_handle_of(RtThread *t);
void rt_handle_close(uint32_t h);

// Memory helpers
uint32_t rt_heap_alloc(uint32_t size, int zero);
uint32_t rt_heap_top(void);
uint64_t rt_heap_live(void);        // bytes in allocated heap blocks
uint64_t rt_guest_rss_kb(void);     // resident guest memory (Linux)
void rt_guest_commit(uint32_t addr, uint32_t size);   // before the system writes guest memory (Windows)
uint32_t rt_virt_top(void);         // end of the VirtualAlloc region in use
uint64_t rt_thread_instructions(void);           // this thread's retired instructions (macOS)
uint64_t rt_locked_instructions(void);           // total retired while holding the runtime lock                    // end of the used heap area (diagnostics)
uint32_t rt_host_fn(void (*fn)(void));          // a callable guest address for a host function
void rt_set_appdata_dir(const char *dir);       // host directory for "C:\\AppData" (saves)
void rt_heap_free(uint32_t addr);
uint32_t rt_heap_size(uint32_t addr);
uint32_t rt_heap_realloc(uint32_t addr, uint32_t size, int zero);
uint32_t rt_strlen(uint32_t a);
void rt_strcpy_to(uint32_t dst, const char *src);   // writes a C string into MEM
void rt_strget(uint32_t src, char *dst, int max);   // reads a C string from MEM

// Win32 shim and natives
void rt_win32_init(void);
RecompFn rt_import(const char *name);               // NULL if not implemented
RecompFn rt_import_sound(const char *name);         // DirectSound / mmio (host/dsound.cpp)
void rt_lock_host(void);                            // runtime lock for non-emulated host threads
void rt_unlock_host(void);
void rt_message_post(uint32_t tid, uint32_t msg, uint32_t wparam, uint32_t lparam);
const char *rt_game_dir(void);
void rt_map_path(const char *win, char *host, int max);   // Windows/host0: path -> host path

#ifdef __cplusplus
}
#endif
