/*
 * mali_cxxrt.cpp  -  the small ARM64 MSVC C++ runtime that Jalur B needs.
 *
 * WHY THIS EXISTS
 * ---------------
 * A host with the Windows SDK but without the MSVC ARM64 C/C++ runtime
 * libraries can still link C and *plain* C++ ARM64 code: see
 * windrv/mesa/tools/link-probe.ps1 and mesa/README.md. What it cannot link is
 * code that uses the MSVC standard library (STL), because a few symbols live
 * in libcpmt.lib, which is part of the missing workload.
 *
 * Measured on 2026-11-09, `ucrt.lib + BufferOverflow.lib + msvcurt.lib +
 * kernel32.lib + ntdll.lib`, with the STL compiled `_HAS_EXCEPTIONS=0 /GR-`
 * (which removes the hard SEH machinery - __CxxFrameHandler3, _CxxThrowException,
 * __std_exception_copy/destroy, type_info::vftable - and avoids msvcurt.lib's
 * broken RTTI member), leaves exactly four symbols undefined:
 *
 *     atexit
 *     std::_Raise_handler
 *     void * __cdecl operator new(unsigned __int64)
 *     void __cdecl operator delete(void *, unsigned __int64)
 *
 * Everything else the STL needs (_Mtx_lock/_Mtx_unlock, std::_Xlength_error,
 * std::_Throw_Cpp_error, ...) is already defined by msvcurt.lib. This file
 * supplies the remaining four, and the other operator new/delete forms the STL
 * can reach, so that Mesa's mandatory C++ (src/util/u_qsort.cpp,
 * texcompress_astc*.cpp) links.
 *
 * SAFETY
 * ------
 * These are the ordinary definitions the CRT would have provided; they do not
 * try to reimplement exceptions or the STL. The global operator new/delete use
 * the UCRT's malloc/free. atexit forwards to the UCRT's per-module
 * _crt_atexit, so handlers run at process exit / DLL unload exactly as usual.
 * _Raise_handler is the null handler the non-throwing STL expects.
 *
 * Build with: windrv\mesa\tools\build-runtime.ps1
 *
 * SPDX-License-Identifier: MIT
 */

#include <cstddef>
#include <cstdlib>
#include <new>

/* ------------------------------------------------------------------------- */
/* Global operator new / delete                                              */
/*                                                                           */
/* malloc/free come from the dynamic UCRT (ucrt.lib -> ucrtbase.dll), which  */
/* is present on Windows 11 ARM64. The throwing forms terminate instead of   */
/* throwing, because this shim is only used with _HAS_EXCEPTIONS=0 builds.   */
/* ------------------------------------------------------------------------- */

static void *mali_cxxrt_alloc(std::size_t n)
{
    void *p = std::malloc(n ? n : 1);
    if (!p)
        std::abort();
    return p;
}

void *__cdecl operator new(std::size_t n)
{
    return mali_cxxrt_alloc(n);
}

void *__cdecl operator new[](std::size_t n)
{
    return mali_cxxrt_alloc(n);
}

void *__cdecl operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    return std::malloc(n ? n : 1);
}

void *__cdecl operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
    return std::malloc(n ? n : 1);
}

void __cdecl operator delete(void *p) noexcept
{
    std::free(p);
}

void __cdecl operator delete[](void *p) noexcept
{
    std::free(p);
}

void __cdecl operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}

void __cdecl operator delete[](void *p, std::size_t) noexcept
{
    std::free(p);
}

void __cdecl operator delete(void *p, const std::nothrow_t &) noexcept
{
    std::free(p);
}

void __cdecl operator delete[](void *p, const std::nothrow_t &) noexcept
{
    std::free(p);
}

/* C++17 aligned allocation. Over-aligned blocks are rare in Mesa, but the STL
 * can reach them; use the UCRT's _aligned_malloc/_aligned_free. */
#if defined(__cpp_aligned_new)
extern "C" void *__cdecl _aligned_malloc(std::size_t, std::size_t);
extern "C" void  __cdecl _aligned_free(void *);

void *__cdecl operator new(std::size_t n, std::align_val_t a)
{
    void *p = _aligned_malloc(n ? n : 1, static_cast<std::size_t>(a));
    if (!p)
        std::abort();
    return p;
}

void *__cdecl operator new[](std::size_t n, std::align_val_t a)
{
    return ::operator new(n, a);
}

void __cdecl operator delete(void *p, std::align_val_t) noexcept
{
    _aligned_free(p);
}

void __cdecl operator delete[](void *p, std::align_val_t) noexcept
{
    _aligned_free(p);
}

void __cdecl operator delete(void *p, std::size_t, std::align_val_t a) noexcept
{
    (void)a;
    _aligned_free(p);
}

void __cdecl operator delete[](void *p, std::size_t, std::align_val_t a) noexcept
{
    (void)a;
    _aligned_free(p);
}
#endif /* __cpp_aligned_new */

/* ------------------------------------------------------------------------- */
/* atexit                                                                    */
/*                                                                           */
/* ucrt.lib does not export a plain `atexit'; it exports _crt_atexit, which   */
/* registers with the calling module's onexit table (and therefore runs at    */
/* process exit for an exe, or DLL unload for a DLL). Forwarding is exactly   */
/* the behaviour the CRT would give.                                         */
/* ------------------------------------------------------------------------- */

extern "C" int __cdecl _crt_atexit(void(__cdecl *)(void));

extern "C" int __cdecl atexit(void(__cdecl *fn)(void))
{
    return _crt_atexit(fn);
}

/* ------------------------------------------------------------------------- */
/* std::_Raise_handler                                                       */
/*                                                                           */
/* With _HAS_EXCEPTIONS=0, <exception> declares this as an imported data      */
/* symbol (see VC\Tools\MSVC\...\include\exception line ~81). The DllImport    */
/* import lives in msvcurt.lib; when the STL is linked without _DLL it wants  */
/* the plain data symbol, so provide it. The non-throwing STL never calls it. */
/* ------------------------------------------------------------------------- */

namespace std {
_Prhand _Raise_handler = nullptr;
}

/* ------------------------------------------------------------------------- */
/* the data-import slot for std::_Raise_handler                              */
/*                                                                           */
/* <exception> declares _Raise_handler __declspec(dllimport), so every         */
/* referencing C++ translation unit loads its address from the import slot     */
/* __imp_?_Raise_handler@std@@... instead of naming the variable directly.     */
/* That name cannot be spelled as a C++ identifier ('?' and '@'), so it is     */
/* given an explicit assembler name with __asm__, and its contents are -- as a */
/* data import slot must be -- the address of the definition above.            */
/*                                                                           */
/* Measured 2026-11-09: without this slot libgallium_wgl.dll still imports     */
/* MSVCP140.dll for this one symbol (referenced by lower_precision.cpp.obj     */
/* and texcompress_astc_luts.cpp.obj).                                         */
/* ------------------------------------------------------------------------- */

extern "C" void *mali_raise_handler_imp
    __asm__("__imp_?_Raise_handler@std@@3P6AXAEBVexception@stdext@@@ZEA")
    = (void *)&std::_Raise_handler;
