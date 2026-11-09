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

#endif /* MALI_COMPAT_UNISTD_H */