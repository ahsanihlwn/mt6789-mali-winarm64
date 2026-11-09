/*
 * xf86drm.h  -  minimal libdrm API surface for Windows (MSVC/ARM64).
 *
 * Mesa's panfrost/lib/kmod/pan_kmod.{c,h} and panfrost_kmod.c include
 * <xf86drm.h> and use the libdrm entry points to talk to a DRM render node.
 * Windows has neither libdrm nor DRM; this project substitutes a shim
 * (windrv/mesa/runtime/mali_drm.cpp) that presents a single synthetic
 * /dev/dri/renderD128 backed by our Mali KMD (see windrv/PANVK-SHIM.md).
 *
 * This header only declares the subset Mesa 25.0.7 needs. The implementation
 * lives entirely in mali_drm.cpp; keep the two in sync.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_XF86DRM_H
#define MALI_COMPAT_XF86DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>   /* O_CLOEXEC, used by drm.h's DRM_CLOEXEC */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Version query (DRM_IOCTL_VERSION)                                   */
/* ------------------------------------------------------------------ */
typedef struct _drmVersion {
    int    version_major;
    int    version_minor;
    int    version_patchlevel;
    int    name_len;
    char  *name;
    int    date_len;
    char  *date;
    int    desc_len;
    char  *desc;
} drmVersion, *drmVersionPtr;

/* ------------------------------------------------------------------ */
/* Device enumeration (drmGetDevices2)                                 */
/* ------------------------------------------------------------------ */
#define DRM_NODE_PRIMARY  0
#define DRM_NODE_CONTROL  1
#define DRM_NODE_RENDER   2
#define DRM_NODE_MAX      3

typedef enum {
    DRM_BUS_PCI      = 0,
    DRM_BUS_USB      = 1,
    DRM_BUS_PLATFORM = 2,
    DRM_BUS_HOST1X   = 3,
} drmDeviceBusType;

struct _drmPciBusInfo {
    uint16_t domain;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
};

struct _drmPciDeviceInfo {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint8_t  revision_id;
};

struct _drmUsbBusInfo {
    uint8_t bus;
    uint8_t dev;
};

struct _drmUsbDeviceInfo {
    uint16_t vendor;
    uint16_t product;
};

struct _drmPlatformBusInfo {
    char fullname[512];
};

struct _drmPlatformDeviceInfo {
    char **compatible;
};

struct _drmHost1xBusInfo {
    struct _drmPciBusInfo pci;
};

struct _drmHost1xDeviceInfo {
    char **compatible;
};

typedef struct _drmDevice {
    char **nodes;
    int    available_nodes;
    drmDeviceBusType bustype;
    union {
        struct _drmPciBusInfo      pci;
        struct _drmUsbBusInfo      usb;
        struct _drmPlatformBusInfo platform;
        struct _drmHost1xBusInfo   host1x;
    } businfo;
    union {
        struct _drmPciDeviceInfo      pci;
        struct _drmUsbDeviceInfo      usb;
        struct _drmPlatformDeviceInfo platform;
        struct _drmHost1xDeviceInfo   host1x;
    } deviceinfo;
} drmDevice, *drmDevicePtr;

/* ------------------------------------------------------------------ */
/* Flags                                                               */
/* ------------------------------------------------------------------ */
/* DRM_CLOEXEC / DRM_RDWR are defined by drm-uapi/drm.h, which every user of
 * this header also includes. */

/* ------------------------------------------------------------------ */
/* Core calls                                                          */
/* ------------------------------------------------------------------ */
int  drmIoctl(int fd, unsigned long request, void *arg);

drmVersionPtr drmGetVersion(int fd);
void          drmFreeVersion(drmVersionPtr version);

int  drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
int  drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int  drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
int  drmGetNodeTypeFromFd(int fd);

int  drmGetCap(int fd, uint64_t capability, uint64_t *value);

typedef unsigned int drm_magic_t;
int  drmGetMagic(int fd, drm_magic_t *magic);
int  drmAuthMagic(int fd, drm_magic_t magic);

/* ------------------------------------------------------------------ */
/* PRIME (dma-buf)                                                     */
/* ------------------------------------------------------------------ */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmCloseBufferHandle(int fd, uint32_t handle);

/* ------------------------------------------------------------------ */
/* Synchronization objects                                             */
/* ------------------------------------------------------------------ */
#define DRM_SYNCOBJ_CREATE_SIGNALED                          (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL                      (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT               (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE                (1 << 2)
#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED               (1 << 0)

#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE      (1 << 0)
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE      (1 << 0)

#define DRM_SYNCOBJ_TRANSFER_FLAGS_QUERY_CURRENT             (1 << 0)
#define DRM_SYNCOBJ_TRANSFER_FLAGS_QUERY_PENDING             (1 << 1)

int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

int drmSyncobjWait(int fd, const uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags,
                   uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, const uint32_t *handles,
                           const uint64_t *points, unsigned num_handles,
                           int64_t timeout_nsec, unsigned flags,
                           uint32_t *first_signaled);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                             const uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineQuery(int fd, const uint32_t *handles, uint64_t *points,
                            uint32_t handle_count);
int drmSyncobjQuery(int fd, const uint32_t *handles, uint64_t *points,
                    uint32_t handle_count);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point,
                       uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* MALI_COMPAT_XF86DRM_H */