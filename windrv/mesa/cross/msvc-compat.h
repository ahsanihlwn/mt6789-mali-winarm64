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
 * QueryPerformanceCounter and ignores the clock id entirely, so every id
 * below just resolves to the same monotonic counter.
 *
 * clockid_t is only needed because panfrost/perf/pan_perf.h returns one from
 * pan_perf_gpu_clock_id(); the UCRT's <time.h> has clock_t but no clockid_t.
 */
#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif

#ifndef MALI_COMPAT_CLOCKID_T
#define MALI_COMPAT_CLOCKID_T 1
typedef int clockid_t;
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
 * sysconf(_SC_PAGESIZE)
 * ---------------------
 * Meson's has_function('sysconf') probe passes here (cross/compat/unistd.h
 * defines an inline sysconf), so the build gets -DHAVE_SYSCONF and
 * src/util/os_misc.c takes its sysconf() branch. But os_misc.c only includes
 * <unistd.h> on Unix, so on Windows the call is undeclared and the definition
 * unreachable. Both live in this force-include instead, which every
 * translation unit sees, and <unistd.h> no longer defines the function.
 * Windows on ARM64 has 4 KiB pages.
 */
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE _SC_PAGESIZE
#endif

/*
 * sysconf(_SC_CLK_TCK)
 * --------------------
 * panfrost/vulkan/panvk_frame_prof.c converts a "process CPU ticks" value to
 * seconds through this. Its tick source is /proc/self/stat, which does not
 * exist here, so the count is always 0 and the value of _SC_CLK_TCK is
 * immaterial; CLOCKS_PER_SEC (1000 with the UCRT) is the least surprising
 * answer for Windows.
 */
#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK 2
#endif

#ifndef MALI_COMPAT_SYSCONF
#define MALI_COMPAT_SYSCONF 1
static inline long sysconf(int name)
{
    if (name == _SC_PAGESIZE)
        return 4096;
    if (name == _SC_CLK_TCK)
        return 1000;
    return -1;
}
#endif

/*
 * ARM64_FPCR / ARM64_FPSR
 * -----------------------
 * src/util/u_math.c reads and writes the FPCR through the MSVC intrinsics
 * _ReadStatusReg/_WriteStatusReg when DETECT_CC_MSVC. clang's <intrin.h>
 * declares those, but the register numbers they take are defined in MSVC's
 * <arm64intr.h>, and clang's copy of that header ships only the barrier enum
 * (measured: clang 20.1.8, arm64intr.h has no ARM64_FPCR). The build stops at
 *
 *     error: use of undeclared identifier 'ARM64_FPCR'
 *
 * ARM64_SYSREG below is MSVC's own encoding (arm64intr.h 14.50.35717), so the
 * numbers match what clang's intrinsics expect.
 */
#if defined(_M_ARM64) || defined(__aarch64__)
#ifndef ARM64_SYSREG
#define ARM64_SYSREG(op0, op1, crn, crm, op2) \
    ((((op0) & 1) << 14) | (((op1) & 7) << 11) | (((crn) & 15) << 7) | \
     (((crm) & 15) << 3) | ((op2) & 7))
#endif
#ifndef ARM64_FPCR
#define ARM64_FPCR ARM64_SYSREG(3, 3, 4, 4, 0)
#endif
#ifndef ARM64_FPSR
#define ARM64_FPSR ARM64_SYSREG(3, 3, 4, 4, 1)
#endif
#endif /* _M_ARM64 */

/*
 * uint / uchar
 * ------------
 * The BSD/glibc spellings come from <sys/types.h> on Unix; the UCRT has no
 * equivalent, so the FourFectVK v9 code that uses them - src/poly/geometry.h's
 * poly_geometry_params (DEVICE(uint), DEVICE(uchar)) and its helpers - stops at
 *
 *     error: unknown type name 'uint'; did you mean 'int'?
 *
 * which also collapses that struct's size and trips its
 * `sizeof(poly_geometry_params) == 79 * 4` static_assert (a cascade, not a
 * separate ABI problem: every use is a pointee type, so only the name matters).
 * Only the two names the tree actually reaches are provided; the rest of
 * <sys/types.h>'s vocabulary is MariaDB-of-the-90s and not worth guessing at.
 */
#ifndef MALI_COMPAT_UINT_DEFINED
#define MALI_COMPAT_UINT_DEFINED 1
typedef unsigned int uint;
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

/*
 * fcntl / F_DUPFD_CLOEXEC
 * -----------------------
 * panfrost/lib/kmod/kbase_kmod.c duplicates dma-buf descriptors with
 *
 *     fcntl(fd, F_DUPFD_CLOEXEC, 3)
 *
 * The UCRT's <fcntl.h> has the O_* flags and _open but no fcntl() at all, and
 * no F_DUPFD* constants. Both are declared here rather than in a compat
 * <fcntl.h> so the SDK header keeps providing the O_* flags. The definition is
 * in runtime/mali_drm.cpp (plain _dup(); the minimum-descriptor hint and
 * close-on-exec are meaningless on Windows). It is only reachable from the
 * dma-buf share/export paths, which stay disabled on Windows.
 */
#ifndef F_DUPFD
#define F_DUPFD          0
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC  1030   /* Linux value; read only by our shim */
#endif

#ifdef __cplusplus
extern "C" {
#endif

int fcntl(int fd, int cmd, ...);

#ifdef __cplusplus
}
#endif

/*
 * strcasecmp / strncasecmp
 * ------------------------
 * glibc's <string.h> declares these when _DEFAULT_SOURCE is in effect, so
 * portable Mesa code calls them after including only <string.h> (pan_trace.c's
 * tracepoint filter does exactly that). The UCRT has no <strings.h> and no
 * such names, hence the declarations here; the definitions are in
 * runtime/mali_posix.cpp and cross/compat/strings.h mirrors them for the
 * sources that do include <strings.h> explicitly.
 */
#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif

/*
 * strndup
 * -------
 * Mesa already *defines* this for Windows in src/util/strndup.c (that file is
 * gated on _WIN32 and is compiled into libmesa_util here -- the link would fail
 * with a duplicate symbol if we also defined it). What is missing is the
 * declaration: portability-aware callers include "util/strndup.h", but
 * panfrost/vulkan/panvk_vX_shader.c relies on glibc's <string.h> declaring it,
 * so the call is an implicit-declaration error. Declaring it here reproduces
 * the glibc situation without touching Mesa. The signature must match
 * src/util/strndup.h exactly.
 */
#ifdef __cplusplus
extern "C" {
#endif

char *strndup(const char *str, size_t max);

#ifdef __cplusplus
}
#endif

/*
 * strtok_r
 * --------
 * POSIX (declared by <string.h> on glibc, no UCRT equivalent); panvk's
 * physical-device setup uses it to split a frequency list. The UCRT's
 * strtok_s() has identical semantics -- same arguments, same "pass NULL to
 * continue, state in *saveptr" contract, same NULL return at the end -- so this
 * is a rename rather than a reimplementation. It is a macro for the same reason
 * alloca is: nothing here needs to take its address, and a macro cannot leave a
 * declaration without a definition behind.
 */
#ifndef strtok_r
#define strtok_r(str, delim, saveptr) strtok_s((str), (delim), (saveptr))
#endif

/*
 * alloca / PATH_MAX
 * -----------------
 * glibc's <stdlib.h> pulls in <alloca.h> under _DEFAULT_SOURCE, so portable
 * Mesa code calls alloca() after including only <stdlib.h>
 * (bifrost/bi_spill_ssa.c does). clang has __builtin_alloca on every target;
 * route the name there rather than to UCRT's _alloca, which is an intrinsic
 * the MSVC front end special-cases and is not a plain function.
 *
 * PATH_MAX has no Windows equivalent (UCRT has _MAX_PATH = 260, too small for
 * the temp-path buffers Mesa builds). Use the Linux value.
 */
#ifndef alloca
#define alloca __builtin_alloca
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif /* _MSC_VER */

#endif /* MALI_MSVC_COMPAT_H */