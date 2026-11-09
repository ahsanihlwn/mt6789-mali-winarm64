/*
 * sched.h  -  the single POSIX scheduling call Mesa needs, for Windows/ARM64.
 *
 * MSVC and the Windows SDK ship no <sched.h>. Mesa's
 * src/vulkan/runtime/vk_drm_syncobj.c includes it and calls sched_yield() in
 * the retry loops of vk_drm_syncobj_wait() (25.0.7), so the compat include dir
 * (-I windrv\mesa\cross\compat) supplies the declaration and
 * windrv\mesa\runtime\mali_win32os.cpp implements it on SwitchToThread().
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_SCHED_H
#define MALI_COMPAT_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

int sched_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_SCHED_H */
