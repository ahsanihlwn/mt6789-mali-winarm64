/*
 * MaliHw.c  -  MMIO mapping, SPM/MTCMOS power sequencing, clock mux.
 *
 * This is the SoC-generic half: every register offset and bit comes from the
 * descriptor in MaliSoc.c. Nothing here names MT6789 directly.
 *
 * The SPM MTCMOS bit layout is the one used by mtk-scpsys.c and verified
 * against mtk-scpsys-mt6789.c:
 *   PWR_RST_B bit0, PWR_ISO bit1, PWR_ON bit2, PWR_ON_2ND bit3,
 *   PWR_CLK_DIS bit4, SRAM_PDN bit8, SRAM_PDN_ACK bit12.
 * Power status is read from PWR_STATUS (0x16C) and PWR_STATUS_2ND (0x170);
 * a domain counts as on only when both report its status bit.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include "MaliKmd.h"

#define MALI_POLL_ITERATIONS  1000
#define MALI_POLL_DELAY_US    10

#define MALI_GPU_CMD_SOFT_RESET     0x01u

static ULONG MaliReadRegion(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Region, _In_ ULONG Offset)
{
    if (Region >= MTK_REGION_COUNT || Ctx->Base[Region] == NULL || Offset + 4 > Ctx->MapSize[Region]) {
        return 0xFFFFFFFFu;
    }
    return READ_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Base[Region] + Offset));
}

static VOID MaliWriteRegion(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Region, _In_ ULONG Offset, _In_ ULONG Value)
{
    if (Region >= MTK_REGION_COUNT || Ctx->Base[Region] == NULL || Offset + 4 > Ctx->MapSize[Region]) {
        return;
    }
    WRITE_REGISTER_ULONG((PULONG)((PUCHAR)Ctx->Base[Region] + Offset), Value);
}

/*
 * kbase issues a GPU soft reset on every power-up (kbase_pm_do_reset) before
 * bringing the core stacks up: it clears leftover job-slot state from a
 * previous owner (the bootloader leaves the GPU running for the splash
 * screen). The reset also clears PWRON, so MaliPowerCores() must run *after*
 * it - otherwise the first submit fails with JS_STATUS 0x41 (POWER_FAULT).
 */
static NTSTATUS MaliGpuSoftReset(_In_ PMALI_KMD_DEVICE Ctx)
{
    ULONG i;

    MaliWriteRegion(Ctx, MTK_REGION_GPU, MALI_GPU_IRQ_CLEAR, MALI_RESET_COMPLETED);
    MaliWriteRegion(Ctx, MTK_REGION_GPU, MALI_GPU_COMMAND, MALI_GPU_CMD_SOFT_RESET);

    for (i = 0; i < MALI_POLL_ITERATIONS; i++) {
        if (MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_GPU_IRQ_RAWSTAT) &
            MALI_RESET_COMPLETED) {
            MaliWriteRegion(Ctx, MTK_REGION_GPU, MALI_GPU_IRQ_CLEAR,
                            MALI_RESET_COMPLETED);
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(MALI_POLL_DELAY_US);
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "MaliKmd[%s]: GPU soft reset did not complete\n", Ctx->Soc->Name);
    return STATUS_IO_TIMEOUT;
}

NTSTATUS MaliHwInitialize(_In_ PMALI_KMD_DEVICE Ctx)
{
    UINT32 i;
    NTSTATUS Status = STATUS_SUCCESS;

    KeInitializeSpinLock(&Ctx->RegLock);

    /*
     * The GPU block is not ACPI-described yet, so map straight from the SoC
     * descriptor. Once ACPI publishes the device (see ROADMAP Phase 1) this
     * should prefer the translated resources from EvtDevicePrepareHardware.
     */
    for (i = 0; i < MTK_REGION_COUNT; i++) {
        PHYSICAL_ADDRESS Pa;
        if (Ctx->Soc->Regions[i].Size == 0) {
            continue;
        }
        Ctx->MapSize[i] = Ctx->Soc->Regions[i].Size;
        Pa.QuadPart = Ctx->Soc->Regions[i].Base;
        Ctx->Base[i] = MmMapIoSpaceEx(Pa, Ctx->MapSize[i], PAGE_READWRITE | PAGE_NOCACHE);
        if (Ctx->Base[i] == NULL) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "MaliKmd: MmMapIoSpaceEx failed for region %u (0x%llx)\n",
                       i, (unsigned long long)Ctx->Soc->Regions[i].Base);
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Fail;
        }
    }

    /*
     * Read-only identification, but only when the MFG domain already reports
     * powered (the bootloader often leaves the GPU on for the logo). Reading
     * the Mali register block while its MTCMOS domain is off can raise a bus
     * error, so when it is off we record 0 and wait for an explicit --on.
     */
    Ctx->GpuIdRegister = 0;
    Ctx->CoreFeatures  = 0;
    Ctx->MmuFeatures   = 0;
    Ctx->ShaderPresent = 0;
    Ctx->PowerState    = MALI_POWER_OFF;

    {
        const MTK_GPU_SOC_DESC *Soc = Ctx->Soc;
        ULONG Mask = Soc->MfgPwrMask;
        ULONG S1 = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Soc->PwrStatusOfs);
        ULONG S2 = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Soc->PwrStatus2ndOfs);

        if (Mask != 0 && (S1 & Mask) == Mask && (S2 & Mask) == Mask) {
            Ctx->GpuIdRegister = MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_GPU_ID);
            Ctx->CoreFeatures  = MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_CORE_FEATURES);
            Ctx->MmuFeatures   = MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_MMU_FEATURES);
            Ctx->ShaderPresent = MaliReadRegion(Ctx, MTK_REGION_GPU, Soc->ShaderPresentOfs);
        } else {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "MaliKmd[%s]: MFG powered off at load (PWR_STATUS=0x%08X, mask=0x%08X); "
                       "GPU registers not touched\n", Soc->Name, S1, Mask);
        }
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd[%s]: GPU_ID=0x%08X CORE=0x%08X MMU=0x%08X SHADER=0x%08X\n",
               Ctx->Soc->Name, Ctx->GpuIdRegister, Ctx->CoreFeatures,
               Ctx->MmuFeatures, Ctx->ShaderPresent);
    return STATUS_SUCCESS;

Fail:
    MaliHwCleanup(Ctx);
    return Status;
}

VOID MaliHwCleanup(_In_ PMALI_KMD_DEVICE Ctx)
{
    UINT32 i;
    for (i = 0; i < MTK_REGION_COUNT; i++) {
        if (Ctx->Base[i] != NULL) {
            MmUnmapIoSpace(Ctx->Base[i], Ctx->MapSize[i]);
            Ctx->Base[i] = NULL;
        }
    }
}

/* Wait until (read & Mask) == Expected, bounded. */
static BOOLEAN MaliPollReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Region, _In_ ULONG Offset,
                           _In_ ULONG Mask, _In_ ULONG Expected)
{
    ULONG i;
    for (i = 0; i < MALI_POLL_ITERATIONS; i++) {
        if ((MaliReadRegion(Ctx, Region, Offset) & Mask) == Expected) {
            return TRUE;
        }
        KeStallExecutionProcessor(MALI_POLL_DELAY_US);
    }
    return FALSE;
}

static VOID MaliRunOps(_In_ PMALI_KMD_DEVICE Ctx, _In_ const MTK_GPU_REG_OP *Ops, _In_ UINT32 Count)
{
    UINT32 i;
    if (Ops == NULL) {
        return;
    }
    for (i = 0; i < Count; i++) {
        ULONG V = MaliReadRegion(Ctx, Ops[i].Region, Ops[i].Offset);
        V |= Ops[i].OrMask;
        V &= Ops[i].AndMask;
        MaliWriteRegion(Ctx, Ops[i].Region, Ops[i].Offset, V);
        if (Ops[i].DelayUs) {
            KeStallExecutionProcessor(Ops[i].DelayUs);
        }
    }
}

static BOOLEAN MaliPollMfgOn(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Mask)
{
    ULONG i;
    const MTK_GPU_SOC_DESC *Soc = Ctx->Soc;
    for (i = 0; i < MALI_POLL_ITERATIONS; i++) {
        ULONG S1 = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Soc->PwrStatusOfs);
        ULONG S2 = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Soc->PwrStatus2ndOfs);
        if ((S1 & Mask) == Mask && (S2 & Mask) == Mask) {
            return TRUE;
        }
        KeStallExecutionProcessor(MALI_POLL_DELAY_US);
    }
    return FALSE;
}

static BOOLEAN MaliPollMfgOff(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Mask)
{
    ULONG i;
    const MTK_GPU_SOC_DESC *Soc = Ctx->Soc;
    for (i = 0; i < MALI_POLL_ITERATIONS; i++) {
        ULONG S1 = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Soc->PwrStatusOfs);
        ULONG S2 = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Soc->PwrStatus2ndOfs);
        if ((S1 & Mask) == 0 && (S2 & Mask) == 0) {
            return TRUE;
        }
        KeStallExecutionProcessor(MALI_POLL_DELAY_US);
    }
    return FALSE;
}

/*
 * One MTCMOS domain, matching the vendor scpsys_power_on/off order exactly.
 * Writing these bits in the wrong order (or with the wrong polarity) is what
 * turns a bring-up into a silent bus hang, so keep this in step with
 * drivers/soc/mediatek/mtk-scpsys.c.
 */
static NTSTATUS MaliPowerDomainOn(_In_ PMALI_KMD_DEVICE Ctx, _In_ const MTK_GPU_PWR_DOMAIN *D)
{
    ULONG Ctl = MaliReadRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs);

    /* 1. Assert power-on (main and 2nd copy). */
    Ctl |= MTK_SPM_PWR_ON_BIT;
    MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl |= MTK_SPM_PWR_ON_2ND_BIT;
    MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);

    /* 2. Wait until both power-status registers report the domain on. */
    if (!MaliPollMfgOn(Ctx, D->StaMask)) {
        return STATUS_IO_TIMEOUT;
    }
    KeStallExecutionProcessor(100);

    /* 3. Release clock-disable, isolation and reset. */
    Ctl &= ~MTK_SPM_PWR_CLK_DIS_BIT; MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl &= ~MTK_SPM_PWR_ISO_BIT;     MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl |= MTK_SPM_PWR_RST_B_BIT;    MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);

    /* 4. Un-gate SRAM and wait for the power-down ACK to clear. */
    Ctl = MaliReadRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs);
    Ctl &= ~D->SramPdnBit;
    MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    if (!MaliPollReg(Ctx, MTK_REGION_SLEEP, D->CtlOfs, D->SramPdnAckBit, 0)) {
        return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS MaliPowerDomainOff(_In_ PMALI_KMD_DEVICE Ctx, _In_ const MTK_GPU_PWR_DOMAIN *D)
{
    ULONG Ctl = MaliReadRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs);

    /* Isolate, hold reset, disable clock, then drop power. */
    Ctl |= MTK_SPM_PWR_ISO_BIT;      MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl &= ~MTK_SPM_PWR_RST_B_BIT;   MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl |= MTK_SPM_PWR_CLK_DIS_BIT;  MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl &= ~MTK_SPM_PWR_ON_BIT;      MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    Ctl &= ~MTK_SPM_PWR_ON_2ND_BIT;  MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);

    if (!MaliPollMfgOff(Ctx, D->StaMask)) {
        return STATUS_IO_TIMEOUT;
    }

    /* Gate SRAM and wait for the ACK to assert. */
    Ctl = MaliReadRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs);
    Ctl |= D->SramPdnBit;
    MaliWriteRegion(Ctx, MTK_REGION_SLEEP, D->CtlOfs, Ctl);
    if (!MaliPollReg(Ctx, MTK_REGION_SLEEP, D->CtlOfs, D->SramPdnAckBit, D->SramPdnAckBit)) {
        return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS MaliPowerDomains(_In_ PMALI_KMD_DEVICE Ctx, _In_ BOOLEAN On)
{
    UINT32 i;
    const MTK_GPU_SOC_DESC *Soc = Ctx->Soc;

    if (Soc->MfgDomainCount == 0) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "MaliKmd[%s]: no SPM domains described, refusing power change\n",
                   Soc->Name);
        return STATUS_NOT_SUPPORTED;
    }

    if (On) {
        /* Parents first: MFG0 -> MFG1 -> MFG2/MFG3. */
        for (i = 0; i < Soc->MfgDomainCount; i++) {
            NTSTATUS S = MaliPowerDomainOn(Ctx, &Soc->MfgDomains[i]);
            if (!NT_SUCCESS(S)) {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "MaliKmd[%s]: MFG%u power-on failed 0x%08X\n",
                           Soc->Name, i, S);
                return S;
            }
        }
    } else {
        /* Children first: MFG3/MFG2 -> MFG1 -> MFG0. */
        for (i = Soc->MfgDomainCount; i-- > 0; ) {
            NTSTATUS S = MaliPowerDomainOff(Ctx, &Soc->MfgDomains[i]);
            if (!NT_SUCCESS(S)) {
                DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                           "MaliKmd[%s]: MFG%u power-off failed 0x%08X\n",
                           Soc->Name, i, S);
                return S;
            }
        }
    }
    return STATUS_SUCCESS;
}

/*
 * Power the shader / tiler / L2 core stacks.
 *
 * The *PRESENT bitmaps are only meaningful once the MTCMOS domains are up, so
 * this runs after MaliPowerDomains(). A garbage mask (0 or all-ones) means the
 * block is not really powered: refuse instead of writing nonsense to PWRON.
 *
 * Writing PWRON is a power write, and MaliHwPowerOn is only ever reached from
 * an explicit IOCTL (or from a job submit that the caller asked for).
 */
static NTSTATUS MaliPowerCores(_In_ PMALI_KMD_DEVICE Ctx, _In_ BOOLEAN On)
{
    const MTK_GPU_SOC_DESC *Soc = Ctx->Soc;
    ULONG Shader, Tiler = 0, L2 = 0;

    if (Soc->ShaderPwrOnOfs == 0 || Soc->ShaderPresentOfs == 0) {
        return STATUS_SUCCESS;
    }

    Shader = MaliReadRegion(Ctx, MTK_REGION_GPU, Soc->ShaderPresentOfs);
    if (Shader == 0 || Shader == 0xFFFFFFFFu) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "MaliKmd[%s]: SHADER_PRESENT=0x%08X, skipping core power\n",
                   Soc->Name, Shader);
        return STATUS_SUCCESS;
    }
    if (Soc->TilerPresentOfs) {
        Tiler = MaliReadRegion(Ctx, MTK_REGION_GPU, Soc->TilerPresentOfs);
    }
    if (Soc->L2PresentOfs) {
        L2 = MaliReadRegion(Ctx, MTK_REGION_GPU, Soc->L2PresentOfs);
    }

    if (On) {
        if (Soc->L2PwrOnOfs && L2 != 0 && L2 != 0xFFFFFFFFu) {
            MaliWriteRegion(Ctx, MTK_REGION_GPU, Soc->L2PwrOnOfs, L2);
        }
        if (Soc->TilerPwrOnOfs && Tiler != 0 && Tiler != 0xFFFFFFFFu) {
            MaliWriteRegion(Ctx, MTK_REGION_GPU, Soc->TilerPwrOnOfs, Tiler);
        }
        MaliWriteRegion(Ctx, MTK_REGION_GPU, Soc->ShaderPwrOnOfs, Shader);
        if (Soc->ShaderReadyOfs != 0 &&
            !MaliPollReg(Ctx, MTK_REGION_GPU, Soc->ShaderReadyOfs, Shader, Shader)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "MaliKmd[%s]: shader cores not ready (mask 0x%08X)\n",
                       Soc->Name, Shader);
            return STATUS_IO_TIMEOUT;
        }
    } else if (Soc->ShaderPwrOffOfs != 0) {
        MaliWriteRegion(Ctx, MTK_REGION_GPU, Soc->ShaderPwrOffOfs, Shader);
    }
    return STATUS_SUCCESS;
}

NTSTATUS MaliHwPowerOn(_In_ PMALI_KMD_DEVICE Ctx)
{
    NTSTATUS Status;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Ctx->RegLock, &OldIrql);
    if (Ctx->PowerState == MALI_POWER_ON) {
        KeReleaseSpinLock(&Ctx->RegLock, OldIrql);
        return STATUS_SUCCESS;
    }

    /* Clock mux, only when the platform supplies a real register (0 = skip). */
    if (Ctx->Soc->ClkMuxOfs != 0) {
        ULONG Mux = MaliReadRegion(Ctx, MTK_REGION_MFG_TOP, Ctx->Soc->ClkMuxOfs);
        Mux &= ~0xFu;
        Mux |= Ctx->Soc->ClkMuxRefCore;
        MaliWriteRegion(Ctx, MTK_REGION_MFG_TOP, Ctx->Soc->ClkMuxOfs, Mux);
    }

    MaliRunOps(Ctx, Ctx->Soc->PowerOnOps, Ctx->Soc->PowerOnOpCount);
    Status = MaliPowerDomains(Ctx, TRUE);
    if (NT_SUCCESS(Status)) {
        /* Reset first, then the core stacks: the reset clears PWRON. */
        Status = MaliGpuSoftReset(Ctx);
    }
    if (NT_SUCCESS(Status)) {
        Status = MaliPowerCores(Ctx, TRUE);
    }

    if (NT_SUCCESS(Status)) {
        KeStallExecutionProcessor(1000);
        Ctx->PowerState = MALI_POWER_ON;
        /* Re-read identification now that the GPU is actually powered. */
        Ctx->GpuIdRegister = MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_GPU_ID);
        Ctx->CoreFeatures  = MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_CORE_FEATURES);
        Ctx->MmuFeatures   = MaliReadRegion(Ctx, MTK_REGION_GPU, MALI_MMU_FEATURES);
        Ctx->ShaderPresent = MaliReadRegion(Ctx, MTK_REGION_GPU, Ctx->Soc->ShaderPresentOfs);
    }

    KeReleaseSpinLock(&Ctx->RegLock, OldIrql);
    return Status;
}

NTSTATUS MaliHwPowerOff(_In_ PMALI_KMD_DEVICE Ctx)
{
    NTSTATUS Status;
    KIRQL OldIrql;

    KeAcquireSpinLock(&Ctx->RegLock, &OldIrql);
    if (Ctx->PowerState == MALI_POWER_OFF) {
        KeReleaseSpinLock(&Ctx->RegLock, OldIrql);
        return STATUS_SUCCESS;
    }

    /* Cores down before the domains, mirroring power-on order. */
    (VOID)MaliPowerCores(Ctx, FALSE);
    Status = MaliPowerDomains(Ctx, FALSE);
    MaliRunOps(Ctx, Ctx->Soc->PowerOffOps, Ctx->Soc->PowerOffOpCount);

    if (NT_SUCCESS(Status)) {
        Ctx->PowerState = MALI_POWER_OFF;
    }
    KeReleaseSpinLock(&Ctx->RegLock, OldIrql);
    return Status;
}

ULONG MaliHwReadSubsysReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Block, _In_ ULONG Offset)
{
    return MaliReadRegion(Ctx, Block, Offset);
}

VOID MaliHwGetPwrStatus(_In_ PMALI_KMD_DEVICE Ctx, _Out_ MALI_KMD_PWR_STATUS *Out)
{
    ULONG Mask;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->RawPwrStatus    = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Ctx->Soc->PwrStatusOfs);
    Out->RawPwrStatus2nd = MaliReadRegion(Ctx, MTK_REGION_SLEEP, Ctx->Soc->PwrStatus2ndOfs);
    Out->MfgMaskExpected = Ctx->Soc->MfgPwrMask;

    Mask = Ctx->Soc->MfgPwrMask;
    Out->MfgMaskActual = Out->RawPwrStatus & Mask;
    Out->Powered = (Mask != 0 &&
                    (Out->RawPwrStatus & Mask) == Mask &&
                    (Out->RawPwrStatus2nd & Mask) == Mask) ? 1u : 0u;
}

NTSTATUS MaliHwReadReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Block, _In_ ULONG Offset, _Out_ PULONG Value)
{
    if (Block >= MTK_REGION_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->Base[Block] == NULL || (Offset & 3) || Offset + 4 > Ctx->MapSize[Block]) {
        return STATUS_INVALID_PARAMETER;
    }
    *Value = MaliReadRegion(Ctx, Block, Offset);
    return STATUS_SUCCESS;
}

NTSTATUS MaliHwWriteReg(_In_ PMALI_KMD_DEVICE Ctx, _In_ ULONG Block, _In_ ULONG Offset, _In_ ULONG Value)
{
    if (Block >= MTK_REGION_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Ctx->Base[Block] == NULL || (Offset & 3) || Offset + 4 > Ctx->MapSize[Block]) {
        return STATUS_INVALID_PARAMETER;
    }
    MaliWriteRegion(Ctx, Block, Offset, Value);
    return STATUS_SUCCESS;
}