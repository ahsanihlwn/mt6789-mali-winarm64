/*
 * linux/types.h  -  minimal <linux/types.h> stand-in for Windows MSVC/ARM64.
 *
 * include/drm-uapi/mali_kbase_ioctl.h (the FourFectVK stack's kbase uAPI,
 * derived from the Linux kernel's) is written in the kernel's fixed-width
 * spellings. Mesa's DRM headers only use __u32/__u64 in their __linux__
 * branch, so on Windows nothing else provides them.
 *
 * Windows' <stdint.h> types have the same widths on aarch64, which is all the
 * ABI needs; the kbase structs are laid out identically on both sides.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_LINUX_TYPES_H
#define MALI_COMPAT_LINUX_TYPES_H

#include <stdint.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;

typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

#endif /* MALI_COMPAT_LINUX_TYPES_H */
