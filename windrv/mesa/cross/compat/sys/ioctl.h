/*
 * sys/ioctl.h  -  minimal <sys/ioctl.h> stand-in for Windows MSVC/ARM64.
 *
 * src/util/libsync.h includes <sys/ioctl.h> for the Android/Linux sync-file
 * helpers (sync_merge, sync_valid_fd). Those helpers are unused by this
 * project (the panfrost/JM backend uses DRM sync objects instead), but the
 * header must parse.
 *
 * The bulk of the DRM request encoding lives in sys/ioccom.h, which drm.h
 * includes directly. This header just re-exports it and declares ioctl().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_SYS_IOCTL_H
#define MALI_COMPAT_SYS_IOCTL_H

#include "ioccom.h"

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_SYS_IOCTL_H */