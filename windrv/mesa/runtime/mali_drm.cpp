/*
 * mali_drm.cpp  -  libdrm-compatible shim over the Mali KMD (Windows ARM64).
 *
 * WHY THIS EXISTS
 * ---------------
 * Mesa's PanVK panfrost (Job Manager) backend is written against Linux DRM:
 * it opens /dev/dri/renderD128, enumerates devices with drmGetDevices2(),
 * creates/maps buffer objects with the DRM_IOCTL_PANFROST_* ioctls, and
 * synchronises with DRM sync objects. None of that exists on Windows.
 *
 * This file supplies exactly that surface and translates it onto our own
 * kernel driver through windrv/umd's MaliUmd* API (mali_kmd_ioctl.h, ABI v2).
 * Mesa itself is not modified: it keeps calling libdrm and gets a working
 * render node. See windrv/PANVK-SHIM.md for the design discussion.
 *
 * What is emulated
 *   - one synthetic DRM device, /dev/dri/renderD128, backed by the Mali KMD
 *   - drmVersion -> "panfrost" (so Mesa picks the panfrost KMOD backend)
 *   - DRM_IOCTL_PANFROST_{CREATE_BO,GET_BO_OFFSET,MMAP_BO,GET_PARAM,WAIT_BO,
 *     MADVISE,SUBMIT}
 *   - DRM sync objects (binary + timeline), entirely in user mode
 *   - mmap/munmap of buffer objects: returns the KMD's existing CPU mapping
 *
 * Why sync objects can live in user mode
 *   Our KMD submits jobs synchronously (MALI_KMD_JOB_SUBMIT with TimeoutMs
 *   polls until completion). Therefore an out_sync is already satisfied by the
 *   time the submit ioctl returns, and waiting on an in_sync reduces to a
 *   user-mode flag check. No kernel fence object is needed.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Do not let the UCRT declare the POSIX spellings (open/close/read/...) as
 * __declspec(dllimport): we define those here, and defining a dllimport
 * function is an error. The underscored _open/_close stay declared.
 */
#define _CRT_DECLARE_NONSTDC_NAMES 0
#define _CRT_SECURE_NO_WARNINGS 1

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>

#include "mali_umd.h"

/* <windows.h> pulls in winsock.h, which defines its own _IO/_IOR/_IOW. Our
 * cross/compat/sys/ioccom.h (included by drm.h) owns those names for the DRM
 * request encoding; clear the Winsock ones first so the definitions do not
 * conflict. */
#ifdef _IO
#  undef _IO
#endif
#ifdef _IOR
#  undef _IOR
#endif
#ifdef _IOW
#  undef _IOW
#endif
#ifdef _IOWR
#  undef _IOWR
#endif

#include "xf86drm.h"
#include "drm-uapi/drm.h"
#include "drm-uapi/panfrost_drm.h"

#ifndef ENODEV
#  define ENODEV 19
#endif
#ifndef ENOTTY
#  define ENOTTY 25
#endif
#ifndef ETIME
#  define ETIME 132
#endif
#ifndef EBADF
#  define EBADF 9
#endif

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

#define MALI_MAX_FDS       64
#define MALI_MAX_SYNCOBJS  4096
#define MALI_FD_BASE       0x40000000   /* never collides with CRT fds   */
#define MALI_MMAP_BASE     0x10000000u  /* fake DRM mmap offsets         */

/* GPU register offsets (see the Linux panfrost_regs.h). We read these through
 * MALI_REG_BLOCK_GPU to answer DRM_PANFROST_PARAM_*, exactly like the kernel's
 * panfrost_gpu_init_features() does. */
#define GPU_REG_ID                  0x000
#define GPU_REG_L2_FEATURES         0x004
#define GPU_REG_CORE_FEATURES       0x008
#define GPU_REG_TILER_FEATURES      0x00C
#define GPU_REG_MEM_FEATURES        0x010
#define GPU_REG_MMU_FEATURES        0x014
#define GPU_REG_AS_PRESENT          0x018
#define GPU_REG_JS_PRESENT          0x01C
#define GPU_REG_AFBC_FEATURES       0x04C
#define GPU_REG_TIMESTAMP_LO        0x098
#define GPU_REG_TIMESTAMP_HI        0x09C
#define GPU_REG_MAX_THREADS         0x0A0
#define GPU_REG_MAX_WORKGROUP_SIZE  0x0A4
#define GPU_REG_MAX_BARRIER_SIZE    0x0A8
#define GPU_REG_THREAD_FEATURES     0x0AC
#define GPU_REG_TEXTURE_FEATURES(n) (0x0B0 + (n) * 4)
#define GPU_REG_JS_FEATURES(n)      (0x0C0 + (n) * 4)
#define GPU_REG_SHADER_PRESENT_LO   0x100
#define GPU_REG_SHADER_PRESENT_HI   0x104
#define GPU_REG_TILER_PRESENT_LO    0x110
#define GPU_REG_TILER_PRESENT_HI    0x114
#define GPU_REG_L2_PRESENT_LO       0x120
#define GPU_REG_L2_PRESENT_HI       0x124
#define GPU_REG_COHERENCY_FEATURES  0x300
#define GPU_REG_THREAD_TLS_ALLOC    0x310
#define GPU_REG_STACK_PRESENT_LO    0xE00
#define GPU_REG_STACK_PRESENT_HI    0xE04

/* ------------------------------------------------------------------ */
/* Small logging helper (debugger only, no CRT stdio dependency)       */
/* ------------------------------------------------------------------ */
static void
mali_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    OutputDebugStringA("[mali-drm] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

/* ------------------------------------------------------------------ */
/* Objects                                                             */
/* ------------------------------------------------------------------ */

struct MaliBo {
    struct MaliBo *next;
    uint32_t handle;
    uint32_t mmap_off;
    uint64_t size;
    uint64_t gpu_va;
    void    *cpu_va;
};

/* A reference-counted synchronisation state. Both syncobj handles and exported
 * sync-file descriptors point at one of these; a syncobj destroy only drops a
 * reference. */
struct MaliSync {
    LONG     refs;
    bool     timeline;
    bool     signaled;
    uint64_t point;
};

struct MaliDevice {
    PMALI_UMD_DEVICE umd;
    struct MaliBo   *bos;
    uint32_t         next_mmap_off;
    struct MaliSync *syncs[MALI_MAX_SYNCOBJS];
};

enum MaliFdKind {
    MALI_FD_NONE = 0,
    MALI_FD_DEVICE,
    MALI_FD_SYNCFILE,
};

struct MaliFd {
    enum MaliFdKind kind;
    struct MaliDevice *dev;      /* MALI_FD_DEVICE  */
    struct MaliSync   *sync;     /* MALI_FD_SYNCFILE */
};

static SRWLOCK         g_lock = SRWLOCK_INIT;
static struct MaliFd   g_fds[MALI_MAX_FDS];

/* ------------------------------------------------------------------ */
/* fd table                                                            */
/* ------------------------------------------------------------------ */

static void
fd_lock(void)   { AcquireSRWLockExclusive(&g_lock); }
static void
fd_unlock(void) { ReleaseSRWLockExclusive(&g_lock); }

/* Caller must NOT hold the lock. */
static struct MaliFd *
fd_lookup(int fd)
{
    struct MaliFd *out = NULL;
    int idx;

    if (fd < MALI_FD_BASE)
        return NULL;
    idx = fd - MALI_FD_BASE;
    if (idx < 0 || idx >= MALI_MAX_FDS)
        return NULL;

    fd_lock();
    if (g_fds[idx].kind != MALI_FD_NONE)
        out = &g_fds[idx];
    fd_unlock();
    return out;
}

/* Returns a synthetic fd, or -1. Caller must not hold the lock. */
static int
fd_alloc(enum MaliFdKind kind)
{
    int i, fd = -1;

    fd_lock();
    for (i = 0; i < MALI_MAX_FDS; i++) {
        if (g_fds[i].kind == MALI_FD_NONE) {
            g_fds[i].kind = kind;
            g_fds[i].dev  = NULL;
            g_fds[i].sync = NULL;
            fd = MALI_FD_BASE + i;
            break;
        }
    }
    fd_unlock();
    return fd;
}

/* Drops whatever the slot holds and frees it. Caller must not hold the lock. */
static void
fd_free(int fd)
{
    struct MaliFd entry;
    int idx = fd - MALI_FD_BASE;
    struct MaliBo *bo, *next;

    if (idx < 0 || idx >= MALI_MAX_FDS)
        return;

    fd_lock();
    entry = g_fds[idx];
    g_fds[idx].kind = MALI_FD_NONE;
    g_fds[idx].dev  = NULL;
    g_fds[idx].sync = NULL;
    fd_unlock();

    if (entry.kind == MALI_FD_SYNCFILE && entry.sync) {
        if (InterlockedDecrement(&entry.sync->refs) == 0)
            free(entry.sync);
    } else if (entry.kind == MALI_FD_DEVICE && entry.dev) {
        struct MaliDevice *dev = entry.dev;

        for (bo = dev->bos; bo; bo = next) {
            next = bo->next;
            MaliUmdBoFree(dev->umd, bo->handle);
            free(bo);
        }
        if (dev->umd)
            MaliUmdClose(dev->umd);
        free(dev);
    }
}

/* ------------------------------------------------------------------ */
/* Sync object helpers                                                 */
/* ------------------------------------------------------------------ */

static struct MaliSync *
sync_new(bool timeline, bool signaled)
{
    struct MaliSync *s = (struct MaliSync *)calloc(1, sizeof(*s));

    if (!s)
        return NULL;
    s->refs = 1;
    s->timeline = timeline;
    s->signaled = signaled;
    s->point = signaled ? 1 : 0;
    return s;
}

static struct MaliSync *
sync_ref(struct MaliSync *s)
{
    if (s)
        InterlockedIncrement(&s->refs);
    return s;
}

static void
sync_unref(struct MaliSync *s)
{
    if (s && InterlockedDecrement(&s->refs) == 0)
        free(s);
}

/* Allocate a syncobj handle (must be nonzero: out_sync==0 means "none"). */
static uint32_t
sync_handle_alloc(struct MaliDevice *dev, struct MaliSync *s)
{
    uint32_t h;

    for (h = 1; h < MALI_MAX_SYNCOBJS; h++) {
        if (!dev->syncs[h]) {
            dev->syncs[h] = s;
            return h;
        }
    }
    return 0;
}

static struct MaliSync *
sync_handle_get(struct MaliDevice *dev, uint32_t handle)
{
    if (handle == 0 || handle >= MALI_MAX_SYNCOBJS)
        return NULL;
    return dev->syncs[handle];
}

static bool
sync_is_signaled(struct MaliSync *s, uint64_t point)
{
    if (!s)
        return false;
    if (s->timeline)
        return s->point >= point;
    return s->signaled;
}

static void
sync_signal(struct MaliSync *s, uint64_t point)
{
    if (!s)
        return;
    if (s->timeline) {
        if (point > s->point)
            s->point = point;
    } else {
        s->signaled = true;
        s->point = 1;
    }
}

static uint64_t
now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    if (!freq.QuadPart)
        return 0;
    return (uint64_t)((c.QuadPart / freq.QuadPart) * 1000000000ull) +
           (uint64_t)(((c.QuadPart % freq.QuadPart) * 1000000000ull) / freq.QuadPart);
}

/* Wait until one or all of the given syncobjs satisfy their point.
 * timeout_ns <= 0 means "poll once". Returns 0 on success, -1/ETIME on
 * timeout. */
static int
sync_wait_many(struct MaliDevice *dev, const uint32_t *handles,
               const uint64_t *points, unsigned count, int64_t timeout_ns,
               unsigned flags, uint32_t *first_signaled)
{
    uint64_t deadline = 0;
    bool infinite;

    if (timeout_ns > 0)
        deadline = now_ns() + (uint64_t)timeout_ns;
    infinite = (timeout_ns < 0) || (timeout_ns == INT64_MAX);

    for (;;) {
        unsigned i;
        unsigned ok_count = 0;
        bool all_ok = true;

        for (i = 0; i < count; i++) {
            struct MaliSync *s = sync_handle_get(dev, handles[i]);
            uint64_t pt = points ? points[i] : 0;
            bool ok = s ? sync_is_signaled(s, pt) : false;

            if (ok) {
                ok_count++;
                if (first_signaled && *first_signaled == UINT32_MAX)
                    *first_signaled = i;
            } else {
                all_ok = false;
                if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)
                    break;
            }
        }

        if (count == 0)
            return 0;
        if (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) {
            if (all_ok)
                return 0;
        } else if (ok_count > 0) {
            return 0;
        }

        if (!infinite) {
            if (timeout_ns <= 0 || now_ns() >= deadline) {
                errno = ETIME;
                return -1;
            }
        }
        Sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* Buffer object helpers                                               */
/* ------------------------------------------------------------------ */

static struct MaliBo *
bo_find(struct MaliDevice *dev, uint32_t handle)
{
    struct MaliBo *bo;

    for (bo = dev->bos; bo; bo = bo->next) {
        if (bo->handle == handle)
            return bo;
    }
    return NULL;
}

static struct MaliBo *
bo_find_mmap(struct MaliDevice *dev, uint32_t off, uint32_t *delta)
{
    struct MaliBo *bo;

    for (bo = dev->bos; bo; bo = bo->next) {
        if (off >= bo->mmap_off && (uint64_t)(off - bo->mmap_off) < bo->size) {
            *delta = off - bo->mmap_off;
            return bo;
        }
    }
    return NULL;
}

/* Allocate a fake DRM mmap offset range for a BO. Offsets stay below 2^31
 * because Mesa stores them in a 32-bit off_t. */
static uint32_t
bo_alloc_mmap_off(struct MaliDevice *dev, uint64_t size)
{
    uint32_t span = (uint32_t)((size + 0xFFFFu) & ~0xFFFFull);
    uint32_t off;

    if (span < 0x10000u)
        span = 0x10000u;

    for (;;) {
        off = dev->next_mmap_off;
        dev->next_mmap_off += span;
        if (off + span < 0x80000000u)
            return off;
        dev->next_mmap_off = MALI_MMAP_BASE;
    }
}

/* ------------------------------------------------------------------ */
/* Register / parameter helpers                                        */
/* ------------------------------------------------------------------ */

static uint32_t
gpu_reg(struct MaliDevice *dev, uint32_t offset)
{
    UINT32 v = 0;

    if (!dev || !dev->umd)
        return 0;
    if (!MaliUmdRegRead(dev->umd, MALI_REG_BLOCK_GPU, offset, &v))
        return 0;
    return v;
}

/* ------------------------------------------------------------------ */
/* ioctl translation                                                   */
/* ------------------------------------------------------------------ */

static int
mali_get_param(struct MaliDevice *dev, struct drm_panfrost_get_param *p)
{
    MALI_KMD_CAPS caps;
    uint64_t value = 0;
    uint32_t lo, hi;

    ZeroMemory(&caps, sizeof(caps));
    MaliUmdGetCaps(dev->umd, &caps);

    switch (p->param) {
    case DRM_PANFROST_PARAM_GPU_PROD_ID:
        value = (uint64_t)(caps.GpuIdRegister >> 16);
        break;
    case DRM_PANFROST_PARAM_GPU_REVISION:
        value = (uint64_t)(caps.GpuIdRegister & 0xFFFFu);
        break;
    case DRM_PANFROST_PARAM_CORE_FEATURES:
        value = caps.CoreFeatures;
        break;
    case DRM_PANFROST_PARAM_MMU_FEATURES:
        value = caps.MmuFeatures;
        break;

    case DRM_PANFROST_PARAM_SHADER_PRESENT:
        lo = gpu_reg(dev, GPU_REG_SHADER_PRESENT_LO);
        hi = gpu_reg(dev, GPU_REG_SHADER_PRESENT_HI);
        value = ((uint64_t)hi << 32) | lo;
        break;
    case DRM_PANFROST_PARAM_TILER_PRESENT:
        lo = gpu_reg(dev, GPU_REG_TILER_PRESENT_LO);
        hi = gpu_reg(dev, GPU_REG_TILER_PRESENT_HI);
        value = ((uint64_t)hi << 32) | lo;
        break;
    case DRM_PANFROST_PARAM_L2_PRESENT:
        lo = gpu_reg(dev, GPU_REG_L2_PRESENT_LO);
        hi = gpu_reg(dev, GPU_REG_L2_PRESENT_HI);
        value = ((uint64_t)hi << 32) | lo;
        break;
    case DRM_PANFROST_PARAM_STACK_PRESENT:
        lo = gpu_reg(dev, GPU_REG_STACK_PRESENT_LO);
        hi = gpu_reg(dev, GPU_REG_STACK_PRESENT_HI);
        value = ((uint64_t)hi << 32) | lo;
        break;

    case DRM_PANFROST_PARAM_AS_PRESENT:
        value = gpu_reg(dev, GPU_REG_AS_PRESENT);
        break;
    case DRM_PANFROST_PARAM_JS_PRESENT:
        value = gpu_reg(dev, GPU_REG_JS_PRESENT);
        break;
    case DRM_PANFROST_PARAM_L2_FEATURES:
        value = gpu_reg(dev, GPU_REG_L2_FEATURES);
        break;
    case DRM_PANFROST_PARAM_TILER_FEATURES:
        value = gpu_reg(dev, GPU_REG_TILER_FEATURES);
        break;
    case DRM_PANFROST_PARAM_MEM_FEATURES:
        value = gpu_reg(dev, GPU_REG_MEM_FEATURES);
        break;
    case DRM_PANFROST_PARAM_THREAD_FEATURES:
        value = gpu_reg(dev, GPU_REG_THREAD_FEATURES);
        break;
    case DRM_PANFROST_PARAM_MAX_THREADS:
        value = gpu_reg(dev, GPU_REG_MAX_THREADS);
        break;
    case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ:
        value = gpu_reg(dev, GPU_REG_MAX_WORKGROUP_SIZE);
        break;
    case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ:
        value = gpu_reg(dev, GPU_REG_MAX_BARRIER_SIZE);
        break;
    case DRM_PANFROST_PARAM_COHERENCY_FEATURES:
        value = gpu_reg(dev, GPU_REG_COHERENCY_FEATURES);
        if (!value)
            value = 1u << 0; /* COHERENCY_ACE_LITE */
        break;
    case DRM_PANFROST_PARAM_TEXTURE_FEATURES0:
    case DRM_PANFROST_PARAM_TEXTURE_FEATURES1:
    case DRM_PANFROST_PARAM_TEXTURE_FEATURES2:
    case DRM_PANFROST_PARAM_TEXTURE_FEATURES3:
        value = gpu_reg(dev, GPU_REG_TEXTURE_FEATURES(
            p->param - DRM_PANFROST_PARAM_TEXTURE_FEATURES0));
        break;
    case DRM_PANFROST_PARAM_JS_FEATURES0:
    case DRM_PANFROST_PARAM_JS_FEATURES1:
    case DRM_PANFROST_PARAM_JS_FEATURES2:
    case DRM_PANFROST_PARAM_JS_FEATURES3:
    case DRM_PANFROST_PARAM_JS_FEATURES4:
    case DRM_PANFROST_PARAM_JS_FEATURES5:
    case DRM_PANFROST_PARAM_JS_FEATURES6:
    case DRM_PANFROST_PARAM_JS_FEATURES7:
    case DRM_PANFROST_PARAM_JS_FEATURES8:
    case DRM_PANFROST_PARAM_JS_FEATURES9:
    case DRM_PANFROST_PARAM_JS_FEATURES10:
    case DRM_PANFROST_PARAM_JS_FEATURES11:
    case DRM_PANFROST_PARAM_JS_FEATURES12:
    case DRM_PANFROST_PARAM_JS_FEATURES13:
    case DRM_PANFROST_PARAM_JS_FEATURES14:
    case DRM_PANFROST_PARAM_JS_FEATURES15:
        value = gpu_reg(dev, GPU_REG_JS_FEATURES(
            p->param - DRM_PANFROST_PARAM_JS_FEATURES0));
        break;
    case DRM_PANFROST_PARAM_NR_CORE_GROUPS: {
        uint64_t l2 = ((uint64_t)gpu_reg(dev, GPU_REG_L2_PRESENT_HI) << 32) |
                      gpu_reg(dev, GPU_REG_L2_PRESENT_LO);
        unsigned n = 0;
        while (l2) { n += (unsigned)(l2 & 1); l2 >>= 1; }
        value = n ? n : 1;
        break;
    }
    case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC:
        value = gpu_reg(dev, GPU_REG_THREAD_TLS_ALLOC);
        break;
    case DRM_PANFROST_PARAM_AFBC_FEATURES:
        value = gpu_reg(dev, GPU_REG_AFBC_FEATURES);
        break;
    case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP:
        value = ((uint64_t)gpu_reg(dev, GPU_REG_TIMESTAMP_HI) << 32) |
                gpu_reg(dev, GPU_REG_TIMESTAMP_LO);
        break;
    case DRM_PANFROST_PARAM_SYSTEM_TIMESTAMP_FREQUENCY:
        value = (uint64_t)caps.MaxFreqKhz * 1000ull;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    p->value = value;
    return 0;
}

static int
mali_submit(struct MaliDevice *dev, struct drm_panfrost_submit *s)
{
    MALI_UMD_SUBMIT sub;
    UINT32 js_status = 0, irq_stat = 0;

    /* Honour in-syncs. Our submits are synchronous, so these are normally
     * already signalled; this also covers a CPU fence the app waited on. */
    if (s->in_sync_count && s->in_syncs) {
        const uint32_t *handles = (const uint32_t *)(uintptr_t)s->in_syncs;
        unsigned i;

        for (i = 0; i < s->in_sync_count; i++) {
            if (!sync_is_signaled(sync_handle_get(dev, handles[i]), 0)) {
                uint32_t first = UINT32_MAX;
                if (sync_wait_many(dev, &handles[i], NULL, 1, -1,
                                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
                                   &first))
                    return -1;
            }
        }
    }

    ZeroMemory(&sub, sizeof(sub));
    sub.Slot = (s->requirements & PANFROST_JD_REQ_FS) ? 1u : 0u;
    sub.Flags = 0;
    sub.Jc = s->jc;
    sub.Affinity = 0;
    sub.AsNumber = 0;
    sub.TimeoutMs = 5000;

    if (!MaliUmdJobSubmit(dev->umd, &sub, &js_status, &irq_stat)) {
        mali_log("submit failed (jc=0x%llx slot=%u)", (unsigned long long)s->jc,
                 sub.Slot);
        errno = EIO;
        return -1;
    }
    if (js_status != 0) {
        mali_log("job rejected (jc=0x%llx slot=%u js=0x%x irq=0x%x)",
                 (unsigned long long)s->jc, sub.Slot, js_status, irq_stat);
        errno = EIO;
        return -1;
    }

    if (s->out_sync)
        sync_signal(sync_handle_get(dev, s->out_sync), 1);

    return 0;
}

static int
drm_dispatch(struct MaliDevice *dev, unsigned long request, void *arg)
{
    switch (request) {
    case DRM_IOCTL_PANFROST_CREATE_BO: {
        struct drm_panfrost_create_bo *p =
            (struct drm_panfrost_create_bo *)arg;
        MALI_UMD_BO bo;
        struct MaliBo *nb;

        ZeroMemory(&bo, sizeof(bo));
        if (!MaliUmdBoAlloc(dev->umd, p->size,
                            MALI_BO_CPU_RD | MALI_BO_CPU_WR |
                            MALI_BO_GPU_RD | MALI_BO_GPU_WR, &bo)) {
            errno = ENOMEM;
            return -1;
        }

        nb = (struct MaliBo *)calloc(1, sizeof(*nb));
        if (!nb) {
            MaliUmdBoFree(dev->umd, bo.Handle);
            errno = ENOMEM;
            return -1;
        }
        nb->handle   = bo.Handle;
        nb->size     = bo.Size;
        nb->gpu_va   = bo.GpuVa;
        nb->cpu_va   = bo.CpuVa;
        nb->mmap_off = bo_alloc_mmap_off(dev, bo.Size);
        nb->next     = dev->bos;
        dev->bos     = nb;

        p->handle = bo.Handle;
        p->offset = bo.GpuVa;
        return 0;
    }

    case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
        struct drm_panfrost_get_bo_offset *p =
            (struct drm_panfrost_get_bo_offset *)arg;
        struct MaliBo *bo = bo_find(dev, p->handle);
        if (!bo) { errno = EINVAL; return -1; }
        p->offset = bo->gpu_va;
        return 0;
    }

    case DRM_IOCTL_PANFROST_MMAP_BO: {
        struct drm_panfrost_mmap_bo *p = (struct drm_panfrost_mmap_bo *)arg;
        struct MaliBo *bo = bo_find(dev, p->handle);
        if (!bo) { errno = EINVAL; return -1; }
        p->offset = bo->mmap_off;
        return 0;
    }

    case DRM_IOCTL_PANFROST_GET_PARAM:
        return mali_get_param(dev, (struct drm_panfrost_get_param *)arg);

    case DRM_IOCTL_PANFROST_WAIT_BO:
        /* Our submissions are synchronous, so every BO is idle. */
        return 0;

    case DRM_IOCTL_PANFROST_MADVISE: {
        struct drm_panfrost_madvise *p = (struct drm_panfrost_madvise *)arg;
        p->retained = 1; /* never purge: our BOs are pinned in the KMD */
        return 0;
    }

    case DRM_IOCTL_PANFROST_SUBMIT:
        return mali_submit(dev, (struct drm_panfrost_submit *)arg);

    case DRM_IOCTL_PANFROST_PERFCNT_ENABLE:
    case DRM_IOCTL_PANFROST_PERFCNT_DUMP:
        errno = ENOTTY; /* not needed by PanVK's normal path */
        return -1;

    default:
        mali_log("unsupported DRM request 0x%lx", request);
        errno = ENOTTY;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Sync object ioctls (used directly by panvk_vX_event.c/queue.c)      */
/* ------------------------------------------------------------------ */

static int
sync_ioctl(struct MaliDevice *dev, unsigned long request, void *arg)
{
    switch (request) {
    case DRM_IOCTL_SYNCOBJ_CREATE: {
        struct drm_syncobj_create *p = (struct drm_syncobj_create *)arg;
        bool signaled = (p->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
        struct MaliSync *s = sync_new(false, signaled);
        uint32_t h;

        if (!s) { errno = ENOMEM; return -1; }
        h = sync_handle_alloc(dev, s);
        if (!h) { sync_unref(s); errno = ENOMEM; return -1; }
        p->handle = h;
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_DESTROY: {
        struct drm_syncobj_destroy *p = (struct drm_syncobj_destroy *)arg;
        struct MaliSync *s = sync_handle_get(dev, p->handle);
        if (!s) { errno = EINVAL; return -1; }
        dev->syncs[p->handle] = NULL;
        sync_unref(s);
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: {
        struct drm_syncobj_handle *p = (struct drm_syncobj_handle *)arg;
        struct MaliSync *s = sync_handle_get(dev, p->handle);
        int fd;

        if (!s) { errno = EINVAL; return -1; }
        fd = fd_alloc(MALI_FD_SYNCFILE);
        if (fd < 0) { errno = EMFILE; return -1; }
        fd_lock();
        g_fds[fd - MALI_FD_BASE].sync = sync_ref(s);
        fd_unlock();
        p->fd = fd;
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: {
        struct drm_syncobj_handle *p = (struct drm_syncobj_handle *)arg;
        struct MaliFd *fe = fd_lookup(p->fd);
        struct MaliSync *s, *ns;
        uint32_t h;

        if (!fe || fe->kind != MALI_FD_SYNCFILE || !fe->sync) {
            errno = EBADF;
            return -1;
        }
        fd_lock();
        s = fe->sync;
        ns = sync_new(s->timeline, s->signaled);
        if (ns) {
            ns->point = s->point;
        }
        fd_unlock();
        if (!ns) { errno = ENOMEM; return -1; }

        h = sync_handle_alloc(dev, ns);
        if (!h) { sync_unref(ns); errno = ENOMEM; return -1; }
        p->handle = h;
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_WAIT: {
        struct drm_syncobj_wait *p = (struct drm_syncobj_wait *)arg;
        uint32_t first = UINT32_MAX;
        const uint32_t *handles = (const uint32_t *)(uintptr_t)p->handles;
        int ret;

        ret = sync_wait_many(dev, handles, NULL, p->count_handles,
                             p->timeout_nsec, p->flags, &first);
        if (ret == 0 && p->first_signaled)
            p->first_signaled = first;
        return ret;
    }

    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
        struct drm_syncobj_timeline_wait *p =
            (struct drm_syncobj_timeline_wait *)arg;
        uint32_t first = UINT32_MAX;
        const uint32_t *handles = (const uint32_t *)(uintptr_t)p->handles;
        const uint64_t *points  = (const uint64_t *)(uintptr_t)p->points;
        int ret;

        ret = sync_wait_many(dev, handles, points, p->count_handles,
                             p->timeout_nsec, p->flags, &first);
        if (ret == 0 && p->first_signaled)
            p->first_signaled = first;
        return ret;
    }

    case DRM_IOCTL_SYNCOBJ_RESET: {
        struct drm_syncobj_array *p = (struct drm_syncobj_array *)arg;
        const uint32_t *handles = (const uint32_t *)(uintptr_t)p->handles;
        unsigned i;

        for (i = 0; i < p->count_handles; i++) {
            struct MaliSync *s = sync_handle_get(dev, handles[i]);
            if (!s) { errno = EINVAL; return -1; }
            s->signaled = false;
            s->point = 0;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_SIGNAL: {
        struct drm_syncobj_array *p = (struct drm_syncobj_array *)arg;
        const uint32_t *handles = (const uint32_t *)(uintptr_t)p->handles;
        unsigned i;

        for (i = 0; i < p->count_handles; i++) {
            struct MaliSync *s = sync_handle_get(dev, handles[i]);
            if (!s) { errno = EINVAL; return -1; }
            sync_signal(s, 1);
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: {
        struct drm_syncobj_timeline_array *p =
            (struct drm_syncobj_timeline_array *)arg;
        const uint32_t *handles = (const uint32_t *)(uintptr_t)p->handles;
        const uint64_t *points  = (const uint64_t *)(uintptr_t)p->points;
        unsigned i;

        for (i = 0; i < p->count_handles; i++) {
            struct MaliSync *s = sync_handle_get(dev, handles[i]);
            if (!s) { errno = EINVAL; return -1; }
            sync_signal(s, points[i]);
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_QUERY: {
        struct drm_syncobj_timeline_array *p =
            (struct drm_syncobj_timeline_array *)arg;
        const uint32_t *handles = (const uint32_t *)(uintptr_t)p->handles;
        uint64_t *points  = (uint64_t *)(uintptr_t)p->points;
        unsigned i;

        for (i = 0; i < p->count_handles; i++) {
            struct MaliSync *s = sync_handle_get(dev, handles[i]);
            if (!s) { errno = EINVAL; return -1; }
            points[i] = s->timeline ? s->point : (s->signaled ? 1 : 0);
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_TRANSFER: {
        struct drm_syncobj_transfer *p = (struct drm_syncobj_transfer *)arg;
        struct MaliSync *src = sync_handle_get(dev, p->src_handle);
        struct MaliSync *dst = sync_handle_get(dev, p->dst_handle);

        if (!src || !dst) { errno = EINVAL; return -1; }
        if (sync_is_signaled(src, p->src_point))
            sync_signal(dst, p->dst_point);
        return 0;
    }

    default:
        errno = ENOTTY;
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* libdrm entry points                                                 */
/* ------------------------------------------------------------------ */

extern "C" int __cdecl
drmIoctl(int fd, unsigned long request, void *arg)
{
    struct MaliFd *fe = fd_lookup(fd);

    if (!fe || fe->kind != MALI_FD_DEVICE || !fe->dev) {
        errno = ENODEV;
        return -1;
    }
    return drm_dispatch(fe->dev, request, arg);
}

/* The syncobj wrappers below are what libdrm normally implements. Mesa's
 * vk_drm_syncobj.c calls them; panvk_vX_event.c calls drmIoctl directly. */

extern "C" int __cdecl
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    struct drm_syncobj_create args;
    ZeroMemory(&args, sizeof(args));
    args.flags = flags;
    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args))
        return -1;
    *handle = args.handle;
    return 0;
}

extern "C" int __cdecl
drmSyncobjDestroy(int fd, uint32_t handle)
{
    struct drm_syncobj_destroy args;
    ZeroMemory(&args, sizeof(args));
    args.handle = handle;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

extern "C" int __cdecl
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
    struct drm_syncobj_handle args;
    ZeroMemory(&args, sizeof(args));
    args.handle = handle;
    args.fd = -1;
    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args))
        return -1;
    *obj_fd = args.fd;
    return 0;
}

extern "C" int __cdecl
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
    struct drm_syncobj_handle args;
    ZeroMemory(&args, sizeof(args));
    args.fd = obj_fd;
    if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args))
        return -1;
    *handle = args.handle;
    return 0;
}

extern "C" int __cdecl
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    return drmSyncobjHandleToFD(fd, handle, sync_file_fd);
}

extern "C" int __cdecl
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    (void)fd; (void)handle; (void)sync_file_fd;
    errno = ENOTTY;
    return -1;
}

extern "C" int __cdecl
drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
    struct drm_syncobj_wait args;
    ZeroMemory(&args, sizeof(args));
    args.handles = (uint64_t)(uintptr_t)handles;
    args.count_handles = num_handles;
    args.timeout_nsec = timeout_nsec;
    args.flags = flags;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args);
}

extern "C" int __cdecl
drmSyncobjTimelineWait(int fd, const uint32_t *handles, const uint64_t *points,
                       unsigned num_handles, int64_t timeout_nsec,
                       unsigned flags, uint32_t *first_signaled)
{
    struct drm_syncobj_timeline_wait args;
    ZeroMemory(&args, sizeof(args));
    args.handles = (uint64_t)(uintptr_t)handles;
    args.points = (uint64_t)(uintptr_t)points;
    args.count_handles = num_handles;
    args.timeout_nsec = timeout_nsec;
    args.flags = flags;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
}

extern "C" int __cdecl
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
    struct drm_syncobj_array args;
    ZeroMemory(&args, sizeof(args));
    args.handles = (uint64_t)(uintptr_t)handles;
    args.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

extern "C" int __cdecl
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
    struct drm_syncobj_array args;
    ZeroMemory(&args, sizeof(args));
    args.handles = (uint64_t)(uintptr_t)handles;
    args.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

extern "C" int __cdecl
drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                         const uint64_t *points, uint32_t handle_count)
{
    struct drm_syncobj_timeline_array args;
    ZeroMemory(&args, sizeof(args));
    args.handles = (uint64_t)(uintptr_t)handles;
    args.points = (uint64_t)(uintptr_t)points;
    args.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args);
}

extern "C" int __cdecl
drmSyncobjQuery(int fd, const uint32_t *handles, uint64_t *points,
                uint32_t handle_count)
{
    struct drm_syncobj_timeline_array args;
    ZeroMemory(&args, sizeof(args));
    args.handles = (uint64_t)(uintptr_t)handles;
    args.points = (uint64_t)(uintptr_t)points;
    args.count_handles = handle_count;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
}

extern "C" int __cdecl
drmSyncobjTimelineQuery(int fd, const uint32_t *handles, uint64_t *points,
                        uint32_t handle_count)
{
    return drmSyncobjQuery(fd, handles, points, handle_count);
}

extern "C" int __cdecl
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                   uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
    struct drm_syncobj_transfer args;
    ZeroMemory(&args, sizeof(args));
    args.dst_handle = dst_handle;
    args.dst_point = dst_point;
    args.src_handle = src_handle;
    args.src_point = src_point;
    args.flags = flags;
    return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
}

/* ---- version ----------------------------------------------------- */

extern "C" drmVersionPtr __cdecl
drmGetVersion(int fd)
{
    drmVersionPtr v;

    if (!fd_lookup(fd)) {
        errno = ENODEV;
        return NULL;
    }

    v = (drmVersionPtr)calloc(1, sizeof(*v));
    if (!v) {
        errno = ENOMEM;
        return NULL;
    }

    /* "panfrost" makes pan_kmod_dev_create() pick panfrost_kmod_ops, i.e. the
     * Job Manager (JM) backend this GPU needs (Valhall v9 is not CSF). */
    v->name = _strdup("panfrost");
    v->date = _strdup("20260101");
    v->desc = _strdup("Mali KMD libdrm shim");
    if (!v->name || !v->date || !v->desc) {
        free(v->name); free(v->date); free(v->desc); free(v);
        errno = ENOMEM;
        return NULL;
    }
    v->name_len = (int)strlen(v->name);
    v->date_len = (int)strlen(v->date);
    v->desc_len = (int)strlen(v->desc);
    v->version_major = 1;
    v->version_minor = 3;   /* enables BO_NOEXEC + timestamp queries */
    v->version_patchlevel = 0;
    return v;
}

extern "C" void __cdecl
drmFreeVersion(drmVersionPtr v)
{
    if (!v)
        return;
    free(v->name);
    free(v->date);
    free(v->desc);
    free(v);
}

/* ---- device enumeration ------------------------------------------ */

static drmDevicePtr
drm_device_new(void)
{
    drmDevicePtr d = (drmDevicePtr)calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->nodes = (char **)calloc(DRM_NODE_MAX, sizeof(char *));
    if (!d->nodes) {
        free(d);
        return NULL;
    }
    d->nodes[DRM_NODE_RENDER] = _strdup("/dev/dri/renderD128");
    d->available_nodes = 1 << DRM_NODE_RENDER;
    d->bustype = DRM_BUS_PLATFORM;
    strcpy(d->businfo.platform.fullname, "mali");
    return d;
}

extern "C" int __cdecl
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
    PMALI_UMD_DEVICE probe;
    drmDevicePtr d;

    (void)flags;
    if (max_devices < 1 || !devices)
        return 0;

    /* Only advertise a node when the KMD is actually present, so Vulkan
     * enumerates zero physical devices instead of a dead one. */
    probe = MaliUmdOpen();
    if (!probe)
        return 0;
    MaliUmdClose(probe);

    d = drm_device_new();
    if (!d)
        return -ENOMEM;

    devices[0] = d;
    return 1;
}

extern "C" void __cdecl
drmFreeDevices(drmDevicePtr devices[], int count)
{
    int i;

    if (!devices)
        return;
    for (i = 0; i < count; i++) {
        drmDevicePtr d = devices[i];
        if (!d)
            continue;
        if (d->nodes) {
            int n;
            for (n = 0; n < DRM_NODE_MAX; n++)
                free(d->nodes[n]);
            free(d->nodes);
        }
        free(d);
        devices[i] = NULL;
    }
}

extern "C" int __cdecl
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
    (void)flags;
    if (!fd_lookup(fd) || !device) {
        errno = ENODEV;
        return -1;
    }
    *device = drm_device_new();
    if (!*device) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

extern "C" void __cdecl
drmFreeDevice(drmDevicePtr *device)
{
    if (!device || !*device)
        return;
    drmFreeDevices(device, 1);
    *device = NULL;
}

extern "C" int __cdecl
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
    if (!a || !b)
        return 0;
    if (a == b)
        return 1;
    if (a->bustype != b->bustype)
        return 0;
    if (a->available_nodes != b->available_nodes)
        return 0;
    return 1;
}

extern "C" int __cdecl
drmGetNodeTypeFromFd(int fd)
{
    return fd_lookup(fd) ? DRM_NODE_RENDER : -1;
}

extern "C" int __cdecl
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    (void)fd;
    if (!value) {
        errno = EINVAL;
        return -1;
    }
    switch (capability) {
    case DRM_CAP_PRIME:
        *value = 0;             /* no dma-buf import/export */
        return 0;
    case DRM_CAP_TIMESTAMP_MONOTONIC:
        *value = 1;
        return 0;
    case DRM_CAP_SYNCOBJ:
        *value = 1;
        return 0;
    case DRM_CAP_SYNCOBJ_TIMELINE:
        *value = 1;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

extern "C" int __cdecl
drmGetMagic(int fd, drm_magic_t *magic)
{
    if (!fd_lookup(fd) || !magic) {
        errno = ENODEV;
        return -1;
    }
    *magic = 0;
    return 0;
}

extern "C" int __cdecl
drmAuthMagic(int fd, drm_magic_t magic)
{
    (void)magic;
    return fd_lookup(fd) ? 0 : -1;
}

/* ---- PRIME ------------------------------------------------------- */

extern "C" int __cdecl
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    (void)fd; (void)handle; (void)flags; (void)prime_fd;
    errno = ENOTTY;
    return -1;
}

extern "C" int __cdecl
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
    (void)fd; (void)prime_fd; (void)handle;
    errno = ENOTTY;
    return -1;
}

extern "C" int __cdecl
drmCloseBufferHandle(int fd, uint32_t handle)
{
    struct MaliFd *fe = fd_lookup(fd);
    struct MaliBo **pp;
    struct MaliBo *bo;

    if (!fe || fe->kind != MALI_FD_DEVICE || !fe->dev) {
        errno = ENODEV;
        return -1;
    }

    pp = &fe->dev->bos;
    while (*pp && (*pp)->handle != handle)
        pp = &(*pp)->next;
    bo = *pp;
    if (!bo) {
        errno = EINVAL;
        return -1;
    }
    *pp = bo->next;

    MaliUmdBoFree(fe->dev->umd, handle);
    free(bo);
    return 0;
}

/* ------------------------------------------------------------------ */
/* mmap / munmap                                                       */
/* ------------------------------------------------------------------ */

extern "C" void *__cdecl
mmap(void *addr, size_t length, int prot, int flags, int fd, long offset)
{
    struct MaliFd *fe = fd_lookup(fd);
    struct MaliBo *bo;
    uint32_t delta = 0;

    (void)addr; (void)prot; (void)flags;

    if (fe && fe->kind == MALI_FD_DEVICE && fe->dev) {
        bo = bo_find_mmap(fe->dev, (uint32_t)offset, &delta);
        if (bo) {
            if ((uint64_t)delta + length > bo->size)
                return MAP_FAILED;
            return (void *)((char *)bo->cpu_va + delta);
        }
    }

    errno = ENODEV;
    return MAP_FAILED;
}

extern "C" int __cdecl
munmap(void *addr, size_t length)
{
    /* Buffer objects are owned by the KMD and stay mapped for the life of the
     * process; Mesa only ever unmaps BO ranges through pan_kmod_bo_munmap(). */
    (void)addr; (void)length;
    return 0;
}

extern "C" int __cdecl
mprotect(void *addr, size_t length, int prot)
{
    /* Only the Panfrost debug decoder calls mprotect(), to relax a read-only
     * mapping before patching it (src/panfrost/lib/genxml/decode_common.c).
     * BO mappings already come back read/write from the KMD, so there is
     * nothing to change; report success. */
    (void)addr; (void)length; (void)prot;
    return 0;
}

/* ------------------------------------------------------------------ */
/* open / close interception                                           */
/* ------------------------------------------------------------------ */

static bool
is_drm_node(const char *path)
{
    return path && _strnicmp(path, "/dev/dri/", 9) == 0;
}

extern "C" int __cdecl
open(const char *path, int flags, int mode)
{
    if (!is_drm_node(path))
        return _open(path, flags, mode);

    {
        PMALI_UMD_DEVICE umd = MaliUmdOpen();
        struct MaliDevice *dev;
        int fd;

        if (!umd) {
            errno = ENODEV;
            return -1;
        }

        dev = (struct MaliDevice *)calloc(1, sizeof(*dev));
        if (!dev) {
            MaliUmdClose(umd);
            errno = ENOMEM;
            return -1;
        }
        dev->umd = umd;
        dev->next_mmap_off = MALI_MMAP_BASE;

        /* Ensure the GPU block is readable for DRM_PANFROST_PARAM_*; the
         * KMD keeps it powered, this is harmless if it already is. */
        MaliUmdPowerOn(umd);

        fd = fd_alloc(MALI_FD_DEVICE);
        if (fd < 0) {
            MaliUmdClose(umd);
            free(dev);
            errno = EMFILE;
            return -1;
        }
        fd_lock();
        g_fds[fd - MALI_FD_BASE].dev = dev;
        fd_unlock();
        return fd;
    }
}

extern "C" int (__cdecl *__imp_open)(const char *, int, int) = open;

extern "C" int __cdecl
close(int fd)
{
    if (fd >= MALI_FD_BASE) {
        if (!fd_lookup(fd)) {
            errno = EBADF;
            return -1;
        }
        fd_free(fd);
        return 0;
    }
    return _close(fd);
}

extern "C" int (__cdecl *__imp_close)(int) = close;

/* ------------------------------------------------------------------ */
/* POSIX clock / poll / ioctl used by util/libsync.h                   */
/* ------------------------------------------------------------------ */

extern "C" int __cdecl
clock_gettime(int clk_id, struct timespec *tp)
{
    uint64_t ns;

    (void)clk_id;
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    ns = now_ns();
    tp->tv_sec = (time_t)(ns / 1000000000ull);
    tp->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

extern "C" int __cdecl
poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    unsigned long i;

    /* Our synthetic sync-file descriptors are already satisfied (submits are
     * synchronous), so report them readable. Other descriptors are treated as
     * not ready; libsync's sync_wait() then reports a timeout, which is fine
     * because the panthor/CSF path is never selected on this hardware. */
    for (i = 0; i < nfds; i++) {
        if (fds[i].fd >= MALI_FD_BASE && fd_lookup(fds[i].fd)) {
            fds[i].revents = POLLIN;
            return 1;
        }
    }

    if (timeout > 0)
        Sleep((DWORD)timeout);
    for (i = 0; i < nfds; i++)
        fds[i].revents = 0;
    return 0;
}

extern "C" int __cdecl
ioctl(int fd, unsigned long request, ...)
{
    /* Only the Android/Linux sync-file helpers call this; they are unused on
     * this backend. Keep the symbol so libsync.h links. */
    (void)fd;
    (void)request;
    errno = ENOTTY;
    return -1;
}