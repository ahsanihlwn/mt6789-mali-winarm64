/*
 * sys/sysmacros.h  -  glibc-compatible major()/minor()/makedev() for MSVC.
 *
 * Force-included nothing: this is reached through the project-local compat
 * include dir (-I windrv\mesa\cross\compat) that tools\setup.ps1 adds to the
 * cross file. Windows has no <sys/sysmacros.h>, but Mesa's
 * src/panfrost/vulkan/panvk_physical_device.c includes it to report the DRM
 * primary/render node numbers in VkPhysicalDeviceDrmPropertiesEXT.
 *
 * The values it formats come from stat() on the synthetic render node our libdrm
 * shim publishes (see windrv\mesa\runtime\mali_drm.cpp), i.e. they are ours to
 * choose. glibc's 64-bit encoding is used so the output looks like a normal
 * Linux DRM node (major 226 = /dev/dri on Linux).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_SYS_SYSMACROS_H
#define MALI_COMPAT_SYS_SYSMACROS_H

static inline unsigned int mali_dev_major(unsigned long long dev)
{
    return (unsigned int)(((dev >> 8) & 0xfffu) | ((dev >> 32) & ~0xfffu));
}

static inline unsigned int mali_dev_minor(unsigned long long dev)
{
    return (unsigned int)((dev & 0xffu) | ((dev >> 12) & ~0xffu));
}

#define major(dev) mali_dev_major((unsigned long long)(dev))
#define minor(dev) mali_dev_minor((unsigned long long)(dev))
#define makedev(maj, min)                                                      \
    ((((unsigned long long)(maj) & 0xfffu) << 8) |                             \
     (((unsigned long long)(maj) & ~0xfffu) << 32) |                           \
     (((unsigned long long)(min) & 0xffu)) |                                   \
     ((((unsigned long long)(min) & ~0xffu)) << 12))

#endif /* MALI_COMPAT_SYS_SYSMACROS_H */
