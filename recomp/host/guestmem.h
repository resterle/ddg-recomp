// guestmem.h - the emulated 32-bit address space
//
// 4 GB at `base`, plus an alias of all of it directly below: translated code adds a sign-extended
// 32-bit base(+index) to a signed displacement (recomp_rt.h, RtAddr), so an access can land
// anywhere in [base - 4 GB, base + 4 GB); the alias maps base - x to the same bytes as
// base + 4 GB - x, which makes this exactly equal to 32-bit wrap-around for every address.
//
// rt_map_guest_memory(want): want = 0 lets the system choose (tests); otherwise the 4 GB must
// land exactly at `want` (fixed-base builds). Returns the base or NULL.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#define RT_GUEST_SIZE (1ull << 32)
#define RT_GUEST_ALIAS RT_GUEST_SIZE      // alias size below the base

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_error.h>

static inline uint8_t *rt_map_guest_memory(uint64_t want)
{
    // reserve alias + main in one piece so both fit next to each other
    mach_vm_address_t lo = want ? want - RT_GUEST_ALIAS : 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &lo, RT_GUEST_ALIAS + RT_GUEST_SIZE,
                                        want ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        static int told;
        if (!told++) fprintf(stderr, "guest memory: mach_vm_allocate at %llx failed: %d (%s)\n",
                             (unsigned long long)lo, kr, mach_error_string(kr));
        return NULL;
    }
    mach_vm_address_t base = lo + RT_GUEST_ALIAS;
    // replace the lower 4 GB with a shared view of the main 4 GB
    mach_vm_address_t alias = lo;
    vm_prot_t cur, max;
    kr = mach_vm_remap(mach_task_self(), &alias, RT_GUEST_ALIAS, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                       mach_task_self(), base, FALSE, &cur, &max, VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS || alias != lo) {
        fprintf(stderr, "guest memory: alias mapping failed (%d)\n", kr);
        return NULL;
    }
    return (uint8_t *)(uintptr_t)base;
}

// After fork() the child's main memory is a private copy while the alias still shows the
// parent's pages; re-create the alias over the child's own copy (test harness only).
static inline int rt_guest_realias(uint8_t *base)
{
    mach_vm_address_t alias = (mach_vm_address_t)(uintptr_t)(base - RT_GUEST_ALIAS);
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &alias, RT_GUEST_ALIAS, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                     mach_task_self(), (mach_vm_address_t)(uintptr_t)base,
                                     FALSE, &cur, &max, VM_INHERIT_SHARE);
    return kr == KERN_SUCCESS;
}

#else   // Linux: one memfd mapped twice
#include <sys/syscall.h>
#include <unistd.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000    // kernels before 4.17 treat it as a hint (checked below)
#endif

static inline uint8_t *rt_map_guest_memory(uint64_t want)
{
    int fd = (int)syscall(SYS_memfd_create, "guest", 0);
    if (fd < 0 || ftruncate(fd, (off_t)RT_GUEST_SIZE) != 0) return NULL;
    uint8_t *lo = mmap(want ? (void *)(uintptr_t)(want - RT_GUEST_ALIAS) : NULL, RT_GUEST_ALIAS + RT_GUEST_SIZE,
                       PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (want ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (lo == MAP_FAILED) { close(fd); return NULL; }
    if (want && lo != (uint8_t *)(uintptr_t)(want - RT_GUEST_ALIAS)) {     // hint not honoured
        munmap(lo, RT_GUEST_ALIAS + RT_GUEST_SIZE);
        close(fd);
        return NULL;
    }
    uint8_t *base = lo + RT_GUEST_ALIAS;
    if (mmap(base, RT_GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(lo, RT_GUEST_ALIAS, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    close(fd);
    return base;
}

// Linux maps both views MAP_SHARED, so a forked child shares them with the parent; nothing to
// re-create, but the test harness must not rely on copy-on-write there.
static inline int rt_guest_realias(uint8_t *base) { (void)base; return 1; }
#endif
