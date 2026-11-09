/*
 * unistd.h  -  minimal POSIX <unistd.h> stand-in for Windows MSVC/ARM64.
 *
 * Mesa's panfrost/lib/kmod headers include <unistd.h> unconditionally
 * (pan_kmod.h, panthor_kmod.c, ...). The UCRT has no such header; the closest
 * thing is <io.h> + <process.h>, which this file pulls in.
 *
 * Nothing here is an implementation: the POSIX names (read/write/close/...)
 * are provided by windrv/mesa/runtime/mali_posix.cpp and mali_drm.cpp, which
 * also define the __imp_* pointers the UCRT headers expect for their
 * __declspec(dllimport) declarations.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_UNISTD_H
#define MALI_COMPAT_UNISTD_H

#include <io.h>
#include <process.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdint.h>

/* Standard descriptor numbers (POSIX names; UCRT only has _fileno()). */
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

/*
 * access() mode bits. The UCRT provides _access() but no F_OK/R_OK/W_OK/X_OK
 * (its <io.h> only has the _A_* attribute flags), while POSIX declares them
 * here. panvk_instance.c probes /dev/mali<N> nodes with access(path, F_OK);
 * our shim's access() forwards to _access(), which reports missing nodes as
 * ENOENT, so the optional-node scan just skips them. The values are the POSIX
 * ones.
 */
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

/*
 * O_CLOEXEC does not exist in the UCRT. "Close on exec" is meaningless on
 * Windows anyway (handles are per-process), so define it to 0. Callers do
 * `open(path, O_RDWR | O_CLOEXEC)` and the flag simply folds away.
 */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* Windows has no ssize_t in its CRT. It is used as the return type of the
 * POSIX read/write wrappers, so make it pointer-sized like POSIX expects. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

/*
 * sysconf / _SC_*
 * ----------------
 * panfrost's pan_mempool.c (PAN_DBG_OVERFLOW guard) and a few other Linux-only
 * code paths ask for the system page size. Windows exposes it via
 * GetSystemInfo, not sysconf; on ARM64 it is always 4 KiB.
 *
 * The define *and* the inline function live in cross/msvc-compat.h, which is
 * force-included into every translation unit: Meson's has_function('sysconf')
 * probe passes here, so src/util/os_misc.c takes its sysconf() branch without
 * itself including <unistd.h>. Defining it in both headers would be a
 * redefinition, so this header deliberately only re-exports the selectors.
 */
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
#endif

/*
 * pread()
 * -------
 * POSIX gives pread() a home in <unistd.h>, so it is declared here; the
 * implementation is in runtime/mali_drm.cpp (seek, read, restore position).
 * The only caller in this tree is panfrost/vulkan/jm/panvk_vX_gpu_queue.c,
 * a Linux-only debug dump that reads back a BO through /proc/self/mem -- on
 * Windows the open() fails first, so the call is unreachable, but it still has
 * to type-check.
 *
 * The offset is int64_t rather than off_t on purpose: the UCRT's off_t is
 * 32-bit, while our mmap() already takes a 64-bit offset (GPU VAs do not fit
 * in 32 bits). Note the call site casts a host pointer through off_t before
 * calling, so it truncates on its own; that is inert here, not something this
 * declaration can or should paper over.
 */
#ifdef __cplusplus
extern "C" {
#endif

int64_t pread(int fd, void *buf, size_t count, int64_t offset);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_UNISTD_H */