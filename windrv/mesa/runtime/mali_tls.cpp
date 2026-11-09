/*
 * mali_tls.cpp  -  MSVC thread-local-storage and thread-safe-static runtime
 *                  support for the CRT-free ARM64 Windows link recipe.
 *
 * WHY THIS EXISTS
 * ---------------
 * Two families of symbols that normally live in vcruntime.lib are emitted by
 * clang for ordinary C++/C code, and the ARM64 MSVC runtime is not installed on
 * this host:
 *
 *   1. C++11 thread-safe function-local statics ("magic statics").  Measured
 *      2026-11-09 on Mesa's src/util/texcompress_astc_luts.cpp
 *      (Granite::get_astc_luts), which produced:
 *
 *          undefined symbol: _Init_thread_epoch
 *          undefined symbol: _Init_thread_header / _footer / _abort
 *
 *   2. Dynamic/TLS-guard support for thread_local variables.  clang guards
 *      every access with
 *
 *          if (!__tls_guard) __dyn_tls_on_demand_init();
 *
 *      Measured 2026-11-09 on Mesa's src/mapi/glapi/glapi_dispatch.c (4445
 *      references: every GL entry point reads the thread-local dispatch table).
 *
 * Both are reimplemented here from the shipped MSVC CRT sources so the
 * behaviour matches the compiler's expectations exactly:
 *
 *   crt/src/vcruntime/thread_safe_statics.cpp   (the epoch/lock protocol)
 *   crt/src/vcruntime/tlsdyn.cpp                (__dyn_tls_init / guard)
 *
 * Thread-safe statics
 * -------------------
 * A static's guard word holds the epoch at which it was initialised: 0 means
 * uninitialised, -1 means "being initialised", anything else is the global
 * epoch counter value observed when initialisation finished.  The first thread
 * to see 0 claims it (-1), runs the initialiser and publishes _Init_global_epoch
 * (incrementing it).  Others spin until it is published, then record the epoch
 * in their thread-local copy so subsequent checks are a single load.  A per
 * process lock serialises the transition; the wait releases it.
 *
 * TLS guard
 * ---------
 * When this DLL is loaded after threads already exist, the OS allocates a TLS
 * block for them but does not run per-variable initialisers.  clang therefore
 * tests a thread-local __tls_guard before each thread_local access and calls
 * __dyn_tls_on_demand_init() once per thread.  That walks the .CRT$XD* array of
 * initialiser function pointers (the same convention as MSVC).  Mesa's GL/DRI
 * thread_local objects here are statically initialised (the value lives in the
 * .tls template), so the array is empty and this simply sets the guard; the
 * mechanism is implemented in full so any future dynamic thread_local works.
 *
 * The image's TLS directory itself is synthesised by lld-link from the .tls
 * section (it already points AddressOfIndex at _tls_index, defined in
 * mali_win32os.cpp), so no _tls_used is needed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>
#include <intrin.h>
#include <limits.h>

extern "C" unsigned long _tls_index;   /* defined in mali_win32os.cpp */

/* ==================================================================== */
/* Thread-safe static initialization                                     */
/* ==================================================================== */

static const int tss_uninitialized    = 0;
static const int tss_being_initialized = -1;

extern "C" {
int _Init_global_epoch = INT_MIN;
__declspec(thread) int _Init_thread_epoch = INT_MIN;
}

/*
 * A process-wide lock for static initialization.  A plain spin lock is enough:
 * it is only held while flipping a guard word, never across the initialiser
 * body (the initialising thread keeps the state "being initialized" instead of
 * the lock), so it is never held across a syscall or a GPU wait.
 */
static volatile long g_tss_lock = 0;

static void tss_lock(void)
{
    while (_InterlockedCompareExchange(&g_tss_lock, 1, 0) != 0)
        SwitchToThread();
}

static void tss_unlock(void)
{
    _InterlockedExchange(&g_tss_lock, 0);
}

/* Exported for parity with the MSVC CRT; the header/footer below use them. */
extern "C" void __cdecl _Init_thread_lock(void)   { tss_lock(); }
extern "C" void __cdecl _Init_thread_unlock(void) { tss_unlock(); }

extern "C" void __cdecl _Init_thread_wait_v2(void)
{
    /* Release the lock so the initializing thread can make progress, then let
     * the caller re-check the guard word.  The MSVC version waits on a
     * condition variable; polling keeps this shim free of extra libraries. */
    tss_unlock();
    Sleep(1);
    tss_lock();
}

extern "C" void __cdecl _Init_thread_notify(void)
{
    /* Nothing to signal: _Init_thread_wait_v2() polls. */
}

extern "C" void __cdecl _Init_thread_header(int *pOnce)
{
    _Init_thread_lock();

    if (*pOnce == tss_uninitialized) {
        *pOnce = tss_being_initialized;
    } else {
        while (*pOnce == tss_being_initialized) {
            _Init_thread_wait_v2();

            if (*pOnce == tss_uninitialized) {
                *pOnce = tss_being_initialized;
                _Init_thread_unlock();
                return;
            }
        }
        _Init_thread_epoch = _Init_global_epoch;
    }

    _Init_thread_unlock();
}

extern "C" void __cdecl _Init_thread_abort(int *pOnce)
{
    _Init_thread_lock();
    *pOnce = tss_uninitialized;
    _Init_thread_unlock();
    _Init_thread_notify();
}

extern "C" void __cdecl _Init_thread_footer(int *pOnce)
{
    _Init_thread_lock();
    ++_Init_global_epoch;
    *pOnce = _Init_global_epoch;
    _Init_thread_epoch = _Init_global_epoch;
    _Init_thread_unlock();
    _Init_thread_notify();
}

/* ==================================================================== */
/* TLS guard / on-demand dynamic initialization                          */
/* ==================================================================== */

typedef void (__cdecl *_mali_pvfv)(void);

/*
 * Bounds of the array the compiler fills with per-thread initialiser function
 * pointers for dynamically-initialized thread_local objects (.CRT$XD*, the
 * same section convention MSVC uses).  With statically-initialized TLS the
 * range is empty and the walk below is a no-op.
 */
extern "C" {
__declspec(allocate(".CRT$XDA")) _mali_pvfv __xd_a = nullptr;
__declspec(allocate(".CRT$XDZ")) _mali_pvfv __xd_z = nullptr;
}

/* Per-thread flag the compiler tests before touching a guarded thread_local. */
extern "C" __declspec(thread) bool __tls_guard = false;

static void mali_run_tls_initializers(void)
{
    for (_mali_pvfv *p = &__xd_a + 1; p != &__xd_z; ++p) {
        if (*p)
            (*p)();
    }
}

extern "C" void __cdecl __dyn_tls_on_demand_init(void)
{
    if (__tls_guard)
        return;

    /* Set the guard before running initializers, exactly as the CRT does, so a
     * re-entrant access during initialization does not recurse. */
    __tls_guard = true;
    mali_run_tls_initializers();
}
