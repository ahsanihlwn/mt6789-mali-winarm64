/*
 * MaliJob.c  -  Job Manager submission for the Mali KMD (JM backend).
 *
 * The kernel does not build job chains: userspace (the Vulkan ICD) does, in a
 * BO, and hands the KMD the GPU virtual address of the chain head. The KMD
 * only programs the job slot and reports completion:
 *
 *   JS_HEAD_NEXT_LO/HI   <- job chain head (GPU VA)
 *   JS_AFFINITY_NEXT     <- shader core mask
 *   JS_CONFIG_NEXT       <- address space, cache flushes, thread priority
 *   JS_COMMAND_NEXT      <- JS_COMMAND_START
 *
 * Completion arrives either as a JOB interrupt (handled in MaliIrq.c, which
 * bumps MaliJob.SlotGen) or by polling JOB_IRQ_RAWSTAT. The poll path is the
 * one that works before interrupt resources exist.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include "MaliKmd.h"

#define MALI_JOB_DEFAULT_TIMEOUT_MS  2000
#define MALI_JOB_POLL_STEP_US        1000

static ULONG MaliJobReadReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Offset)
{
    return MaliHwReadSubsysReg(Ctx, MTK_REGION_GPU, Offset);
}

static VOID MaliJobWriteReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Offset, _In_ ULONG Value)
{
    (VOID)MaliHwWriteReg(Ctx, MTK_REGION_GPU, Offset, Value);
}

NTSTATUS MaliJobInitialize(_In_ PMALI_KMD_DEVICE Ctx)
{
    RtlZeroMemory(&Ctx->Job, sizeof(Ctx->Job));
    Ctx->Job.SlotCount = 1;
    return STATUS_SUCCESS;
}

VOID MaliJobCleanup(_In_ PMALI_KMD_DEVICE Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

ULONG MaliJobSlotCount(_In_ PMALI_KMD_DEVICE Ctx)
{
    ULONG Present = MaliJobReadReg(Ctx, MALI_JS_PRESENT);
    ULONG Count = 0;

    /* JS_PRESENT is a bitmask of implemented slots. A failed read returns
     * 0 or all-ones, neither of which is a sane mask. */
    if (Present == 0u || Present == 0xFFFFFFFFu) {
        return 1;
    }
    while (Present != 0u) {
        Count += (Present & 1u);
        Present >>= 1;
    }
    return (Count == 0u) ? 1u : Count;
}

static BOOLEAN MaliJobSlotValid(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Slot)
{
    return (Slot < MALI_JOB_SLOT_MAX && Slot < Ctx->Job.SlotCount);
}

NTSTATUS MaliJobSubmit(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Slot, _In_ UINT64 Jc,
                       _In_ UINT64 Affinity, _In_ ULONG AsNumber, _In_ ULONG Flags,
                       _In_ ULONG TimeoutMs, _Out_ PULONG OutStatus, _Out_ PULONG OutIrq)
{
    NTSTATUS Status;
    ULONG Base, DoneMask, ErrMask;
    ULONG Cfg, Stat = 0, Irq = 0, Gen, i;
    BOOLEAN Completed = FALSE;

    if (Jc == 0 || (Jc & 0xF) != 0) {
        return STATUS_INVALID_PARAMETER;   /* chain head must be 16-byte aligned */
    }

    /* Bring the GPU up on demand: the ICD should not have to. */
    Status = MaliHwPowerOn(Ctx);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = MaliMmuEnable(Ctx);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx->Job.SlotCount = MaliJobSlotCount(Ctx);
    if (!MaliJobSlotValid(Ctx, Slot)) {
        return STATUS_INVALID_PARAMETER;
    }

    Base     = MALI_JOB_SLOT_BASE + Slot * MALI_JOB_SLOT_STRIDE;
    DoneMask = MALI_JOB_IRQ_SLOT_DONE(Slot);
    ErrMask  = MALI_JOB_IRQ_SLOT_ERROR(Slot);

    /*
     * Affinity must never be zero: kbase substitutes the available shader
     * mask when the computed mask is empty, and a zero mask on the wire is an
     * AFFINITY_FAULT (JS_STATUS 0x44). We power every present core on, so the
     * present mask is the right default.
     */
    if (Affinity == 0) {
        Affinity = (UINT64)Ctx->ShaderPresent;
        if (Affinity == 0 || Affinity == 0xFFFFFFFFu) {
            Affinity = 1;   /* last resort: shader core 0 */
        }
    }

    /* Drop any stale completion for this slot so the fence cannot fire early. */
    MaliJobWriteReg(Ctx, MALI_JOB_IRQ_CLEAR, DoneMask | ErrMask);

    /* Make sure this slot's interrupt is enabled (MASK: 1 = enabled). Writing
     * only on the submit path keeps driver load free of writes to a block that
     * may not be powered yet. */
    MaliJobWriteReg(Ctx, MALI_JOB_IRQ_MASK,
                    MaliJobReadReg(Ctx, MALI_JOB_IRQ_MASK) | DoneMask | ErrMask);

    Cfg = AsNumber & 0x3Fu;
    if (!(Flags & MALI_JOB_FLAG_NO_MMU)) {
        Cfg |= MALI_JS_CONFIG_START_MMU;
    }
    if (Flags & MALI_JOB_FLAG_JOB_CHAIN) {
        Cfg |= MALI_JS_CONFIG_JOB_CHAIN_FLAG;
    }
    Cfg |= MALI_JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE;
    Cfg |= MALI_JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE;
    /*
     * Flush reduction is deliberately NOT enabled: kbase only turns it on
     * together with a valid JS_FLUSH_ID_NEXT value, which this driver does not
     * track yet. The conservative clean+invalidate is always correct.
     */
    Cfg |= MALI_JS_CONFIG_THREAD_PRI(8);

    /* Snapshot the IRQ generation *before* starting, so a completion that
     * lands immediately cannot be missed. */
    Gen = Ctx->Job.SlotGen[Slot];

    MaliJobWriteReg(Ctx, Base + MALI_JS_HEAD_NEXT_LO,     (ULONG)(Jc & 0xFFFFFFFFu));
    MaliJobWriteReg(Ctx, Base + MALI_JS_HEAD_NEXT_HI,     (ULONG)(Jc >> 32));
    MaliJobWriteReg(Ctx, Base + MALI_JS_AFFINITY_NEXT_LO, (ULONG)(Affinity & 0xFFFFFFFFu));
    MaliJobWriteReg(Ctx, Base + MALI_JS_AFFINITY_NEXT_HI, (ULONG)(Affinity >> 32));
    MaliJobWriteReg(Ctx, Base + MALI_JS_CONFIG_NEXT,      Cfg);
    MaliJobWriteReg(Ctx, Base + MALI_JS_COMMAND_NEXT,     MALI_JS_COMMAND_START);

    if (TimeoutMs == 0) {
        *OutStatus = MaliJobReadReg(Ctx, Base + MALI_JS_STATUS);
        *OutIrq    = MaliJobReadReg(Ctx, MALI_JOB_IRQ_RAWSTAT) & (DoneMask | ErrMask);
        return STATUS_SUCCESS;
    }
    if (TimeoutMs == 0xFFFFFFFFu) {
        TimeoutMs = MALI_JOB_DEFAULT_TIMEOUT_MS;
    }

    for (i = 0; i < TimeoutMs; i++) {
        Irq = MaliJobReadReg(Ctx, MALI_JOB_IRQ_RAWSTAT) & (DoneMask | ErrMask);
        if (Irq != 0 || Ctx->Job.SlotGen[Slot] != Gen) {
            Completed = TRUE;
            break;
        }
        KeStallExecutionProcessor(MALI_JOB_POLL_STEP_US);
    }

    Stat = MaliJobReadReg(Ctx, Base + MALI_JS_STATUS);
    MaliJobWriteReg(Ctx, MALI_JOB_IRQ_CLEAR, DoneMask | ErrMask);

    *OutStatus = Stat;
    *OutIrq    = Irq;

    if (!Completed) {
        return STATUS_IO_TIMEOUT;
    }
    if (Irq & ErrMask) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "MaliKmd: job slot %u failed, JS_STATUS=0x%02X\n", Slot, Stat);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    return STATUS_SUCCESS;
}

NTSTATUS MaliJobStatus(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Slot, _Out_ PULONG Status,
                       _Out_ PULONG Config, _Out_ PULONG Present, _Out_ PULONG64 Head,
                       _Out_ PULONG64 Tail, _Out_ PULONG Features)
{
    ULONG Base;
    ULONG PresentMask;

    if (Ctx->PowerState != MALI_POWER_ON) {
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->Job.SlotCount = MaliJobSlotCount(Ctx);
    if (!MaliJobSlotValid(Ctx, Slot)) {
        return STATUS_INVALID_PARAMETER;
    }

    PresentMask = MaliJobReadReg(Ctx, MALI_JS_PRESENT);
    Base = MALI_JOB_SLOT_BASE + Slot * MALI_JOB_SLOT_STRIDE;

    *Present  = PresentMask;
    *Status   = MaliJobReadReg(Ctx, Base + MALI_JS_STATUS);
    *Config   = MaliJobReadReg(Ctx, Base + MALI_JS_CONFIG);
    *Features = MaliJobReadReg(Ctx, MALI_JS_FEATURES(Slot));
    *Head = (UINT64)MaliJobReadReg(Ctx, Base + MALI_JS_HEAD_LO)
          | ((UINT64)MaliJobReadReg(Ctx, Base + MALI_JS_HEAD_HI) << 32);
    *Tail = (UINT64)MaliJobReadReg(Ctx, Base + MALI_JS_TAIL_LO)
          | ((UINT64)MaliJobReadReg(Ctx, Base + MALI_JS_TAIL_HI) << 32);
    return STATUS_SUCCESS;
}

NTSTATUS MaliJobStop(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Slot, _In_ BOOLEAN Hard)
{
    ULONG Base;

    if (Ctx->PowerState != MALI_POWER_ON) {
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->Job.SlotCount = MaliJobSlotCount(Ctx);
    if (!MaliJobSlotValid(Ctx, Slot)) {
        return STATUS_INVALID_PARAMETER;
    }

    Base = MALI_JOB_SLOT_BASE + Slot * MALI_JOB_SLOT_STRIDE;
    MaliJobWriteReg(Ctx, Base + MALI_JS_COMMAND,
                    Hard ? MALI_JS_COMMAND_HARD_STOP : MALI_JS_COMMAND_SOFT_STOP);
    return STATUS_SUCCESS;
}

NTSTATUS MaliJobNullTest(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Slot,
                         _Out_ PULONG JsStatus, _Out_ PULONG IrqStat)
{
    PMALI_BO Bo = NULL;
    NTSTATUS Status;
    ULONG Stat = 0, Irq = 0;

    Status = MaliMemAlloc(Ctx, 4096, MALI_BO_FLAG_CPU_WRITE | MALI_BO_FLAG_GPU_READ, &Bo);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* MaliMemAlloc zeroed the buffer, so the chain header is all zeros and
     * terminates immediately. No JOB_CHAIN flag: this is the first chain on
     * the slot, which is exactly the "flag == 0" case, so a plain
     * JS_COMMAND_SOFT_STOP later targets it unambiguously. */
    Status = MaliJobSubmit(Ctx, Slot, Bo->PhysBase, 0, MALI_MMU_ASN,
                           0, MALI_JOB_DEFAULT_TIMEOUT_MS,
                           &Stat, &Irq);

    *JsStatus = Stat;
    *IrqStat  = Irq;

    (VOID)MaliMemFree(Ctx, Bo->Handle);
    return Status;
}
