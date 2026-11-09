/*
 * arm64-msvc-atomic.h  -  lower-case MSVC interlocked intrinsics for ARM64.
 *
 * Force-included (clang-cl /FI) into every Mesa translation unit by the
 * generated cross file. See tools\setup.ps1.
 *
 * WHY
 * ---
 * Mesa's src/util/u_atomic.h takes the MSVC-intrinsic path whenever _MSC_VER is
 * defined, and uses the lower-case intrinsic spellings that MSVC's own
 * <intrin.h> provides on x86/x64:
 *
 *     _interlockedincrement64  _interlockeddecrement64
 *     _interlockedexchangeadd64 _interlockedexchange64
 *     _interlockedadd64         _interlockedadd
 *
 * clang's ARM64 <intrin.h> only declares the capitalised forms (measured: clang
 * 20.1.8, intrin0.h lines 76-80 and intrin.h lines 375-376, both under
 * __aarch64__), so the build stops with:
 *
 *     error: call to undeclared function '_interlockedexchange64'
 *
 * Mapping the lower-case names onto the capitalised builtins fixes that without
 * patching Mesa. _interlockedadd8/_interlockedadd16 are deliberately NOT
 * defined here: u_atomic.h defines them itself.
 *
 * C and C++ safe; no effect on non-ARM64 or non-MSVC targets.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_ARM64_MSVC_ATOMIC_H
#define MALI_ARM64_MSVC_ATOMIC_H

#if defined(_MSC_VER) && defined(_M_ARM64)

#include <intrin.h>

static __forceinline __int64 _interlockedincrement64(__int64 volatile *_Addend)
{
    return _InterlockedIncrement64(_Addend);
}

static __forceinline __int64 _interlockeddecrement64(__int64 volatile *_Addend)
{
    return _InterlockedDecrement64(_Addend);
}

static __forceinline __int64 _interlockedexchangeadd64(__int64 volatile *_Addend,
                                                       __int64 _Value)
{
    return _InterlockedExchangeAdd64(_Addend, _Value);
}

static __forceinline __int64 _interlockedexchange64(__int64 volatile *_Target,
                                                    __int64 _Value)
{
    return _InterlockedExchange64(_Target, _Value);
}

static __forceinline __int64 _interlockedadd64(__int64 volatile *_Addend,
                                               __int64 _Value)
{
    return _InterlockedAdd64(_Addend, _Value);
}

static __forceinline long _interlockedadd(long volatile *_Addend, long _Value)
{
    return _InterlockedAdd(_Addend, _Value);
}

#endif /* _MSC_VER && _M_ARM64 */

#endif /* MALI_ARM64_MSVC_ATOMIC_H */