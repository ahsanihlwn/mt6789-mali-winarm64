/*
 * mali_irq.h  -  Interrupt plumbing for the Mali KMD (JM backend).
 *
 * The MT6789 device tree lists five interrupt lines for the GPU block:
 *
 *   GPU, MMU, JOB, EVENT, PWR      (GIC SPI 396..400)
 *
 * Only the three the job-manager backend actually services (GPU/MMU/JOB) are
 * wired up here; EVENT and PWR belong to the power-management work (the vendor
 * tree hands those to the MediaTek ged driver).
 *
 * Interrupt resources come from the device's translated resource list. Until
 * the GPU node is published in ACPI (ROADMAP Phase 2g) a ROOT-enumerated
 * device has none, so MaliIrqInitialize accepts zero lines and job completion
 * falls back to polling JOB_IRQ_RAWSTAT.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include "mali_job.h"

#define MALI_IRQ_LINE_GPU     0
#define MALI_IRQ_LINE_MMU     1
#define MALI_IRQ_LINE_JOB     2
#define MALI_IRQ_LINE_EVENT   3
#define MALI_IRQ_LINE_PWR     4
#define MALI_IRQ_LINES        5
#define MALI_IRQ_LINES_WIRED  3   /* GPU / MMU / JOB, in DT order */

typedef struct _MALI_IRQ_LINE {
    struct _MALI_KMD_DEVICE *Ctx;
    ULONG Line;
    ULONG RawStatusOfs;
    ULONG ClearOfs;
    ULONG MaskOfs;
    ULONG StatusOfs;
} MALI_IRQ_LINE, *PMALI_IRQ_LINE;

typedef struct _MALI_IRQ {
    KSPIN_LOCK   Lock;
    ULONG        Pending[MALI_IRQ_LINES];
    ULONG        SavedMask[MALI_IRQ_LINES];
    ULONG        Masked[MALI_IRQ_LINES];
    volatile LONG DpcQueued[MALI_IRQ_LINES];
    WDFINTERRUPT Obj[MALI_IRQ_LINES];

    ULONG        LineCount;
    ULONG        Count[MALI_IRQ_LINES];
    ULONG        GpuFaultCount;
    ULONG        MmuFaultCount;
    ULONG        JobDoneCount;
    ULONG        JobErrorCount;
    ULONG        LastGpuFaultStatus;
    ULONG        LastMmuFaultStatus;
    UINT64       LastGpuFaultAddress;
    UINT64       LastMmuFaultAddress;
} MALI_IRQ, *PMALI_IRQ;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MALI_IRQ_LINE, MaliIrqGetLineContext)

NTSTATUS MaliIrqInitialize(struct _MALI_KMD_DEVICE *Ctx, WDFCMRESLIST ResourcesTranslated);
VOID     MaliIrqCleanup(struct _MALI_KMD_DEVICE *Ctx);

EVT_WDF_INTERRUPT_ISR MaliIrqIsr;
EVT_WDF_INTERRUPT_DPC MaliIrqDpc;
