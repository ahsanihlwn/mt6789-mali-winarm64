/*
 * poll.h  -  minimal <poll.h> stand-in for Windows MSVC/ARM64.
 *
 * src/util/libsync.h includes <poll.h> and its sync_wait() helper calls
 * poll(). libsync.h is pulled in by panthor_kmod.c (the CSF backend), which
 * Meson builds unconditionally even though this project selects the
 * panfrost/JM backend at runtime.
 *
 * The implementation lives in windrv/mesa/runtime/mali_drm.cpp. It only ever
 * sees our synthetic sync-file descriptors (which are always ready) or plain
 * CRT fds (which it reports as not ready). No caller depends on real fence
 * semantics: panthor is never selected on this hardware.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_POLL_H
#define MALI_COMPAT_POLL_H

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

int poll(struct pollfd *fds, unsigned long nfds, int timeout);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_POLL_H */