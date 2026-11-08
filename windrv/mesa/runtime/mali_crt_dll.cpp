/*
 * mali_crt_dll.cpp  -  the minimal DLL entry point for the CRT-free ARM64 link
 *                      recipe (Jalur B).
 *
 * WHY THIS EXISTS
 * ---------------
 * lld-link's default entry for /dll is DllMainCRTStartup. Normally that lives
 * in libcmt/vcruntime, which are not installed here. Without it a shared
 * library fails to link ("entry point must be defined" / "undefined symbol:
 * DllMainCRTStartup"). This provides it and forwards to the library's own
 * DllMain if it has one.
 *
 * `DllMain` is optional, so it is resolved through /alternatename to a
 * default that reports success. Like mali_crt_exe.cpp this is its own archive
 * member so only DLLs pull it in.
 *
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>

extern "C" int __stdcall DllMain(HINSTANCE, unsigned long, void *);
extern "C" int __stdcall mali_crt_default_dllmain(HINSTANCE, unsigned long, void *)
{
    return TRUE;
}
#pragma comment(linker, "/alternatename:DllMain=mali_crt_default_dllmain")

/* lld-link's default entry for /dll is _DllMainCRTStartup (the CRT's name). */
extern "C" int __stdcall _DllMainCRTStartup(HINSTANCE inst, unsigned long reason, void *reserved)
{
    return DllMain(inst, reason, reserved);
}
