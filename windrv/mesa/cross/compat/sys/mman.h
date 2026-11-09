/*
 * sys/mman.h  -  minimal POSIX memory-mapping header for Windows MSVC/ARM64.
 *
 * src/panfrost/midgard/midgard_compile.c includes <sys/mman.h> without using
 * anything from it. Several other Mesa files include it only under non-Windows
 * guards, or use Mesa's util/os_mman.h wrappers (VirtualAlloc based) on Windows.
 *
 * This header supplies the standard constants and prototypes so such includes
 * resolve. It is NOT an mmap implementation: Mesa's portable entry points are
 * os_mmap/os_munmap. If a Windows build ever calls mmap directly, implement it
 * here (the libdrm shim will need a real one -- see windrv/PANVK-SHIM.md).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_SYS_MMAN_H
#define MALI_COMPAT_SYS_MMAN_H

#include <stddef.h>

#ifndef PROT_NONE
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif

#ifndef MAP_SHARED
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#endif

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_SYS_MMAN_H */