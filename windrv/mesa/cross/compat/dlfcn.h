/*
 * dlfcn.h  -  <dlfcn.h> stand-in for Windows MSVC/ARM64.
 *
 * src/panfrost/vulkan/panvk_physical_device.c includes <dlfcn.h>
 * unconditionally and uses exactly one thing from it: dladdr(), to find the
 * directory this DLL sits in so it can read a "meta.json" name beside it
 * (FourFectVK's device-name string). That is implemented in
 * windrv/mesa/runtime/mali_drm.cpp on top of GetModuleHandleExA +
 * GetModuleFileNameA.
 *
 * dlopen()/dlsym()/dlclose()/dlerror() are declarations only, and are here for
 * the same reason the include is: the tree must type-check. Nothing in this
 * build calls them; the real users (zink's RenderDoc probe, the drm-shim, the
 * Gallium "intercept" layer) are not built for this target. If one of them
 * ever is, it must be translated onto LoadLibraryA/GetProcAddress -- Mesa
 * already has os_dlopen() in src/util/os_misc.c for that -- and the link will
 * fail loudly rather than quietly doing nothing.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_DLFCN_H
#define MALI_COMPAT_DLFCN_H

#include <stddef.h>

/* glibc values; only RTLD_LAZY/NOW/GLOBAL/LOCAL/NOLOAD are ever referenced. */
#ifndef RTLD_LAZY
#define RTLD_LAZY     0x00001
#define RTLD_NOW      0x00002
#define RTLD_NOLOAD   0x00004
#define RTLD_GLOBAL   0x00100
#define RTLD_LOCAL    0
#define RTLD_NODELETE 0x01000
#endif

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT    ((void *)-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dladdr() result, laid out like glibc's. dli_fname points at storage owned by
 * the implementation and stays valid until the next call from the same thread;
 * dli_sname/dli_saddr are always NULL here because we have no symbol table.
 */
typedef struct {
   const char *dli_fname;
   void       *dli_fbase;
   const char *dli_sname;
   void       *dli_saddr;
} Dl_info;

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);
int   dladdr(const void *addr, Dl_info *info);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_DLFCN_H */