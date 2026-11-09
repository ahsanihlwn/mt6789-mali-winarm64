/*
 * msvc-compat.h  -  force-included (clang-cl /FI) compatibility layer for
 *                   building Mesa on aarch64-pc-windows-msvc.
 *
 * tools\setup.ps1 adds this to the cross file's c_args/cpp_args, so every
 * translation unit sees it. It forward-declares/defines the small things the
 * MSVC CRT does not provide but Mesa's sources assume from glibc/BSD:
 *
 *   - the lower-case _interlocked*64 intrinsics (arm64-msvc-atomic.h)
 *   - asprintf / vasprintf (GNU extension; used by the Midgard compiler)
 *
 * Add further shims here as the build finds them, and keep each one explained.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_MSVC_COMPAT_H
#define MALI_MSVC_COMPAT_H

#include "arm64-msvc-atomic.h"

#if defined(_MSC_VER)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * asprintf / vasprintf
 * --------------------
 * Mesa's src/panfrost/midgard/midgard_compile.c calls asprintf(); UCRT only
 * offers vsnprintf and friends, so clang rejects the call with
 * "call to undeclared function 'asprintf'". UCRT's vsnprintf is C99-compliant
 * (returns the required length for a NULL/0 buffer), so a tiny portable
 * implementation is enough. Return the number of characters written, excluding
 * the NUL, or -1 with *strp = NULL on error.
 */
static inline int vasprintf(char **strp, const char *fmt, va_list ap)
{
    va_list ap2;
    int len;
    char *buf;
    int n;

    va_copy(ap2, ap);
    len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);

    if (len < 0) {
        *strp = NULL;
        return -1;
    }

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        *strp = NULL;
        return -1;
    }

    n = vsnprintf(buf, (size_t)len + 1, fmt, ap);
    if (n < 0) {
        free(buf);
        *strp = NULL;
        return -1;
    }

    *strp = buf;
    return n;
}

static inline int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vasprintf(strp, fmt, ap);
    va_end(ap);

    return n;
}

/*
 * clock_gettime / CLOCK_MONOTONIC
 * -------------------------------
 * src/util/libsync.h (pulled in by the panthor/CSF kmod backend, which Meson
 * always compiles even though this hardware uses the panfrost/JM backend)
 * reads CLOCK_MONOTONIC and calls clock_gettime(). Windows has no Posix clock
 * API; the implementation lives in windrv/mesa/runtime/mali_drm.cpp on top of
 * QueryPerformanceCounter. Only libsync's polling helper uses it.
 */
#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
int clock_gettime(int clk_id, struct timespec *tp);
#ifdef __cplusplus
}
#endif

/*
 * getpagesize
 * -----------
 * src/panfrost/lib/kmod/panthor_kmod.c (CSF backend, compiled unconditionally
 * by Meson) asks for the host page size to align its heap. Windows on ARM64
 * uses 4 KiB pages; the value is only used to size a VA reservation, and the
 * panthor backend is never selected on this hardware.
 */
#ifndef getpagesize
static inline int getpagesize(void)
{
    return 4096;
}
#endif

/*
 * pthread_mutex_t
 * ---------------
 * src/panfrost/lib/pan_blend.h stores a pthread mutex in the blend-shader
 * cache. Mesa's Windows codepaths generally use simple_mtx_t, but this one
 * header is shared with the Linux build and has no _WIN32 branch. The cache
 * lock is never taken by the code this project builds (only init/destroy are
 * reached), yet the type has to exist and behave if a future caller locks it.
 *
 * A spin lock over the ARM64 interlocked builtins is enough: it is never held
 * across a syscall or a GPU wait here, and it avoids dragging <windows.h> into
 * every Mesa translation unit through the force-included header.
 */
#if !defined(MALI_PTHREAD_MUTEX_DEFINED)
#define MALI_PTHREAD_MUTEX_DEFINED 1

typedef struct {
    volatile long locked;
} pthread_mutex_t;

/* Static initialiser, as used by Mesa's src/util/u_atomic.c. */
#ifndef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER { 0 }
#endif

static inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr)
{
    (void)attr;
    m->locked = 0;
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *m)
{
    while (_InterlockedCompareExchange((long volatile *)&m->locked, 1, 0) != 0) {
#if defined(_M_ARM64)
        __yield();
#endif
    }
    return 0;
}

static inline int pthread_mutex_trylock(pthread_mutex_t *m)
{
    return _InterlockedCompareExchange((long volatile *)&m->locked, 1, 0) == 0 ? 0 : 16;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
    _InterlockedExchange((long volatile *)&m->locked, 0);
    return 0;
}

#endif /* MALI_PTHREAD_MUTEX_DEFINED */

/*
 * float16_t: clang's <arm_neon.h> vs Mesa's C++ helper
 * ---------------------------------------------------
 * On ARM64 the UCRT's <wchar.h> does:
 *
 *     #if defined(__clang__) && (defined(_M_ARM64) || defined(_M_ARM64EC))
 *     #include <arm_neon.h>
 *     #endif
 *
 * and clang's arm_vector_types.h typedefs `float16_t` as __fp16 in the global
 * namespace. Mesa's src/util/half_float.h defines a *C++ helper struct* with
 * the same name (fp16 vs uint16_t overload disambiguation), so any TU that
 * pulls in <vector>/<wchar.h> before half_float.h fails with
 *
 *     error: typedef redefinition with different types ('__fp16' vs 'float16_t')
 *     error: invalid vector element type 'float16_t'
 *
 * Mesa never uses NEON intrinsics and the two types are unrelated. Parse
 * <arm_neon.h> once here and then rename the identifier: #define is not
 * retroactive, so the NEON typedefs and inline functions are already complete,
 * while every later use in Mesa -- the struct, its constructors, ir.h,
 * builtin_functions.cpp, ast_to_hir.cpp -- consistently becomes
 * mesa_float16_t. Doing it in the force-include keeps every TU consistent
 * (identical type names across objects, no ODR surprises), and the UCRT's own
 * later #include <arm_neon.h> is a no-op thanks to its __ARM_NEON_H guard.
 */
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(MALI_NO_FLOAT16_RENAME)
#include <arm_neon.h>
#define float16_t mesa_float16_t
#endif

#endif /* _MSC_VER */

#endif /* MALI_MSVC_COMPAT_H */