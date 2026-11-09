/*
 * linux/ioctl.h  -  minimal <linux/ioctl.h> stand-in for Windows MSVC/ARM64.
 *
 * The FourFectVK v9 stack is written against the Linux kbase uAPI: its
 * src/panfrost/lib/kmod/kbase_kmod.c, src/panfrost/vulkan/{jm,csf}/
 * panvk_vX_gpu_queue.c and include/drm-uapi/mali_kbase_ioctl.h all include
 * <linux/ioctl.h> and <linux/types.h> unconditionally. Neither exists for
 * MSVC, and with __linux__ undefined drm.h takes its BSD branch instead, so
 * these two headers are what make the kbase sources parse on Windows.
 *
 * The encoding macros are the same ones sys/ioccom.h defines: Linux's
 * <asm-generic/ioctl.h> and BSD's <sys/ioccom.h> agree on the layout. This is
 * load-bearing rather than cosmetic, because runtime/mali_drm.cpp decodes the
 * kbase requests by value - a mismatch here would misroute every ioctl.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_LINUX_IOCTL_H
#define MALI_COMPAT_LINUX_IOCTL_H

#include <sys/ioccom.h>

#endif /* MALI_COMPAT_LINUX_IOCTL_H */
