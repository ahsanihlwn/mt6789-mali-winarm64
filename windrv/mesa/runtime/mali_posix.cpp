/*
 * mali_posix.cpp  -  POSIX/oldnames aliases for the CRT-free ARM64 link recipe.
 *
 * WHY THIS EXISTS
 * ---------------
 * ucrt.lib exports the underscored UCRT names (_read, _write, _close, _open,
 * _lseek, _unlink, _access, _isatty, _getpid ...) but not their POSIX
 * spellings. On a normal MSVC install the POSIX spellings come from
 * oldnames.lib. Measured 2026-11-09: the Windows SDK here ships no arm64
 * oldnames.lib (the um/ucrt/arm64 dirs contain none), so any code that calls
 * read()/write()/close()/open() fails with
 *
 *     lld-link: error: undefined symbol: __declspec(dllimport) read
 *
 * That is what stopped Mesa's bundled zlib (gzguts.h only maps open-> _open
 * under WINAPI_FAMILY, i.e. UWP, not on the desktop). This member supplies the
 * aliases so such code links. It will also serve the upcoming libdrm/Win32 shim
 * (PANVK-SHIM.md), which needs open/read/write.
 *
 * The references are imports (`__imp_read`), so we define both the function and
 * the import pointer. We deliberately do NOT include <io.h>/<fcntl.h>: they
 * declare these names as __declspec(dllimport), and defining a dllimport
 * function is an error. Declaring the underscored targets by hand avoids that.
 *
 * SPDX-License-Identifier: MIT
 */

/* --- UCRT targets (underscored names, present in ucrt.lib) ---------------- */
#include <stddef.h>

extern "C" int  __cdecl _read(int fd, void *buf, unsigned int count);
extern "C" int  __cdecl _write(int fd, const void *buf, unsigned int count);
extern "C" int  __cdecl _close(int fd);
extern "C" long __cdecl _lseek(int fd, long offset, int origin);
extern "C" int  __cdecl _open(const char *path, int flags, int mode);
extern "C" int  __cdecl _unlink(const char *path);
extern "C" int  __cdecl _access(const char *path, int mode);
extern "C" int  __cdecl _isatty(int fd);
extern "C" int  __cdecl _getpid(void);
extern "C" int  __cdecl _dup(int fd);
extern "C" int  __cdecl _dup2(int oldfd, int newfd);
extern "C" char *__cdecl _strdup(const char *s);
extern "C" int  __cdecl _stricmp(const char *a, const char *b);
extern "C" int  __cdecl _strnicmp(const char *a, const char *b, size_t n);

#define MALI_ALIAS(ret, name, target, params, args)      \
    extern "C" ret __cdecl name params { return target args; } \
    extern "C" ret(__cdecl *__imp_##name) params = name;

/* read and write keep the UCRT signatures, except that read() moved to
 * mali_drm.cpp: the kbase device fd delivers completion records through it, so
 * it has to recognise synthetic fds (like open/close below). */
MALI_ALIAS(int,  write, _write, (int fd, const void *buf, unsigned int count), (fd, buf, count))
MALI_ALIAS(long, lseek, _lseek, (int fd, long offset, int origin), (fd, offset, origin))
MALI_ALIAS(int,  unlink, _unlink, (const char *path), (path))
MALI_ALIAS(int,  access, _access, (const char *path, int mode), (path, mode))
MALI_ALIAS(int,  isatty, _isatty, (int fd), (fd))
MALI_ALIAS(int,  getpid, _getpid, (void), ())
/* wrk_drm / vk_drm_syncobj.c reach these through their POSIX spellings, which
 * the UCRT only exports underscored (oldnames.lib, absent for arm64 here). */
MALI_ALIAS(int,   dup,    _dup,    (int fd), (fd))
MALI_ALIAS(int,   dup2,   _dup2,   (int oldfd, int newfd), (oldfd, newfd))
MALI_ALIAS(char *, strdup, _strdup, (const char *s), (s))
MALI_ALIAS(int,   stricmp, _stricmp, (const char *a, const char *b), (a, b))
/* <strings.h> spellings (BSD): panfrost's pan_env.h calls strcasecmp() to
 * parse the PAN_* knobs, and clang-cl has no <strings.h> - see
 * cross/compat/strings.h. */
MALI_ALIAS(int,   strcasecmp,  _stricmp,  (const char *a, const char *b), (a, b))
MALI_ALIAS(int,   strncasecmp, _strnicmp, (const char *a, const char *b, size_t n), (a, b, n))

/* open() and close() live in mali_drm.cpp because they must distinguish the
 * synthetic DRM render node (and sync-file fds) from ordinary CRT files. */
