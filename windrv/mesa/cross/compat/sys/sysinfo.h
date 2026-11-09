/*
 * sys/sysinfo.h  -  Linux sysinfo() for Windows/ARM64 (MSVC).
 *
 * Mesa's src/panfrost/vulkan/panvk_physical_device.c includes <sys/sysinfo.h>
 * to size the single Vulkan memory heap from the machine's RAM. Windows has no
 * such header, so the compat include dir (-I windrv\mesa\cross\compat) supplies
 * this one and windrv\mesa\runtime\mali_win32os.cpp implements sysinfo() on top
 * of GlobalMemoryStatusEx().
 *
 * The struct layout is deliberately *not* glibc's: it is private to this
 * header + implementation pair, and only ever read by the PanVK caller, which
 * uses totalram and mem_unit. Both are 64-bit here so that a RAM size above
 * 4 GiB is reported correctly (MSVC 'long' is 32-bit, glibc 'long' is 64-bit on
 * LP64).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_SYS_SYSINFO_H
#define MALI_COMPAT_SYS_SYSINFO_H

struct sysinfo {
    unsigned long long totalram;   /* total usable RAM, in units of mem_unit */
    unsigned long long freeram;
    unsigned long long sharedram;
    unsigned long long bufferram;
    unsigned long long totalswap;
    unsigned long long freeswap;
    unsigned long long totalhigh;
    unsigned long long freehigh;
    unsigned long long mem_unit;   /* size of a memory unit in bytes */
    unsigned short procs;
    unsigned short _pad;
};

#ifdef __cplusplus
extern "C" {
#endif

int sysinfo(struct sysinfo *info);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_SYS_SYSINFO_H */
