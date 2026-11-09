/*
 * mali_vcrt.cpp  -  the last pieces of the MSVC runtime the CRT-free ARM64
 *                   recipe still needs, so the built DLLs import nothing from
 *                   VCRUNTIME140.dll or MSVCP140.dll.
 *
 * WHY THIS EXISTS
 * ---------------
 * This host has no ARM64 vcruntime.lib / libcmt.lib / msvcprt.lib, so Mesa and
 * the KMD/UMD link against the SDK's dynamic UCRT (ucrt.lib) plus msvcurt.lib
 * (the OneCore vcruntime import library) and the shim in this directory.
 *
 * Measured on the delivered binaries (llvm-readobj --coff-imports):
 *
 *   libgallium_wgl.dll, vulkan_panfrost.dll
 *     VCRUNTIME140.dll : memcpy memset memmove memcmp memchr strchr strrchr
 *                        strstr longjmp _setjmpex (imported as
 *                        __intrinsic_setjmpex) __CxxFrameHandler3
 *     MSVCP140.dll     : _Mtx_lock _Mtx_unlock
 *                        std::_Throw_Cpp_error(std::) std::_Xlength_error
 *                        std::_Raise_handler
 *
 * VCRUNTIME140.dll / MSVCP140.dll are not reliably present on Windows 11
 * ARM64 (IoT/LTSC images in particular), and installing the Visual C++
 * redistributable is exactly what this project set out to avoid.  The UCRT
 * (ucrtbase.dll) *is* in-box, so only the symbols above have to be owned here.
 *
 * HOW IT IS WIRED IN
 * ------------------
 * The linker takes the first definition of a symbol it meets, so
 * tools\setup.ps1 and tools\build-runtime.ps1 put mali_cxxrt.lib *before*
 * msvcurt.lib in the link line.  For functions, clang-cl references the plain
 * symbol name even when the declaration is __declspec(dllimport) (measured),
 * so owning memcpy/strchr/_Mtx_lock/... here is enough; no __imp_* needed.
 *
 * - __CxxFrameHandler3 is *not* implemented -- it is avoided by compiling
 *   Mesa's C++ with exceptions disabled (/EHs-c-, added by setup.ps1).  Mesa
 *   already builds with _HAS_EXCEPTIONS=0 and contains no throw/catch.
 * - _setjmpex / longjmp are in mali_aarch64.s: the compiler only passes the
 *   jmp_buf address around, so this project owns both halves of the pair and
 *   is free to pick its own buffer layout.
 *
 * Compiled with /clang:-fno-builtin (see build-runtime.ps1) so the bodies below
 * are not treated as compiler builtins and the copy loops vectorise.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>

/* Declared here rather than pulled in from <stdlib.h>: this file must not drag
 * in <string.h>, whose dllimport declarations would clash with the definitions
 * below.  abort resolves against the in-box UCRT (ucrt.lib). */
#if defined(__cplusplus)
extern "C" {
#endif
__declspec(noreturn) void __cdecl abort(void);
#if defined(__cplusplus)
} /* extern "C" */
#endif

#if defined(__cplusplus)
extern "C" {
#endif

/* ------------------------------------------------------------------------- *
 * memory / string intrinsics
 *
 * These are the symbols the ARM64 MSVC toolchain keeps in vcruntime140.dll
 * rather than the UCRT.  Plain (non-vectorised) loops are deliberate: clang
 * auto-vectorises them at /O2, and that keeps this file free of any dependency
 * on the very routines it provides.
 * ------------------------------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d < s || d >= s + n) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    while (n--) {
        if (*p == ch)
            return (void *)(size_t)p;
        p++;
    }
    return NULL;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;
    for (;; s++) {
        if (*s == ch)
            return (char *)(size_t)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == ch)
            last = s;
        if (*s == '\0')
            return (char *)(size_t)last;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)(size_t)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)(size_t)haystack;
    }
    return NULL;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif

/* ------------------------------------------------------------------------- *
 * std::mutex support (MSVCP140.dll: _Mtx_lock / _Mtx_unlock / _Mtx_trylock)
 *
 * MSVC's <mutex> keeps the lock inline in the std::mutex object:
 *
 *   struct _Mtx_internal_imp_t {              // 80 bytes, align 8 (ARM64)
 *       int  _Type;                           //   0
 *       union {                               //   8
 *           _Stl_critical_section _Critical_section; // { void* _Unused;
 *                                                    //   void* _M_srw_lock; }
 *           _Aligned_storage_t<64, 8> _Cs_storage;
 *       };
 *       long _Thread_id;                      //  72 (long is 32-bit in MSVC)
 *       int  _Count;                          //  76
 *   };
 *
 * The constexpr constructor zero-initialises it (there is no call to
 * _Mtx_init_in_situ for this toolset), so vcruntime's _Mtx_lock is expected to
 * lazily build the real lock on first use.  Instead of vcruntime's malloc'd
 * SRWLOCK, this implementation stores the SRWLOCK *inline* in the 64-byte
 * storage area (at offset 8, where _Stl_critical_section::_Unused lives and
 * which nothing else reads).  A zero-initialised SRWLOCK is a valid unlocked
 * SRWLOCK, so there is no allocation and no init step at all.
 *
 * _Thrd_result: _Success=0 _Nomem=1 _Timedout=2 _Busy=3 _Error=4.
 * _Mtx_plain=0x01, _Mtx_try=0x02, _Mtx_timed=0x04, _Mtx_recursive=0x100.
 * ------------------------------------------------------------------------- */

#define MALI_MTX_RECURSIVE  0x100
#define MALI_THRD_SUCCESS   0
#define MALI_THRD_BUSY      3

typedef struct { void *ptr; } MALI_SRWLOCK;

#if defined(__cplusplus)
extern "C" {
#endif
__declspec(dllimport) void __stdcall AcquireSRWLockExclusive(MALI_SRWLOCK *);
__declspec(dllimport) unsigned char __stdcall TryAcquireSRWLockExclusive(MALI_SRWLOCK *);
__declspec(dllimport) void __stdcall ReleaseSRWLockExclusive(MALI_SRWLOCK *);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#if defined(__cplusplus)
} /* extern "C" */
#endif

/* Mirror of _Mtx_internal_imp_t; see the layout comment above. */
typedef struct {
    int   _Type;           /*   0 */
    int   _Pad;            /*   4 */
    void *_CsStorage[8];   /*   8 .. 71  (the union's 64 bytes) */
    long  _Thread_id;      /*  72 */
    int   _Count;          /*  76 */
} MALI_Mtx;

static MALI_SRWLOCK *mali_mtx_cs(MALI_Mtx *m)
{
    /* The SRWLOCK lives in the first 8 bytes of the union storage. */
    return (MALI_SRWLOCK *)&m->_CsStorage[0];
}

#if defined(__cplusplus)
extern "C" {
#endif

int _Mtx_init_in_situ(void *mtx, int type)
{
    MALI_Mtx *m = (MALI_Mtx *)mtx;
    m->_Type = type;
    m->_Thread_id = -1;
    m->_Count = 0;
    m->_CsStorage[0] = NULL;   /* SRWLOCK_INIT */
    return 0;
}

void _Mtx_destroy_in_situ(void *mtx)
{
    (void)mtx;
}

int _Mtx_lock(void *mtx)
{
    MALI_Mtx *m = (MALI_Mtx *)mtx;

    if (m->_Type & MALI_MTX_RECURSIVE) {
        unsigned long tid = GetCurrentThreadId();
        if (m->_Count > 0 && m->_Thread_id == (long)tid) {
            m->_Count++;
            return MALI_THRD_SUCCESS;
        }
        AcquireSRWLockExclusive(mali_mtx_cs(m));
        m->_Thread_id = (long)tid;
        m->_Count = 1;
        return MALI_THRD_SUCCESS;
    }

    AcquireSRWLockExclusive(mali_mtx_cs(m));
    return MALI_THRD_SUCCESS;
}

int _Mtx_trylock(void *mtx)
{
    MALI_Mtx *m = (MALI_Mtx *)mtx;

    if (m->_Type & MALI_MTX_RECURSIVE) {
        unsigned long tid = GetCurrentThreadId();
        if (m->_Count > 0 && m->_Thread_id == (long)tid) {
            m->_Count++;
            return MALI_THRD_SUCCESS;
        }
    }

    if (!TryAcquireSRWLockExclusive(mali_mtx_cs(m)))
        return MALI_THRD_BUSY;

    if (m->_Type & MALI_MTX_RECURSIVE) {
        m->_Thread_id = (long)GetCurrentThreadId();
        m->_Count = 1;
    }
    return MALI_THRD_SUCCESS;
}

int _Mtx_unlock(void *mtx)
{
    MALI_Mtx *m = (MALI_Mtx *)mtx;

    if (m->_Type & MALI_MTX_RECURSIVE) {
        if (--m->_Count > 0)
            return MALI_THRD_SUCCESS;
        m->_Thread_id = -1;
    }

    ReleaseSRWLockExclusive(mali_mtx_cs(m));
    return MALI_THRD_SUCCESS;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif

/* ------------------------------------------------------------------------- *
 * STL [[noreturn]] shims (MSVCP140.dll)
 *
 * Mesa builds with _HAS_EXCEPTIONS=0, so the STL's own implementations of these
 * reduce to a report-and-terminate.  abort() is the same observable behaviour,
 * and it keeps MSVCP140.dll out of the import table.
 * ------------------------------------------------------------------------- */

#if defined(__cplusplus)
namespace std {

[[noreturn]] void __cdecl _Xlength_error(const char *msg)
{
    (void)msg;
    abort();
}

[[noreturn]] void __cdecl _Throw_Cpp_error(int code)
{
    (void)code;
    abort();
}

} /* namespace std */
#endif
