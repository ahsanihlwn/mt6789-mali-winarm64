/*
 * mali_win32os.cpp  -  Win32-backed implementations of Linux-only OS helpers
 *                      that Mesa's Panfrost/PanVK sources call directly.
 *
 * Part of the Jalur B shim (see windrv\PANVK-SHIM.md): Mesa is written for
 * Linux and includes a handful of headers with no Windows equivalent. The
 * project-local compat include dir supplies the *declarations*
 * (cross\compat\sys\*), and this member supplies the *definitions* so the
 * declarations stay out of Mesa itself.
 *
 * Members:
 *   sysinfo()    <- <sys/sysinfo.h>; RAM sizing for PanVK's memory heap.
 *   sched_yield()<- <sched.h>; yield inside vk_drm_syncobj's wait retry loop.
 *   _tls_index   <- vcruntime.lib's TLS slot index for __declspec(thread).
 *
 * Linked into mali_cxxrt.lib, which every Mesa target already links (it is the
 * CRT-free link recipe in tools\setup.ps1), so a translation unit that calls
 * one of these drags in only this member.
 *
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>

#include <string.h>

#include "sched.h"
#include "sys/sysinfo.h"

extern "C" int sysinfo(struct sysinfo *info)
{
    if (info == nullptr)
        return -1;

    memset(info, 0, sizeof(*info));

    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms))
        return -1;

    info->totalram = ms.ullTotalPhys;
    info->freeram  = ms.ullAvailPhys;
    info->mem_unit = 1;
    info->procs    = 1;
    return 0;
}

/*
 * sched_yield()  -  <sched.h>, absent from MSVC.
 *
 * Windows' SwitchToThread() yields to another ready thread on the current
 * processor, which is what sched_yield() means for Mesa's vk_drm_syncobj wait
 * retry loops. Sleep(0) is the coarse fallback when no other thread is ready.
 */
extern "C" int sched_yield(void)
{
    if (!SwitchToThread())
        Sleep(0);
    return 0;
}

/*
 * _tls_index  -  MSVC's per-module index into the Windows TLS array.
 *
 * The real symbol lives in vcruntime.lib, which is not installed for ARM64
 * here, so any translation unit using __declspec(thread) fails to link:
 *
 *     lld-link: error: undefined symbol: _tls_index
 *
 * Measured 2026-11-09 on Mesa's src/util/os_misc.c, whose Windows
 * os_get_option() keeps its result buffer in `static thread_local char[]`.
 *
 * The Windows loader fills this in: the linker emits an IMAGE_TLS_DIRECTORY
 * for the image with AddressOfIndex pointing here, and the loader assigns the
 * module's TLS slot and allocates its per-thread block. Defining the symbol is
 * all that is missing; MSVC TLS needs no startup code for statically
 * initialised (POD) thread_local objects.
 */
extern "C" unsigned long _tls_index = 0;
