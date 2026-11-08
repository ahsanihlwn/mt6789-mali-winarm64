/*
 * MaliKmd.h  -  Private driver context and prototypes.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "mtk_gpu_soc.h"
#include "mali_kmd_ioctl.h"
#include "mali_mmu.h"
#include "mali_mem.h"
#include "mali_job.h"
#include "mali_irq.h"

typedef struct _MALI_KMD_DEVICE {
    WDFDEVICE Device;

    const MTK_GPU_SOC_DESC *Soc;

    /* Mapped MMIO, indexed by MTK_REGION_*. NULL when not mapped. */
    PVOID Base[MTK_REGION_COUNT];
    ULONG MapSize[MTK_REGION_COUNT];

    ULONG PowerState;             /* MALI_POWER_*            */
    ULONG IrqConnected;

    ULONG GpuIdRegister;          /* cached 0x13000000 + 0x00 */
    ULONG CoreFeatures;
    ULONG MmuFeatures;
    ULONG ShaderPresent;

    KSPIN_LOCK RegLock;           /* serialises power transitions */

    /* GPU memory + MMU */
    MALI_MMU    Mmu;
    LIST_ENTRY  BoList;
    KGUARDED_MUTEX BoLock;
    ULONG       NextBoHandle;

    /* Job Manager + interrupts */
    MALI_JOB    Job;
    MALI_IRQ    Irq;
} MALI_KMD_DEVICE, *PMALI_KMD_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MALI_KMD_DEVICE, MaliKmdGetDeviceContext)

/* ---- MaliHw.c ---------------------------------------------------- */
NTSTATUS MaliHwInitialize(_In_ PMALI_KMD_DEVICE Ctx);
VOID     MaliHwCleanup(_In_ PMALI_KMD_DEVICE Ctx);
NTSTATUS MaliHwPowerOn(_In_ PMALI_KMD_DEVICE Ctx);
NTSTATUS MaliHwPowerOff(_In_ PMALI_KMD_DEVICE Ctx);
VOID     MaliHwGetPwrStatus(_In_ PMALI_KMD_DEVICE Ctx, _Out_ MALI_KMD_PWR_STATUS *Out);
NTSTATUS MaliHwReadReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Block,
                       _In_ ULONG Offset, _Out_ PULONG Value);
NTSTATUS MaliHwWriteReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Block,
                        _In_ ULONG Offset, _In_ ULONG Value);
ULONG    MaliHwReadSubsysReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Block, _In_ ULONG Offset);

/* ---- MaliIo.c ---------------------------------------------------- */
NTSTATUS MaliIoCreateQueues(_In_ WDFDEVICE Device);
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL MaliEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE MaliEvtIoCanceledOnQueue;

/* Runs in the caller's process context (needed for user BO mappings). */
EVT_WDF_IO_IN_CALLER_CONTEXT        MaliEvtIoInCallerContext;
VOID MaliHandleBoInCallerContext(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);