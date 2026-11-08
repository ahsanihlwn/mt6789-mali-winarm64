/*
 * MaliIrq.c  -  Interrupt handling for the Mali KMD (JM backend).
 *
 * Three lines are wired: GPU, MMU, JOB (MT6789 DT order). Each line gets its
 * own WDFINTERRUPT and its own DPC so the mask/clear bookkeeping cannot race
 * between lines.
 *
 * The ISR runs at DIRQL: it snapshots the line's status register, records the
 * bits, *disables* the line (MASK := 0) so a level-triggered source cannot
 * storm, and queues the DPC. The DPC clears the raw bits, records statistics
 * and the fault/status details, then restores the mask. Because the source is
 * level-triggered, anything that arrives between clear and re-mask re-fires the
 * ISR, so no completion is lost.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include "MaliKmd.h"

/* RAWSTAT, CLEAR, MASK, STATUS per line (0 = not wired). */
static const ULONG g_LineRegs[MALI_IRQ_LINES][4] = {
    { MALI_GPU_IRQ_RAWSTAT, MALI_GPU_IRQ_CLEAR, MALI_GPU_IRQ_MASK, MALI_GPU_IRQ_STATUS },
    { MALI_MMU_IRQ_RAWSTAT, MALI_MMU_IRQ_CLEAR, MALI_MMU_IRQ_MASK, MALI_MMU_IRQ_STATUS },
    { MALI_JOB_IRQ_RAWSTAT, MALI_JOB_IRQ_CLEAR, MALI_JOB_IRQ_MASK, MALI_JOB_IRQ_STATUS },
    { 0, 0, 0, 0 },   /* EVENT */
    { 0, 0, 0, 0 },   /* PWR   */
};

static ULONG MaliIrqRead(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Offset)
{
    return MaliHwReadSubsysReg(Ctx, MTK_REGION_GPU, Offset);
}

static VOID MaliIrqWrite(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Offset, _In_ ULONG Value)
{
    (VOID)MaliHwWriteReg(Ctx, MTK_REGION_GPU, Offset, Value);
}

static VOID MaliIrqHandleGpu(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Pending)
{
    if ((Pending & (MALI_GPU_FAULT | MALI_MULTIPLE_GPU_FAULTS)) == 0) {
        return;
    }

    {
        ULONG Fs = MaliIrqRead(Ctx, MALI_GPU_FAULTSTATUS);
        UINT64 Addr = (UINT64)MaliIrqRead(Ctx, MALI_GPU_FAULTADDRESS_LO)
                    | ((UINT64)MaliIrqRead(Ctx, MALI_GPU_FAULTADDRESS_HI) << 32);

        Ctx->Irq.GpuFaultCount++;
        Ctx->Irq.LastGpuFaultStatus = Fs;
        Ctx->Irq.LastGpuFaultAddress = Addr;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "MaliKmd: GPU fault status=0x%08X address=0x%llx\n",
                   Fs, (unsigned long long)Addr);
    }
}

static VOID MaliIrqHandleMmu(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Pending)
{
    ULONG n;
    for (n = 0; n < 16; n++) {
        if ((Pending & (MALI_MMU_PAGE_FAULT(n) | MALI_MMU_BUS_ERROR(n))) == 0) {
            continue;
        }
        {
            ULONG Fs = MaliIrqRead(Ctx, MALI_MMU_AS_REG(n, MALI_AS_FAULTSTATUS));
            UINT64 Addr = (UINT64)MaliIrqRead(Ctx, MALI_MMU_AS_REG(n, MALI_AS_FAULTADDRESS_LO))
                        | ((UINT64)MaliIrqRead(Ctx, MALI_MMU_AS_REG(n, MALI_AS_FAULTADDRESS_HI)) << 32);

            Ctx->Irq.MmuFaultCount++;
            Ctx->Irq.LastMmuFaultStatus = Fs;
            Ctx->Irq.LastMmuFaultAddress = Addr;
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "MaliKmd: MMU fault AS%u status=0x%08X address=0x%llx\n",
                       n, Fs, (unsigned long long)Addr);
        }
    }
}

static VOID MaliIrqHandleJob(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Pending)
{
    ULONG Slot;
    ULONG Slots = Ctx->Job.SlotCount;

    if (Slots == 0 || Slots > MALI_JOB_SLOT_MAX) {
        Slots = MALI_JOB_SLOT_MAX;
    }

    for (Slot = 0; Slot < Slots; Slot++) {
        ULONG Done  = MALI_JOB_IRQ_SLOT_DONE(Slot);
        ULONG Error = MALI_JOB_IRQ_SLOT_ERROR(Slot);

        if ((Pending & (Done | Error)) == 0) {
            continue;
        }

        {
            ULONG St = MaliIrqRead(Ctx,
                         MALI_JOB_SLOT_BASE + Slot * MALI_JOB_SLOT_STRIDE + MALI_JS_STATUS);

            Ctx->Job.SlotIrqStatus[Slot] = St;
            InterlockedIncrement((volatile LONG *)&Ctx->Job.SlotGen[Slot]);

            if ((Pending & Error) != 0) {
                Ctx->Irq.JobErrorCount++;
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "MaliKmd: job slot %u error, JS_STATUS=0x%02X\n", Slot, St);
            } else {
                Ctx->Irq.JobDoneCount++;
            }
        }
    }
}

BOOLEAN MaliIrqIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    PMALI_IRQ_LINE Line = MaliIrqGetLineContext(Interrupt);
    PMALI_KMD_DEVICE Ctx = Line->Ctx;
    ULONG Status;

    UNREFERENCED_PARAMETER(MessageID);

    if (Line->StatusOfs == 0) {
        return FALSE;
    }

    Status = MaliIrqRead(Ctx, Line->StatusOfs);
    if (Status == 0) {
        return FALSE;   /* not ours: leave it for the real owner */
    }

    KeAcquireSpinLockAtDpcLevel(&Ctx->Irq.Lock);
    Ctx->Irq.Pending[Line->Line] |= Status;
    if (Ctx->Irq.Masked[Line->Line] == 0) {
        /* MASK: 1 = enabled, so writing 0 disables this line until the DPC
         * has cleared the raw bits. */
        Ctx->Irq.SavedMask[Line->Line] = MaliIrqRead(Ctx, Line->MaskOfs);
        MaliIrqWrite(Ctx, Line->MaskOfs, 0);
        Ctx->Irq.Masked[Line->Line] = 1;
    }
    KeReleaseSpinLockFromDpcLevel(&Ctx->Irq.Lock);

    if (InterlockedExchange(&Ctx->Irq.DpcQueued[Line->Line], 1) == 0) {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
    return TRUE;
}

VOID MaliIrqDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    PMALI_IRQ_LINE Line = MaliIrqGetLineContext(Interrupt);
    PMALI_KMD_DEVICE Ctx = Line->Ctx;
    ULONG Pending, Raw;

    UNREFERENCED_PARAMETER(AssociatedObject);

    KeAcquireSpinLockAtDpcLevel(&Ctx->Irq.Lock);
    Pending = Ctx->Irq.Pending[Line->Line];
    Ctx->Irq.Pending[Line->Line] = 0;
    KeReleaseSpinLockFromDpcLevel(&Ctx->Irq.Lock);

    /* Pick up anything that asserted after the ISR snapshot, then acknowledge. */
    Raw = MaliIrqRead(Ctx, Line->RawStatusOfs);
    Pending |= Raw;
    if (Pending != 0 && Line->ClearOfs != 0) {
        MaliIrqWrite(Ctx, Line->ClearOfs, Pending);
    }

    if (Pending != 0) {
        Ctx->Irq.Count[Line->Line]++;
        Ctx->Job.LastIrqStat = Pending;

        switch (Line->Line) {
        case MALI_IRQ_LINE_GPU: MaliIrqHandleGpu(Ctx, Pending); break;
        case MALI_IRQ_LINE_MMU: MaliIrqHandleMmu(Ctx, Pending); break;
        case MALI_IRQ_LINE_JOB: MaliIrqHandleJob(Ctx, Pending); break;
        default: break;
        }
    }

    KeAcquireSpinLockAtDpcLevel(&Ctx->Irq.Lock);
    if (Ctx->Irq.Masked[Line->Line] != 0) {
        MaliIrqWrite(Ctx, Line->MaskOfs, Ctx->Irq.SavedMask[Line->Line]);
        Ctx->Irq.Masked[Line->Line] = 0;
    }
    KeReleaseSpinLockFromDpcLevel(&Ctx->Irq.Lock);

    InterlockedExchange(&Ctx->Irq.DpcQueued[Line->Line], 0);
}

static NTSTATUS MaliIrqCreateLine(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Line)
{
    WDF_INTERRUPT_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attrs;
    WDFINTERRUPT Int;
    PMALI_IRQ_LINE CtxLine;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attrs, MALI_IRQ_LINE);
    WDF_INTERRUPT_CONFIG_INIT(&Config, MaliIrqIsr, MaliIrqDpc);
    Config.AutomaticSerialization = FALSE;
    Config.PassiveHandling = FALSE;

    Status = WdfInterruptCreate(Ctx->Device, &Config, &Attrs, &Int);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    CtxLine = MaliIrqGetLineContext(Int);
    CtxLine->Ctx          = Ctx;
    CtxLine->Line         = Line;
    CtxLine->RawStatusOfs = g_LineRegs[Line][0];
    CtxLine->ClearOfs     = g_LineRegs[Line][1];
    CtxLine->MaskOfs      = g_LineRegs[Line][2];
    CtxLine->StatusOfs    = g_LineRegs[Line][3];

    Ctx->Irq.Obj[Line] = Int;

    /* Explicit enable: the device is in D0 here (EvtDevicePrepareHardware).
     * WdfInterruptEnable returns VOID; it cannot fail for a valid handle. */
    WdfInterruptEnable(Int);
    return STATUS_SUCCESS;
}

NTSTATUS MaliIrqInitialize(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFCMRESLIST ResourcesTranslated)
{
    ULONG Count, i, Line = 0;

    RtlZeroMemory(&Ctx->Irq, sizeof(Ctx->Irq));
    KeInitializeSpinLock(&Ctx->Irq.Lock);
    Ctx->Irq.LineCount = 0;
    Ctx->IrqConnected = 0;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < Count && Line < MALI_IRQ_LINES_WIRED; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (Desc == NULL || Desc->Type != CmResourceTypeInterrupt) {
            continue;
        }
        if (!NT_SUCCESS(MaliIrqCreateLine(Ctx, Line))) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                       "MaliKmd: interrupt object %u not created; stopping\n", Line);
            break;
        }
        Line++;
    }

    Ctx->Irq.LineCount = Line;
    Ctx->IrqConnected  = (Line != 0) ? 1u : 0u;

    if (Line == 0) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "MaliKmd: no interrupt resources assigned; job completion will be "
                   "polled (ACPI node needed, ROADMAP 2g)\n");
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                   "MaliKmd: %u interrupt line(s) connected (GPU/MMU/JOB)\n", Line);
    }
    return STATUS_SUCCESS;
}

VOID MaliIrqCleanup(_In_ PMALI_KMD_DEVICE Ctx)
{
    ULONG i;

    for (i = 0; i < MALI_IRQ_LINES; i++) {
        if (Ctx->Irq.Obj[i] != NULL) {
            WdfInterruptDisable(Ctx->Irq.Obj[i]);
            WdfObjectDelete(Ctx->Irq.Obj[i]);
            Ctx->Irq.Obj[i] = NULL;
        }
    }
    Ctx->Irq.LineCount = 0;
    Ctx->IrqConnected  = 0;
}
