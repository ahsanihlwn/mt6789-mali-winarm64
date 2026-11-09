/*
 * pthread.h  -  include stub for Mesa sources that #include <pthread.h>.
 *
 * Most of Mesa guards pthread usage behind HAVE_PTHREAD, which Meson leaves
 * undefined for the MSVC/Windows target, so those includes never happen.  A few
 * shared headers include <pthread.h> unconditionally, though -- measured on
 * src/gallium/drivers/panfrost/pan_bo.c (the BO-cache mutex).
 *
 * The actual pthread_mutex_t type and its init/destroy/lock/trylock/unlock
 * helpers live in the force-included cross/msvc-compat.h: they must be visible
 * in every translation unit (pan_blend.h and pan_bo.h embed a mutex), not only
 * in the ones that happen to include <pthread.h>.  This header therefore only
 * exists so the #include resolves; it re-includes the compat layer for clarity.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_PTHREAD_H
#define MALI_COMPAT_PTHREAD_H

#include "../msvc-compat.h"

#endif /* MALI_COMPAT_PTHREAD_H */
