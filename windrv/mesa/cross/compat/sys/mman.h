/*
 * sys/mman.h  -  minimal POSIX memory-mapping header for Windows MSVC/ARM64.
 *
 * src/panfrost/midgard/midgard_compile.c includes <sys/mman.h> without using
 * anything from it. Several other Mesa files include it only under non-Windows
 * guards, or use Mesa's util/os_mman.h wrappers (VirtualAlloc based) on Windows.
 *
 * This header supplies the standard constants and prototypes so such includes
 * resolve. The prototypes are implemented in windrv/mesa/runtime/mali_drm.cpp:
 * mmap() resolves a buffer object by GPU VA (the libdrm shim's mapping) and
 * munmap()/mprotect() are no-ops, because the KMD owns the mapping. Mesa's
 * portable entry points (os_mmap/os_munmap) are still preferred elsewhere.
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

void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);

/*
 * mincore()
 * ---------
 * POSIX puts mincore() in <sys/mman.h>, so it is declared here; it always
 * fails with ENOSYS (see runtime/mali_drm.cpp). The only caller in this tree is
 * panfrost/vulkan/jm/panvk_vX_gpu_queue.c's debug heap dump, which treats a
 * failure as "residency unknown" and prints mincore=unavailable. There is no
 * meaningful Windows answer: BO pages are mapped by our KMD, and the Linux
 * concept of a lazily populated page this query exists for (a GPU growable
 * heap that may never have been written) does not map onto a working-set
 * query. Reporting ENOSYS keeps the diagnostic honest instead of inventing a
 * residency bit.
 */
int mincore(void *addr, size_t length, unsigned char *vec);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_SYS_MMAN_H */