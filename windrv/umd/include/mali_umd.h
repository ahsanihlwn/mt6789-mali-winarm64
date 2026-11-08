/*
 * mali_umd.h  -  Public Win32 user-mode API for the MediaTek Mali-G57 KMD.
 *
 * This is the "winsys" layer: a small, typed wrapper around the kernel IOCTL
 * ABI (mali_kmd_ioctl.h, ABI v2). Everything above it — the Vulkan ICD, and in
 * turn DXVK / vkd3d-proton / Zink — talks to the GPU through this interface.
 *
 * Design rules:
 *   - No C runtime dependency: the implementation only imports kernel32 and
 *     setupapi, so it links into a CRT-free image (matching the project style).
 *   - One source of truth for the ABI: this header includes the kernel header
 *     rather than redeclaring structures.
 *   - Thin by design: it does not build job chains. The caller (ICD) owns the
 *     GPU-visible byte layout; the UMD only marshals IOCTLs.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_UMD_H
#define MALI_UMD_H

#include <windows.h>
#include "mali_kmd_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _MALI_UMD_DEVICE MALI_UMD_DEVICE;
typedef MALI_UMD_DEVICE *PMALI_UMD_DEVICE;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Open the first present Mali KMD device interface. NULL if unavailable. */
PMALI_UMD_DEVICE MaliUmdOpen(void);

/* Open one device interface by its wide device path (from the setup API). */
PMALI_UMD_DEVICE MaliUmdOpenPath(_In_ PCWSTR DevicePath);

/* Enumerate present device interface paths. Returns the count written (may be
 * capped at Capacity); the paths are copied into Paths[Capacity][260]. */
ULONG MaliUmdEnumerate(_Out_writes_(Capacity) PWSTR Paths, _In_ ULONG Capacity);

void MaliUmdClose(_In_ PMALI_UMD_DEVICE Dev);

/* Raw IOCTL passthrough (escape hatch used by bring-up tools). */
BOOL MaliUmdIoctl(_In_ PMALI_UMD_DEVICE Dev, _In_ DWORD Code,
                  _In_reads_bytes_opt_(InLen)  const void *In,  _In_ DWORD InLen,
                  _Out_writes_bytes_opt_(OutLen) void      *Out, _In_ DWORD OutLen);

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */

BOOL MaliUmdGetVersion (_In_ PMALI_UMD_DEVICE Dev, _Out_ MALI_KMD_VERSION   *Version);
BOOL MaliUmdGetCaps    (_In_ PMALI_UMD_DEVICE Dev, _Out_ MALI_KMD_CAPS      *Caps);
BOOL MaliUmdGetIrqStats(_In_ PMALI_UMD_DEVICE Dev, _Out_ MALI_KMD_IRQ_STATS *Stats);

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */

BOOL MaliUmdPower(_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 State);

#define MaliUmdPowerOn(d)  MaliUmdPower((d), MALI_POWER_ON)
#define MaliUmdPowerOff(d) MaliUmdPower((d), MALI_POWER_OFF)

/* ------------------------------------------------------------------ */
/* Raw MMIO (bring-up / diagnostics only)                              */
/* ------------------------------------------------------------------ */

BOOL MaliUmdRegRead (_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Block, _In_ UINT32 Offset, _Out_ UINT32 *Value);
BOOL MaliUmdRegWrite(_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Block, _In_ UINT32 Offset, _In_ UINT32 Value);

/* ------------------------------------------------------------------ */
/* GPU memory (buffer objects)                                         */
/* ------------------------------------------------------------------ */

typedef struct _MALI_UMD_BO {
    UINT32 Handle;      /* KMD BO handle                                  */
    UINT32 Flags;       /* MALI_BO_*                                      */
    UINT64 Size;        /* bytes, page rounded                            */
    UINT64 GpuVa;       /* GPU virtual address (identity == physical)     */
    void  *CpuVa;       /* CPU mapping, valid in this process             */
} MALI_UMD_BO;

BOOL MaliUmdBoAlloc(_In_ PMALI_UMD_DEVICE Dev, _In_ UINT64 Size, _In_ UINT32 Flags,
                    _Out_ MALI_UMD_BO *Bo);
BOOL MaliUmdBoInfo (_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Handle, _Out_ MALI_KMD_BO_INFO *Info);
BOOL MaliUmdBoFree (_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Handle);

/* ------------------------------------------------------------------ */
/* MMU                                                                 */
/* ------------------------------------------------------------------ */

BOOL MaliUmdMmuEnable (_In_ PMALI_UMD_DEVICE Dev);
BOOL MaliUmdMmuDisable(_In_ PMALI_UMD_DEVICE Dev);
BOOL MaliUmdMmuFault  (_In_ PMALI_UMD_DEVICE Dev, _Out_ MALI_KMD_MMU_FAULT *Fault);

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

typedef struct _MALI_UMD_SUBMIT {
    UINT32 Slot;        /* job slot, 0-based                              */
    UINT32 Flags;       /* MALI_JOB_FLAG_*                                */
    UINT64 Jc;          /* GPU VA of the chain head (16-byte aligned)     */
    UINT64 Affinity;    /* shader core mask, 0 = driver default           */
    UINT32 AsNumber;    /* address space, 0                               */
    UINT32 TimeoutMs;   /* 0 = fire and forget, >0 = wait this long       */
} MALI_UMD_SUBMIT;

BOOL MaliUmdJobSubmit(_In_ PMALI_UMD_DEVICE Dev, _In_ const MALI_UMD_SUBMIT *Submit,
                      _Out_opt_ UINT32 *JsStatus, _Out_opt_ UINT32 *IrqStat);
BOOL MaliUmdJobStatus(_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Slot,
                      _Out_ MALI_KMD_JOB_STATUS *Status);
BOOL MaliUmdJobStop  (_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Slot, _In_ BOOL Hard);
BOOL MaliUmdJobSelfTest(_In_ PMALI_UMD_DEVICE Dev, _In_ UINT32 Slot,
                        _Out_opt_ UINT32 *JsStatus, _Out_opt_ UINT32 *IrqStat);

#ifdef __cplusplus
}
#endif

#endif /* MALI_UMD_H */
