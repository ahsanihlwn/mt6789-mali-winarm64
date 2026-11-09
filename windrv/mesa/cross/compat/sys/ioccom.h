/*
 * sys/ioccom.h  -  minimal BSD <sys/ioccom.h> stand-in for Windows.
 *
 * Mesa's include/drm-uapi/drm.h has two branches: __linux__ (needs
 * <linux/types.h> + <asm/ioctl.h>) and "one of the BSDs" (needs
 * <sys/ioccom.h> + <sys/types.h>). Neither exists for MSVC/ARM64, and with
 * __linux__ undefined it takes the BSD branch, so the build stops at
 *
 *     include/drm-uapi/drm.h(47,10): fatal error: 'sys/ioccom.h' file not found
 *
 * (reached via drm_fourcc.h from wsi_common_headless.c). The fourcc constants
 * are all this build wants; nothing ever issues a DRM ioctl on Windows. So the
 * encoding macros below only have to exist and expand to an integer.
 *
 * The values follow Linux's <asm-generic/ioctl.h>, which is the convention the
 * BSD headers are compatible with. <sys/types.h> comes from the UCRT.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_SYS_IOCCOM_H
#define MALI_COMPAT_SYS_IOCCOM_H

#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))

#define _IO(type, nr)       _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size) \
    _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(size))

#define _IOC_DIR(nr)  (((nr) >> _IOC_DIRSHIFT) & ((1U << (_IOC_DIRSHIFT - _IOC_SIZESHIFT)) - 1))
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & ((1U << _IOC_TYPEBITS) - 1))
#define _IOC_NR(nr)   (((nr) >> _IOC_NRSHIFT) & ((1U << _IOC_NRBITS) - 1))
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & ((1U << _IOC_SIZEBITS) - 1))

#endif /* MALI_COMPAT_SYS_IOCCOM_H */