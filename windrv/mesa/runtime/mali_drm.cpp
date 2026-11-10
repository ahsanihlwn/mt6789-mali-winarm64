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
#include <unistd.h>
#include <dlfcn.h>

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
#include "drm-uapi/mali_kbase_ioctl.h"

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
/* kbase uAPI constants (see V9-PANVK-PLAN.md 3.5)                     */
/* ------------------------------------------------------------------ */
/* The v9 PanVK stack speaks the Arm kbase ioctl ABI instead of DRM, so the
 * shim implements the JM subset of it on top of our own KMD. */
#define MALI_KBASE_PAGE_SIZE        4096ull

/* kbase maps its tracking page at a fixed "gpu address" of 3 << 12. */
#define MALI_KBASE_TRACKING_HANDLE  (3ull << 12)

/* Completion records: base_jd_event_v2, and the cap the stack passes to read()
 * (PANVK_KBASE_MAX_EVENTS in panvk_vX_gpu_queue.c). */
#define MALI_KBASE_MAX_EVENTS       256

/* base_jd_atom_v2 offsets inside one 64-byte submit stride (the stack's
 * PANVK_KBASE_ATOM_STRIDE).  pre_dep lives at 36..39, atom_number at 40 and
 * core_req at 44; only the latter two matter to the kernel side. */
#define MALI_KBASE_ATOM_JC          0
#define MALI_KBASE_ATOM_CORE_REQ    44
#define MALI_KBASE_ATOM_NUMBER      40
#define MALI_KBASE_ATOM_STRIDE      64

/* core_req bits (mali_base_jm_kernel.h). */
#define MALI_BASE_JD_REQ_FS         (1u << 0)
#define MALI_BASE_JD_REQ_SOFT_JOB   (1u << 26)
#define MALI_BASE_JD_SOFT_FENCE_TRIGGER (MALI_BASE_JD_REQ_SOFT_JOB | 0x2u)

/* base_jd_event_code values we emit. */
#define MALI_BASE_JD_EVENT_DONE     0x01u
#define MALI_BASE_JD_EVENT_FAILED   0x40u

/* What a BASE_JD_REQ_SOFT_FENCE_TRIGGER atom points at: kbase writes the
 * sync-file fd it created into this struct (jm/mali_base_jm_kernel.h). */
struct KbaseBaseFence {
    int32_t fd;
    int32_t stream_fd;
};

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

/* Completion record delivered by read() on a kbase device fd.  This mirrors the
 * Arm uAPI's base_jd_event_v2 (mali_base_jm_kernel.h) and the copy the v9 stack
 * unpacks in panvk_vX_gpu_queue.c (struct panvk_kbase_event_v2):
 *
 *     __u32 event_code;  __u8 atom_number;  __u8 padding[3];
 *     struct base_jd_udata udata;        (a pair of __u64 values)
 *
 * The stack reads at most 32 of these per read() (PANVK_KBASE_MAX_EVENTS). */
struct MaliKbaseEvent {
    uint32_t event_code;
    uint8_t  atom_number;
    uint8_t  pad[3];
    uint64_t udata[2];
};
static_assert(sizeof(struct MaliKbaseEvent) == 24, "kbase event record size");

struct MaliDevice {
    PMALI_UMD_DEVICE umd;
    struct MaliBo   *bos;
    uint32_t         next_mmap_off;
    struct MaliSync *syncs[MALI_MAX_SYNCOBJS];

    /* kbase uAPI emulation (see V9-PANVK-PLAN.md 3.5).  The v9 stack talks the
     * Arm kbase ioctl ABI rather than DRM, and completion is asynchronous: it
     * polls this fd and then read()s base_jd_event_v2 records.  The queue below
     * is that delivery path, so it must exist per device. */
    SRWLOCK  kbase_lock;
    HANDLE   kbase_evt;            /* auto-reset, signalled when events queue */
    bool     kbase_handshake;      /* VERSION_CHECK passed */
    uint32_t kbase_ev_head;
    uint32_t kbase_ev_count;
    struct MaliKbaseEvent kbase_ev[MALI_KBASE_MAX_EVENTS];

    /* Which face of the KMD this fd represents: /dev/dri/renderD128 (DRM,
     * Gallium panfrost) or /dev/mali0 (kbase, PanVK v9).  drmGetVersion()
     * reports it, and PanVK uses that name to decide whether a node is one it
     * can drive. */
    bool     kbase_node;
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
        if (dev->kbase_evt)
            CloseHandle(dev->kbase_evt);
        if (dev->umd)
            MaliUmdClose(dev->umd);
        free(dev);
    }
}

/* Duplicate a synthetic descriptor onto the lowest free slot.  Only sync files
 * can be duplicated: a device fd names one device instance (and its event
 * queue), so a copy would be ambiguous.  Reached only through
 * fcntl(F_DUPFD_CLOEXEC) on the dma-buf export path, which is disabled here. */
static int
fd_dup_mali(int fd)
{
    struct MaliFd *fe = fd_lookup(fd);
    int nfd;

    if (!fe) {
        errno = EBADF;
        return -1;
    }
    if (fe->kind != MALI_FD_SYNCFILE || !fe->sync) {
        errno = EINVAL;
        return -1;
    }

    nfd = fd_alloc(MALI_FD_SYNCFILE);
    if (nfd < 0)
        return -1;

    fd_lock();
    InterlockedIncrement(&fe->sync->refs);
    g_fds[nfd - MALI_FD_BASE].sync = fe->sync;
    fd_unlock();
    return nfd;
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
bo_find_mmap(struct MaliDevice *dev, uint64_t off, uint64_t *delta)
{
    struct MaliBo *bo;

    /* Synthetic DRM offsets from DRM_IOCTL_PANFROST_MMAP_BO (never zero). */
    for (bo = dev->bos; bo; bo = bo->next) {
        if (bo->mmap_off && off >= bo->mmap_off &&
            off - bo->mmap_off < bo->size) {
            *delta = off - bo->mmap_off;
            return bo;
        }
    }

    /* kbase convention: the mmap() offset is the region's GPU VA.  We hand
     * back a real GPU VA instead of a SAME_VA cookie, so the CPU address is
     * unrelated to it and this lookup is the only way back to the mapping. */
    for (bo = dev->bos; bo; bo = bo->next) {
        if (bo->gpu_va && off >= bo->gpu_va && off - bo->gpu_va < bo->size) {
            *delta = off - bo->gpu_va;
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
    struct MaliFd *f = fd_lookup(fd);

    if (!f) {
        errno = ENODEV;
        return NULL;
    }

    v = (drmVersionPtr)calloc(1, sizeof(*v));
    if (!v) {
        errno = ENOMEM;
        return NULL;
    }

    /* The node decides the name.  /dev/dri/renderD128 is the DRM face of the
     * KMD (Gallium panfrost, panfrost_kmod_ops); /dev/mali0 is the kbase face
     * (PanVK v9, kbase_kmod_ops).  PanVK's DRM probe accepts only "panfrost"
     * or "panthor" (panvk_physical_device.c:113), so "kbase" makes it skip
     * that node instead of aborting enumeration - which is what the Vulkan
     * build needs, since -Dpanfrost-kmds=kbase,panthor cannot create a
     * "panfrost" device at all. */
    v->name = _strdup(f->dev && f->dev->kbase_node ? "kbase" : "panfrost");
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
mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
    struct MaliFd *fe = fd_lookup(fd);
    struct MaliBo *bo;
    uint64_t delta = 0;

    (void)addr; (void)prot; (void)flags;

    if (fe && fe->kind == MALI_FD_DEVICE && fe->dev) {
        /* kbase reads/writes a tracking page for outstanding atoms.  It is not
         * backed by anything we must keep in sync (our submits complete
         * synchronously), so a permanently zero page is enough. */
        if ((uint64_t)offset == MALI_KBASE_TRACKING_HANDLE) {
            static uint8_t tracking[MALI_KBASE_PAGE_SIZE];
            if (length > sizeof(tracking))
                return MAP_FAILED;
            return tracking;
        }

        bo = bo_find_mmap(fe->dev, (uint64_t)offset, &delta);
        if (bo) {
            if (delta + length > bo->size)
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

extern "C" int __cdecl
mincore(void *addr, size_t length, unsigned char *vec)
{
    /* <sys/mman.h> declares this for panvk_vX_gpu_queue.c's Linux-only debug
     * heap dump, which uses it as a hint to decide whether the tiler heap was
     * ever written. There is no Windows equivalent worth faking: our BO
     * mappings come from the KMD, and "resident" describes a lazily populated
     * Linux page, not a file/working-set page. The caller treats failure as
     * "residency unknown", so ENOSYS is the honest answer. */
    (void)addr; (void)length; (void)vec;
    errno = ENOSYS;
    return -1;
}

extern "C" int64_t __cdecl
pread(int fd, void *buf, size_t count, int64_t offset)
{
    /* <unistd.h> declares this.  Like read() above, a synthetic descriptor has
     * no file position to preserve, so reject it rather than seek on a slot
     * index the CRT would not understand. */
    __int64 saved;
    int n, err;

    if (fd >= MALI_FD_BASE) {
        errno = ENOSYS;
        return -1;
    }

    if (count > 0x7fffffffu)
        count = 0x7fffffffu;   /* _read() takes an unsigned int */

    saved = _lseeki64(fd, 0, SEEK_CUR);
    if (saved < 0)
        return -1;

    if (_lseeki64(fd, offset, SEEK_SET) < 0)
        return -1;

    n = _read(fd, buf, (unsigned int)count);
    err = errno;

    /* Restore the position even on failure, and report the read's errno, not
     * the seek's. */
    _lseeki64(fd, saved, SEEK_SET);
    errno = err;

    return n;
}

extern "C" int __cdecl
dladdr(const void *addr, Dl_info *info)
{
    /* <dlfcn.h> declares this for panvk_physical_device.c, which asks for the
     * path of the module containing a function so it can look for a "meta.json"
     * next to the DLL (FourFectVK's device-name string). glibc semantics:
     * 1 on success, 0 on failure, all fields zeroed otherwise.
     *
     * dli_fname must outlive the call, hence the static buffer; the only caller
     * runs once at physical-device creation, so the lack of thread safety is
     * not a concern (and a per-call allocation would have to be freed by
     * dlclose() semantics we do not have).
     *
     * The path is normalised to '/' because the caller takes the directory with
     * strrchr(dir, '/') and would otherwise get nothing back. */
    static char stored[1024];
    char buf[1024];
    HMODULE mod = NULL;
    DWORD n, i;

    if (!info)
        return 0;

    memset(info, 0, sizeof(*info));

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod))
        return 0;

    n = GetModuleFileNameA(mod, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf))
        return 0;

    for (i = 0; i < n; i++) {
        if (buf[i] == '\\')
            buf[i] = '/';
    }

    memcpy(stored, buf, n + 1);
    info->dli_fname = stored;
    info->dli_fbase = (void *)mod;
    return 1;
}

/* ------------------------------------------------------------------ */
/* open / close interception                                           */
/* ------------------------------------------------------------------ */

static bool
is_drm_node(const char *path)
{
    return path && _strnicmp(path, "/dev/dri/", 9) == 0;
}

/* The v9 PanVK stack drives the GPU through kbase, and kbase's node is
 * /dev/mali0 (panvk_instance.c probes /dev/mali0..PAN_KBASE_MAX_NODES-1).  It
 * is the same KMD handle behind a different name: everything below open()
 * dispatches on the fd, and ioctl() picks the kbase or DRM decoder from there.
 */
static bool
is_kbase_node(const char *path)
{
    return path && _strnicmp(path, "/dev/mali0", 11) == 0;
}

extern "C" int __cdecl
open(const char *path, int flags, int mode)
{
    if (!is_drm_node(path) && !is_kbase_node(path))
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
        dev->kbase_node = is_kbase_node(path);

        /* kbase completion is delivered by poll()/read() on this fd, so the
         * device owns an auto-reset event that poll() can wait on.  calloc()
         * already left kbase_lock in the unlocked state. */
        dev->kbase_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!dev->kbase_evt) {
            MaliUmdClose(umd);
            free(dev);
            errno = ENOMEM;
            return -1;
        }

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

/* ------------------------------------------------------------------ */
/* kbase uAPI (Arm Job Manager) on top of the Mali KMD                 */
/* ------------------------------------------------------------------ */
/* FourFectVK's Valhall v9 stack drives the GPU through Arm's kbase ioctl
 * ABI rather than DRM_PANFROST_*, so this runtime has to answer the JM subset
 * on the same fd the DRM shim uses.  The KMD stays an accelerator (see
 * V9-PANVK-PLAN.md 3.5, route A1): every ioctl here is a thin translation onto
 * the MaliUmd* helpers and the IOCTL_MALI_* ABI.
 *
 * Two translation points are worth calling out:
 *
 *  - SAME_VA.  On Linux kbase forces SAME_VA on 64-bit allocations, and the
 *    stack then treats the mmap()ed CPU address *as* the GPU VA
 *    (kbase_kmod.c: gpu_va = same_va ? (uintptr_t)cpu_ptr : alloc_gpu_va).
 *    That identity cannot hold inside a Windows process, so we clear the flag
 *    in out.flags and return the KMD's real GPU VA instead: the stack then
 *    uses out.gpu_va, and mmap() resolves the region by that VA.
 *
 *  - Completion.  The stack's default model is asynchronous: it polls the fd
 *    and read()s base_jd_event_v2 records.  Our KMD submits synchronously, so
 *    each accepted atom is completed immediately and one event is queued per
 *    atom (in submit order), which is exactly the ordering the stack's
 *    progress loop expects.
 */

#define MALI_KBASE_EVENT_SIZE 24

/* Queue one completion record.  Caller must not hold kbase_lock. */
static void
kbase_event_push(struct MaliDevice *dev, uint32_t code, uint8_t atom_number)
{
    AcquireSRWLockExclusive(&dev->kbase_lock);
    if (dev->kbase_ev_count >= MALI_KBASE_MAX_EVENTS) {
        ReleaseSRWLockExclusive(&dev->kbase_lock);
        mali_log("kbase: event queue full, dropping atom %u (code 0x%x)",
                 atom_number, code);
        return;
    }

    struct MaliKbaseEvent *e =
        &dev->kbase_ev[(dev->kbase_ev_head + dev->kbase_ev_count) %
                       MALI_KBASE_MAX_EVENTS];

    e->event_code  = code;
    e->atom_number = atom_number;
    e->pad[0] = e->pad[1] = e->pad[2] = 0;
    e->udata[0] = e->udata[1] = 0;
    dev->kbase_ev_count++;
    ReleaseSRWLockExclusive(&dev->kbase_lock);

    SetEvent(dev->kbase_evt);
}

/* Drain up to count bytes of completion records.  Caller must not hold
 * kbase_lock.  Returns bytes copied, or -1/EAGAIN when the queue is empty. */
static int
kbase_event_drain(struct MaliDevice *dev, void *buf, unsigned int count)
{
    uint8_t *out = (uint8_t *)buf;
    unsigned int n = 0;

    AcquireSRWLockExclusive(&dev->kbase_lock);
    while (dev->kbase_ev_count > 0 &&
           n + MALI_KBASE_EVENT_SIZE <= count) {
        memcpy(out + n, &dev->kbase_ev[dev->kbase_ev_head],
               MALI_KBASE_EVENT_SIZE);
        n += MALI_KBASE_EVENT_SIZE;
        dev->kbase_ev_head =
            (dev->kbase_ev_head + 1) % MALI_KBASE_MAX_EVENTS;
        dev->kbase_ev_count--;
    }
    if (dev->kbase_ev_count == 0)
        ResetEvent(dev->kbase_evt);
    ReleaseSRWLockExclusive(&dev->kbase_lock);

    if (n == 0) {
        errno = EAGAIN;
        return -1;
    }
    return (int)n;
}

/* Create a synthetic, already-signalled sync-file fd.  Used to answer a
 * BASE_JD_REQ_SOFT_FENCE_TRIGGER atom: the kernel writes the sync fd back into
 * the caller's struct base_fence { int fd; int stream_fd; }. */
static int
kbase_sync_fd_create(struct MaliDevice *dev)
{
    struct MaliSync *s = sync_new(false, true);
    int fd;

    (void)dev;
    if (!s)
        return -1;

    fd = fd_alloc(MALI_FD_SYNCFILE);
    if (fd < 0) {
        sync_unref(s);
        return -1;
    }

    fd_lock();
    g_fds[fd - MALI_FD_BASE].sync = s;   /* transfers the initial reference */
    fd_unlock();
    return fd;
}

static struct MaliBo *
kbase_bo_by_va(struct MaliDevice *dev, uint64_t va, uint64_t *delta)
{
    struct MaliBo *bo;

    for (bo = dev->bos; bo; bo = bo->next) {
        if (va >= bo->gpu_va && va - bo->gpu_va < bo->size) {
            if (delta)
                *delta = va - bo->gpu_va;
            return bo;
        }
    }
    return NULL;
}

static void
kbase_bo_unlink(struct MaliDevice *dev, struct MaliBo *bo)
{
    struct MaliBo **pp = &dev->bos;

    while (*pp) {
        if (*pp == bo) {
            *pp = bo->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* --- KBASE_IOCTL_GET_GPUPROPS blob ---------------------------------- */

struct KbasePropOut {
    uint8_t *p;     /* NULL while probing the size */
    size_t   n;
};

static void
kprop_u32(struct KbasePropOut *w, uint32_t key, uint32_t v)
{
    if (w->p) {
        uint32_t hdr = (key << 2) | 2u;
        memcpy(w->p + w->n, &hdr, 4);
        memcpy(w->p + w->n + 4, &v, 4);
    }
    w->n += 8;
}

static void
kprop_u64(struct KbasePropOut *w, uint32_t key, uint64_t v)
{
    if (w->p) {
        uint32_t hdr = (key << 2) | 3u;
        memcpy(w->p + w->n, &hdr, 4);
        memcpy(w->p + w->n + 4, &v, 8);
    }
    w->n += 12;
}

/* Serialise exactly the properties kbase_kmod.c's kbase_dev_query_props()
 * reads, sourced from the same GPU registers the DRM_PANFROST_PARAM_* path
 * already uses (see mali_get_param above). */
static void
kbase_gpuprops_fill(struct MaliDevice *dev, struct KbasePropOut *w)
{
    uint32_t gpu_id = gpu_reg(dev, GPU_REG_ID);
    uint64_t shader =
        ((uint64_t)gpu_reg(dev, GPU_REG_SHADER_PRESENT_HI) << 32) |
        gpu_reg(dev, GPU_REG_SHADER_PRESENT_LO);
    uint64_t l2 =
        ((uint64_t)gpu_reg(dev, GPU_REG_L2_PRESENT_HI) << 32) |
        gpu_reg(dev, GPU_REG_L2_PRESENT_LO);

    kprop_u32(w, KBASE_GPUPROP_RAW_GPU_ID, gpu_id);
    kprop_u32(w, KBASE_GPUPROP_PRODUCT_ID, gpu_id >> 16);
    kprop_u32(w, KBASE_GPUPROP_MAJOR_REVISION, (gpu_id >> 12) & 0xfu);
    kprop_u32(w, KBASE_GPUPROP_MINOR_REVISION, (gpu_id >> 4) & 0xffu);
    kprop_u32(w, KBASE_GPUPROP_VERSION_STATUS, gpu_id & 0xfu);

    kprop_u32(w, KBASE_GPUPROP_RAW_CORE_FEATURES,
              gpu_reg(dev, GPU_REG_CORE_FEATURES));
    kprop_u32(w, KBASE_GPUPROP_RAW_TILER_FEATURES,
              gpu_reg(dev, GPU_REG_TILER_FEATURES));
    kprop_u32(w, KBASE_GPUPROP_RAW_MEM_FEATURES,
              gpu_reg(dev, GPU_REG_MEM_FEATURES));
    kprop_u32(w, KBASE_GPUPROP_RAW_MMU_FEATURES,
              gpu_reg(dev, GPU_REG_MMU_FEATURES));
    kprop_u32(w, KBASE_GPUPROP_RAW_L2_FEATURES,
              gpu_reg(dev, GPU_REG_L2_FEATURES));
    kprop_u64(w, KBASE_GPUPROP_RAW_SHADER_PRESENT, shader);
    kprop_u64(w, KBASE_GPUPROP_RAW_L2_PRESENT, l2);

    kprop_u32(w, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0,
              gpu_reg(dev, GPU_REG_TEXTURE_FEATURES(0)));
    kprop_u32(w, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_1,
              gpu_reg(dev, GPU_REG_TEXTURE_FEATURES(1)));
    kprop_u32(w, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_2,
              gpu_reg(dev, GPU_REG_TEXTURE_FEATURES(2)));
    kprop_u32(w, KBASE_GPUPROP_RAW_TEXTURE_FEATURES_3,
              gpu_reg(dev, GPU_REG_TEXTURE_FEATURES(3)));

    kprop_u32(w, KBASE_GPUPROP_RAW_THREAD_MAX_THREADS,
              gpu_reg(dev, GPU_REG_MAX_THREADS));
    kprop_u32(w, KBASE_GPUPROP_RAW_THREAD_MAX_WORKGROUP_SIZE,
              gpu_reg(dev, GPU_REG_MAX_WORKGROUP_SIZE));
    kprop_u32(w, KBASE_GPUPROP_RAW_THREAD_MAX_BARRIER_SIZE,
              gpu_reg(dev, GPU_REG_MAX_BARRIER_SIZE));
    kprop_u32(w, KBASE_GPUPROP_RAW_THREAD_FEATURES,
              gpu_reg(dev, GPU_REG_THREAD_FEATURES));
    kprop_u32(w, KBASE_GPUPROP_TLS_ALLOC,
              gpu_reg(dev, GPU_REG_THREAD_TLS_ALLOC));
    kprop_u32(w, KBASE_GPUPROP_RAW_COHERENCY_MODE,
              gpu_reg(dev, GPU_REG_COHERENCY_FEATURES));
}

/* --- memory ioctls --------------------------------------------------- */

static int
kbase_mem_alloc(struct MaliDevice *dev, uint64_t va_pages, uint64_t commit_pages,
                uint64_t flags, uint64_t *out_flags, uint64_t *out_va)
{
    MALI_UMD_BO bo;
    struct MaliBo *b;
    uint64_t size = va_pages * MALI_KBASE_PAGE_SIZE;

    (void)commit_pages;     /* our KMD backs the whole region up front */

    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    ZeroMemory(&bo, sizeof(bo));
    if (!MaliUmdBoAlloc(dev->umd, size,
                        MALI_BO_CPU_RD | MALI_BO_CPU_WR |
                        MALI_BO_GPU_RD | MALI_BO_GPU_WR, &bo)) {
        mali_log("kbase: MEM_ALLOC of %llu bytes failed",
                 (unsigned long long)size);
        errno = ENOMEM;
        return -1;
    }

    b = (struct MaliBo *)calloc(1, sizeof(*b));
    if (!b) {
        MaliUmdBoFree(dev->umd, bo.Handle);
        errno = ENOMEM;
        return -1;
    }

    b->handle   = bo.Handle;
    b->size     = bo.Size;
    b->gpu_va   = bo.GpuVa;
    b->cpu_va   = bo.CpuVa;
    b->mmap_off = 0;        /* kbase maps by GPU VA, not a synthetic offset */
    b->next     = dev->bos;
    dev->bos    = b;

    /* Base contra SAME_VA: keep the caller's kbase flags (protection, EX, ...)
     * but drop the identity-mapping request and the "needs mmap" hint.  Never
     * mix our KMD's MALI_BO_* bits in here - this word goes straight back to
     * the stack as a BASE_MEM_* set. */
    *out_flags = flags & ~(uint64_t)(BASE_MEM_SAME_VA | BASE_MEM_NEED_MMAP);
    *out_va = b->gpu_va;
    return 0;
}

/* --- KBASE_IOCTL_JOB_SUBMIT ------------------------------------------ */

static int
kbase_job_submit(struct MaliDevice *dev, const struct kbase_ioctl_job_submit *req)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)req->addr;
    uint32_t stride = req->stride ? req->stride : MALI_KBASE_ATOM_STRIDE;
    uint32_t i;

    if ((!base && req->nr_atoms) || stride < 48) {
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < req->nr_atoms; i++) {
        const uint8_t *a = base + (size_t)i * stride;
        uint64_t jc = 0;
        uint32_t core_req;
        uint8_t atom_number;

        memcpy(&jc, a + MALI_KBASE_ATOM_JC, 8);
        memcpy(&core_req, a + MALI_KBASE_ATOM_CORE_REQ, 4);
        atom_number = a[MALI_KBASE_ATOM_NUMBER];

        if (core_req & MALI_BASE_JD_REQ_SOFT_JOB) {
            if (core_req == MALI_BASE_JD_SOFT_FENCE_TRIGGER && jc) {
                /* The hardware kbase writes the fence's sync fd back into the
                 * caller's struct base_fence (fd, stream_fd). */
                struct KbaseBaseFence *f =
                    (struct KbaseBaseFence *)(uintptr_t)jc;
                int sfd = kbase_sync_fd_create(dev);

                if (sfd < 0) {
                    kbase_event_push(dev, MALI_BASE_JD_EVENT_FAILED,
                                     atom_number);
                    continue;
                }
                f->fd = sfd;
                f->stream_fd = -1;
            }
            /* Fence waits and the other soft jobs are already satisfied:
             * submits are synchronous here. */
            kbase_event_push(dev, MALI_BASE_JD_EVENT_DONE, atom_number);
            continue;
        }

        {
            MALI_UMD_SUBMIT sub;
            UINT32 js_status = 0, irq_stat = 0;
            BOOL ok;

            ZeroMemory(&sub, sizeof(sub));
            sub.Slot      = (core_req & MALI_BASE_JD_REQ_FS) ? 1u : 0u;
            sub.Jc        = jc;
            sub.AsNumber  = 0;
            sub.TimeoutMs = 5000;

            ok = MaliUmdJobSubmit(dev->umd, &sub, &js_status, &irq_stat);
            if (!ok)
                mali_log("kbase: atom %u (slot %u, core_req 0x%x) failed",
                         atom_number, sub.Slot, core_req);

            kbase_event_push(dev,
                             ok ? MALI_BASE_JD_EVENT_DONE
                                : MALI_BASE_JD_EVENT_FAILED,
                             atom_number);
        }
    }

    return 0;
}

/* --- ioctl router ----------------------------------------------------- */

static int
kbase_ioctl(struct MaliDevice *dev, unsigned long request, void *arg)
{
    switch (request) {
    case KBASE_IOCTL_VERSION_CHECK_JM: {
        struct kbase_ioctl_version_check *v =
            (struct kbase_ioctl_version_check *)arg;
        if (v) {
            /* kbase_kmod.c only requires major >= 11 (JM flavour). */
            v->major = 11;
            v->minor = 25;
        }
        dev->kbase_handshake = true;
        return 0;
    }

    case KBASE_IOCTL_VERSION_CHECK_CSF:
        /* Must fail: this probe is how the stack decides JM vs CSF, and this
         * SoC (Valhall v9) has no CSF/Command Stream Frontend at all. */
        errno = EPERM;
        return -1;

    case KBASE_IOCTL_SET_FLAGS:
        return 0;

    case KBASE_IOCTL_GET_GPUPROPS: {
        struct kbase_ioctl_get_gpuprops *p =
            (struct kbase_ioctl_get_gpuprops *)arg;
        struct KbasePropOut w;

        w.p = NULL;
        w.n = 0;
        kbase_gpuprops_fill(dev, &w);

        if (p->size == 0)
            return (int)w.n;        /* probe: length is the return value */

        if (p->size < w.n) {
            errno = EINVAL;
            return -1;
        }

        w.p = (uint8_t *)(uintptr_t)p->buffer;
        w.n = 0;
        kbase_gpuprops_fill(dev, &w);
        return 0;
    }

    case KBASE_IOCTL_MEM_ALLOC: {
        union kbase_ioctl_mem_alloc *r =
            (union kbase_ioctl_mem_alloc *)arg;
        uint64_t flags, va;
        int ret = kbase_mem_alloc(dev, r->in.va_pages, r->in.commit_pages,
                                  r->in.flags, &flags, &va);
        if (ret)
            return ret;
        r->out.flags  = flags;
        r->out.gpu_va = va;
        return 0;
    }

    case KBASE_IOCTL_MEM_QUERY: {
        union kbase_ioctl_mem_query *q =
            (union kbase_ioctl_mem_query *)arg;
        struct MaliBo *bo = kbase_bo_by_va(dev, q->in.gpu_addr, NULL);

        if (!bo) {
            errno = EINVAL;
            return -1;
        }

        switch (q->in.query) {
        case KBASE_MEM_QUERY_COMMIT_SIZE:
        case KBASE_MEM_QUERY_VA_SIZE:
            q->out.value = bo->size;
            return 0;
        case KBASE_MEM_QUERY_FLAGS:
            q->out.value = 0;
            return 0;
        default:
            errno = EINVAL;
            return -1;
        }
    }

    case KBASE_IOCTL_MEM_FREE: {
        struct kbase_ioctl_mem_free *f = (struct kbase_ioctl_mem_free *)arg;
        struct MaliBo *bo = kbase_bo_by_va(dev, f->gpu_addr, NULL);

        if (!bo) {
            errno = EINVAL;
            return -1;
        }
        kbase_bo_unlink(dev, bo);
        MaliUmdBoFree(dev->umd, bo->handle);
        free(bo);
        return 0;
    }

    case KBASE_IOCTL_MEM_SYNC:
        /* CPU cache maintenance.  Every BO this shim hands out is mapped
         * uncached by the KMD (attr index 5), so there is nothing to flush;
         * this is only a performance gap, not a correctness one. */
        return 0;

    case KBASE_IOCTL_MEM_COMMIT:
        /* Growable regions are backed in full by MEM_ALLOC here. */
        return 0;

    case KBASE_IOCTL_MEM_JIT_INIT:
    case KBASE_IOCTL_MEM_EXEC_INIT:
        /* Executable/JIT address ranges are ordinary memory on this KMD (the
         * Mali MMU has no XN bit), so the reservations are no-ops. */
        return 0;

    case KBASE_IOCTL_GET_CPU_GPU_TIMEINFO: {
        union kbase_ioctl_get_cpu_gpu_timeinfo *t =
            (union kbase_ioctl_get_cpu_gpu_timeinfo *)arg;
        uint64_t ns = now_ns();

        if (t->in.request_flags & BASE_TIMEINFO_MONOTONIC_FLAG)
            t->out.sec = ns / 1000000000ull;
        if (t->in.request_flags & BASE_TIMEINFO_CYCLE_COUNTER_FLAG)
            t->out.cycle_counter = ns;
        return 0;
    }

    case KBASE_IOCTL_JOB_SUBMIT:
        return kbase_job_submit(dev, (const struct kbase_ioctl_job_submit *)arg);

    case KBASE_IOCTL_KCPU_QUEUE_CREATE: {
        struct kbase_ioctl_kcpu_queue_new *k =
            (struct kbase_ioctl_kcpu_queue_new *)arg;
        if (k)
            k->id = 0;
        return 0;
    }

    case KBASE_IOCTL_KCPU_QUEUE_DELETE:
    case KBASE_IOCTL_KCPU_QUEUE_ENQUEUE:
        /* Only used behind PANVK_KBASE_KCPU_SYNC, which selects the kbase CPU
         * fence model; the default syncobj/sync-file path does not need it. */
        return 0;

    case KBASE_IOCTL_MEM_ALIAS:
    case KBASE_IOCTL_MEM_IMPORT:
        /* Aliasing is a CSF ring-buffer trick; dma-buf import needs a Linux
         * exporter.  Neither is reachable on the JM path we implement. */
        errno = ENOSYS;
        return -1;

    default:
        mali_log("kbase: unhandled ioctl nr %lu",
                 (unsigned long)((request >> 8) & 0xff));
        errno = ENOTTY;
        return -1;
    }
}

extern "C" int __cdecl
poll(struct pollfd *fds, unsigned long nfds, int timeout)
{
    uint64_t deadline = now_ns();
    unsigned long i;

    if (timeout > 0)
        deadline += (uint64_t)timeout * 1000000ull;

    for (;;) {
        int ready = 0;
        HANDLE wait_on = NULL;

        for (i = 0; i < nfds; i++) {
            struct MaliFd *fe;

            fds[i].revents = 0;
            fe = fd_lookup(fds[i].fd);
            if (!fe)
                continue;       /* foreign fd: never ready */

            if (fe->kind == MALI_FD_SYNCFILE) {
                /* Synthetic sync files are already signalled (submits are
                 * synchronous), so they are always readable. */
                fds[i].revents = POLLIN;
                ready++;
            } else if (fe->kind == MALI_FD_DEVICE && fe->dev) {
                bool pending;

                AcquireSRWLockExclusive(&fe->dev->kbase_lock);
                pending = fe->dev->kbase_ev_count > 0;
                ReleaseSRWLockExclusive(&fe->dev->kbase_lock);

                if (pending) {
                    fds[i].revents = POLLIN;
                    ready++;
                } else if (!wait_on) {
                    wait_on = fe->dev->kbase_evt;
                }
            }
        }

        if (ready || timeout == 0)
            return ready;

        if (wait_on) {
            DWORD ms = INFINITE;
            if (timeout > 0) {
                uint64_t now = now_ns();
                if (now >= deadline)
                    return 0;
                ms = (DWORD)((deadline - now + 999999ull) / 1000000ull);
            }
            WaitForSingleObject(wait_on, ms);
        } else {
            /* Nothing in the set can ever become ready (no kbase device fd),
             * so behave like the old stub rather than spinning on an
             * infinite timeout. */
            if (timeout > 0)
                Sleep((DWORD)timeout);
            return 0;
        }
    }
}

/* kbase_kmod.c's CSF/KCPU polls pass a timespec instead of milliseconds.  The
 * signal mask is always NULL at every call site and POSIX signals do not exist
 * here, so it is dropped. */
extern "C" int __cdecl
ppoll(struct pollfd *fds, unsigned long nfds, const struct timespec *timeout,
      const void *sigmask)
{
    int ms = -1;

    (void)sigmask;

    if (timeout) {
        int64_t t = (int64_t)timeout->tv_sec * 1000;
        t += (timeout->tv_nsec + 999999) / 1000000;
        if (t > 2147483647)
            t = 2147483647;
        ms = (int)t;
    }

    return poll(fds, nfds, ms);
}

/* The v9 stack reads base_jd_event_v2 completion records straight off the
 * device fd (panvk_kbase_progress_locked).  Non-synthetic fds keep the CRT
 * behaviour. */
extern "C" int __cdecl
read(int fd, void *buf, unsigned int count)
{
    if (fd >= MALI_FD_BASE) {
        struct MaliFd *fe = fd_lookup(fd);

        if (fe && fe->kind == MALI_FD_DEVICE && fe->dev)
            return kbase_event_drain(fe->dev, buf, count);

        errno = EBADF;
        return -1;
    }
    return _read(fd, buf, count);
}

extern "C" int (__cdecl *__imp_read)(int, void *, unsigned int) = read;

extern "C" int __cdecl
ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    if (fd >= MALI_FD_BASE) {
        struct MaliFd *fe = fd_lookup(fd);

        if (!fe) {
            errno = EBADF;
            return -1;
        }
        if (fe->kind == MALI_FD_DEVICE && fe->dev) {
            if (((request >> 8) & 0xffu) == KBASE_IOCTL_TYPE)
                return kbase_ioctl(fe->dev, request, arg);
            errno = ENOTTY;
            return -1;
        }
        errno = ENOTTY;
        return -1;
    }

    errno = ENOTTY;
    return -1;
}

/* The UCRT has no fcntl(); the only caller is kbase_kmod.c duplicating a
 * dma-buf descriptor, a path that stays disabled here because there is no
 * dma-heap.  Real CRT descriptors go through _dup().  The minimum-descriptor
 * hint and F_DUPFD_CLOEXEC have no meaning on Windows, so they fold away.
 *
 * The runtime build does not force-include cross/msvc-compat.h, so repeat the
 * two constants here; they must stay identical to that header's. */
#ifndef F_DUPFD
#define F_DUPFD          0
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC  1030
#endif

extern "C" int __cdecl
fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;

    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);
    (void)arg;

    if (cmd != F_DUPFD && cmd != F_DUPFD_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }

    if (fd >= MALI_FD_BASE)
        return fd_dup_mali(fd);

    return _dup(fd);
}