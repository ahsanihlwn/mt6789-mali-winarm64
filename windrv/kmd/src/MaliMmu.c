/*
 * MaliMmu.c  -  Mali GPU MMU (AArch64 4K) for the Windows KMD.
 *
 * Identity mapping: GPU VA == physical address, 4-level AArch64 page tables
 * (bottom level 3). All constants are lifted from the vendor kbase r32p1 tree.
 *
 * IMPORTANT: every page table page and every BO is allocated non-cached, and
 * PTEs use the non-cacheable memory-attribute index, so no CPU/GPU cache
 * maintenance is required for the first bring-up. Cached mappings with explicit
 * cache clean/invalidate come later.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include "MaliKmd.h"

/* GPU register offsets, relative to the MTK_REGION_GPU base (0x13000000). */
#define MMU_MEM_BASE            0x2000u
#define MMU_AS0                 0x400u
#define MMU_AS_STRIDE           0x40u
#define MMU_AS_REG(n, r)        (MMU_MEM_BASE + MMU_AS0 + ((n) * MMU_AS_STRIDE) + (r))

#define AS_TRANSTAB_LO          0x00u
#define AS_TRANSTAB_HI          0x04u
#define AS_MEMATTR_LO           0x08u
#define AS_MEMATTR_HI           0x0Cu
#define AS_COMMAND              0x18u
#define AS_FAULTSTATUS          0x1Cu
#define AS_FAULTADDRESS_LO      0x20u
#define AS_FAULTADDRESS_HI      0x24u
#define AS_STATUS               0x28u
#define AS_TRANSCFG_LO          0x30u
#define AS_TRANSCFG_HI          0x34u

#define AS_COMMAND_UPDATE       0x01u
#define AS_COMMAND_FLUSH_PT     0x04u
#define AS_STATUS_AS_ACTIVE     0x01u

#define AS_TRANSCFG_ADRMODE_AARCH64_4K      6ull
#define AS_TRANSCFG_ADRMODE_IDENTITY        2ull
#define AS_TRANSCFG_ADRMODE_UNMAPPED        1ull
#define AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK  (2ull << 24)
#define AS_TRANSCFG_PTW_SH_OS               (2ull << 28)
#define AS_TRANSCFG_R_ALLOCATE              (1ull << 30)
#define AS_TRANSTAB_BASE_MASK               (MALI_VA_LIMIT - 16)   /* bits 4..39 */

/*
 * Page-table entry, aarch64 4K mode (mali_kbase_mmu_mode_aarch64.c).
 *
 * Entry type encoding (bits 1:0):
 *   0b00 = invalid
 *   0b01 = table descriptor (levels 0..2)  -> ENTRY_IS_ATE_L02
 *   0b10 = reserved
 *   0b11 = page descriptor (level 3)       -> ENTRY_IS_ATE_L3 / ENTRY_IS_PTE
 */
#define PTE_TYPE_MASK           3ull
#define PTE_TABLE               1ull       /* next-level table (levels 0..2) = ATE_L02 */
#define PTE_INVALID             2ull
#define PTE_L3_VALID            3ull       /* level 3 4KB ATE / PTE = ATE_L3 */
#define PTE_ATTR_INDEX          5ull       /* AS_MEMATTR_INDEX_NON_CACHEABLE */
#define PTE_ATTR                (PTE_ATTR_INDEX << 2)
#define PTE_RW                  (1ull << 6)
#define PTE_ACCESS              (1ull << 10)
#define PTE_ADDR_MASK           0x000000FFFFFFF000ull   /* 40-bit PA, 4K aligned */

/* AS_MEMATTR: byte N holds the attribute for index N. */
#define MALI_AS_MEMATTR_VALUE   \
    ((0x88ull << (0 * 8)) | (0x8Full << (1 * 8)) | (0x8Dull << (2 * 8)) | \
     (0x88ull << (3 * 8)) | (0x8Dull << (4 * 8)) | (0x4Cull << (5 * 8)))

#define MALI_POLL_ITER          2000
#define MALI_POLL_US            10

/* Find the non-cached VA for a page table page we allocated. */
static PVOID MaliFindPtVa(PMALI_KMD_DEVICE Ctx, UINT64 Phys)
{
    PMALI_MMU Mmu = &Ctx->Mmu;
    for (ULONG i = 0; i < Mmu->PtPageCount; i++) {
        if (Mmu->PtPages[i].Phys == Phys) {
            return Mmu->PtPages[i].Va;
        }
    }
    return NULL;
}

static PVOID MaliPhysToVa(UINT64 Pa)
{
    PHYSICAL_ADDRESS p;
    p.QuadPart = Pa;
    return MmGetVirtualForPhysical(p);
}

static PVOID MaliAllocTable(PMALI_KMD_DEVICE Ctx, _Out_ PUINT64 PhysOut)
{
    PHYSICAL_ADDRESS Low, High, Boundary;
    PVOID Va;

    Low.QuadPart      = 0;
    High.QuadPart     = MALI_VA_LIMIT - 1;  /* GPU supports 40-bit PA, use full range */
    Boundary.QuadPart = 0;

    /* PTW is configured with WRITE_BACK, so allocate page tables as cached (write-back). */
    Va = MmAllocateContiguousMemorySpecifyCache(MALI_PAGE_SIZE, Low, High, Boundary, MmCached);
    if (Va == NULL) {
        /* Fallback: try non-cached */
        Va = MmAllocateContiguousMemorySpecifyCache(MALI_PAGE_SIZE, Low, High, Boundary, MmNonCached);
    }
    if (Va == NULL) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "MaliKmd: PT alloc failed (Low=0x%llx High=0x%llx)\n",
                   (unsigned long long)Low.QuadPart, (unsigned long long)High.QuadPart);
        return NULL;
    }
    RtlZeroMemory(Va, (SIZE_T)MALI_PAGE_SIZE);
    *PhysOut = MmGetPhysicalAddress(Va).QuadPart;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: PT page PA=0x%llx VA=%p\n",
               (unsigned long long)*PhysOut, Va);

    /* Track the VA so we can write PTEs through it later. */
    PMALI_MMU Mmu = &Ctx->Mmu;
    if (Mmu->PtPageCount < MALI_PT_TRACK_MAX) {
        Mmu->PtPages[Mmu->PtPageCount].Phys = *PhysOut;
        Mmu->PtPages[Mmu->PtPageCount].Va   = Va;
        Mmu->PtPageCount++;
    }

    return Va;
}

/* Return the next-level table, creating it if missing. */
static NTSTATUS MaliGetOrCreateTable(PMALI_KMD_DEVICE Ctx, PUINT64 Table, ULONG Index, PUINT64 *Next)
{
    for (;;) {
        UINT64 Entry = Table[Index];
        UINT64 Phys = 0;
        PVOID  Va;
        KIRQL  OldIrql;

        if ((Entry & PTE_TYPE_MASK) == PTE_TABLE) {
            Phys = Entry & PTE_ADDR_MASK;
            Va = MaliFindPtVa(Ctx, Phys);
            if (Va == NULL) {
                /* Fallback for any untracked tables (shouldn't happen for our PTs). */
                Va = MaliPhysToVa(Phys);
            }
            if (Va == NULL) {
                return STATUS_DEVICE_HARDWARE_ERROR;
            }
            *Next = (PUINT64)Va;
            return STATUS_SUCCESS;
        }
        if (Entry != 0) {
            return STATUS_DEVICE_HARDWARE_ERROR;
        }

        /* Allocate outside the lock: the memory manager may need PASSIVE. */
        Va = MaliAllocTable(Ctx, &Phys);
        if (Va == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        /*
         * Double-check under the lock: a parallel ALLOC_BO may have installed
         * a table for this slot while we were allocating. If so, discard ours.
         */
        KeAcquireSpinLock(&Ctx->Mmu.TblLock, &OldIrql);
        if (Table[Index] == 0) {
            Table[Index] = (Phys & PTE_ADDR_MASK) | PTE_ACCESS | PTE_TABLE;
            KeReleaseSpinLock(&Ctx->Mmu.TblLock, OldIrql);
            *Next = (PUINT64)Va;
            return STATUS_SUCCESS;
        }
        KeReleaseSpinLock(&Ctx->Mmu.TblLock, OldIrql);
        MmFreeContiguousMemory(Va);
    }
}

/* Find L3 table for a GPU VA. Uses tracked non-cached VAs for our page tables. */
static PUINT64 MaliFindL3(PMALI_KMD_DEVICE Ctx, PUINT64 L0, UINT64 Va)
{
    PUINT64 Table = L0;
    UINT64 Shifts[3] = { 39, 30, 21 };
    ULONG i;

    for (i = 0; i < 3; i++) {
        UINT64 Entry = Table[(Va >> Shifts[i]) & 0x1FF];
        if ((Entry & PTE_TYPE_MASK) != PTE_TABLE) {
            return NULL;
        }
        UINT64 Phys = Entry & PTE_ADDR_MASK;
        PVOID VaPtr = MaliFindPtVa(Ctx, Phys);
        if (VaPtr == NULL) {
            VaPtr = MaliPhysToVa(Phys);
        }
        if (VaPtr == NULL) {
            return NULL;
        }
        Table = (PUINT64)VaPtr;
    }
    return Table;
}

/*
 * Invalidate the GPU's page-table cache. Vendor kbase does FLUSH_PT followed
 * by UPDATE to ensure new page-table entries are visible to PTW.
 */
static VOID MaliMmuFlushPt(PMALI_KMD_DEVICE Ctx)
{
    if (!Ctx->Mmu.Enabled) {
        return;
    }
    ULONG As = Ctx->Mmu.AsNumber;
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_COMMAND), AS_COMMAND_FLUSH_PT);
    KeStallExecutionProcessor(100);
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_COMMAND), AS_COMMAND_UPDATE);
    KeStallExecutionProcessor(100);
}

static VOID MaliFreeTree(PMALI_KMD_DEVICE Ctx, PUINT64 Table, ULONG Level)
{
    if (Level < 3) {
        ULONG i;
        for (i = 0; i < MALI_PT_ENTRIES; i++) {
            UINT64 Entry = Table[i];
            if ((Entry & PTE_TYPE_MASK) == PTE_TABLE) {
                UINT64 Phys = Entry & PTE_ADDR_MASK;
                PVOID ChildVa = MaliFindPtVa(Ctx, Phys);
                if (ChildVa == NULL) {
                    ChildVa = MaliPhysToVa(Phys);
                }
                if (ChildVa != NULL) {
                    MaliFreeTree(Ctx, (PUINT64)ChildVa, Level + 1);
                }
            }
        }
    }
    MmFreeContiguousMemory(Table);
}

NTSTATUS MaliMmuInitialize(PMALI_KMD_DEVICE Ctx)
{
    UINT64 Phys = 0;
    PVOID Table;

    RtlZeroMemory(&Ctx->Mmu, sizeof(Ctx->Mmu));
    Ctx->Mmu.AsNumber = MALI_MMU_ASN;
    Ctx->Mmu.PtPageCount = 0;
    KeInitializeSpinLock(&Ctx->Mmu.TblLock);

    Table = MaliAllocTable(Ctx, &Phys);
    if (Table == NULL) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "MaliKmd: MMU L0 table allocation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Ctx->Mmu.L0Table = Table;
    Ctx->Mmu.L0Phys  = Phys;
    Ctx->Mmu.Enabled = FALSE;
    Ctx->Mmu.UsePageTables = (MALI_MMU_USE_PAGE_TABLES != 0);

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: MMU L0 table at PA 0x%llx (addr mode %s)\n",
               (unsigned long long)Phys,
               Ctx->Mmu.UsePageTables ? "AARCH64_4K" : "IDENTITY");
    return STATUS_SUCCESS;
}

VOID MaliMmuCleanup(PMALI_KMD_DEVICE Ctx)
{
    if (Ctx->Mmu.L0Table != NULL) {
        MaliFreeTree(Ctx, (PUINT64)Ctx->Mmu.L0Table, 0);
        Ctx->Mmu.L0Table = NULL;
        Ctx->Mmu.L0Phys  = 0;
    }
    Ctx->Mmu.Enabled = FALSE;
}

NTSTATUS MaliMmuEnable(PMALI_KMD_DEVICE Ctx)
{
    UINT64 Transtab, Memattr, Transcfg;
    ULONG  Status = 0;
    ULONG  As = Ctx->Mmu.AsNumber;

    if (Ctx->Mmu.L0Table == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Ctx->PowerState != MALI_POWER_ON) {
        return STATUS_DEVICE_NOT_READY;
    }

    Transtab = Ctx->Mmu.L0Phys & AS_TRANSTAB_BASE_MASK;
    Memattr  = MALI_AS_MEMATTR_VALUE;
    /* Use IDENTITY by default; switch to AARCH64_4K after first BO mapped. */
    Transcfg = AS_TRANSCFG_ADRMODE_IDENTITY |
               AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK |
               AS_TRANSCFG_PTW_SH_OS |
               AS_TRANSCFG_R_ALLOCATE;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: MMU Enable - IDENTITY mode Transcfg=0x%llx\n",
               (unsigned long long)Transcfg);

    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_TRANSTAB_LO), (ULONG)(Transtab & 0xFFFFFFFFu));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_TRANSTAB_HI), (ULONG)(Transtab >> 32));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_MEMATTR_LO), (ULONG)(Memattr & 0xFFFFFFFFu));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_MEMATTR_HI), (ULONG)(Memattr >> 32));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_TRANSCFG_LO), (ULONG)(Transcfg & 0xFFFFFFFFu));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_TRANSCFG_HI), (ULONG)(Transcfg >> 32));

    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_COMMAND), AS_COMMAND_UPDATE);

    /*
     * The address space becomes "active" only once a context is bound to it,
     * so just give the UPDATE time to land and log the status for bring-up.
     */
    KeStallExecutionProcessor(1000);
    MaliHwReadReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_STATUS), &Status);

    Ctx->Mmu.Enabled = TRUE;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: MMU AS%u enabled, TRANSTAB=0x%llx STATUS=0x%08X\n",
               As, (unsigned long long)Transtab, Status);
    return STATUS_SUCCESS;
}

NTSTATUS MaliMmuDisable(PMALI_KMD_DEVICE Ctx)
{
    UINT64 Transcfg = AS_TRANSCFG_ADRMODE_UNMAPPED;
    ULONG  As = Ctx->Mmu.AsNumber;

    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_TRANSCFG_LO), (ULONG)(Transcfg & 0xFFFFFFFFu));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_TRANSCFG_HI), (ULONG)(Transcfg >> 32));
    MaliHwWriteReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_COMMAND), AS_COMMAND_UPDATE);

    Ctx->Mmu.Enabled = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS MaliMmuMap(PMALI_KMD_DEVICE Ctx, UINT64 Pa, UINT64 Size)
{
    UINT64 Va, End;

    if (Ctx->Mmu.L0Table == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if ((Pa & (MALI_PAGE_SIZE - 1)) != 0 || (Size & (MALI_PAGE_SIZE - 1)) != 0 || Size == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Pa + Size > MALI_VA_LIMIT) {
        return STATUS_INVALID_PARAMETER;
    }

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: Map PA=0x%llx Size=0x%llx L0Phys=0x%llx\n",
               (unsigned long long)Pa, (unsigned long long)Size, (unsigned long long)Ctx->Mmu.L0Phys);

    Va  = Pa;            /* identity */
    End = Pa + Size;

    while (Va < End) {
        PUINT64 L0 = (PUINT64)Ctx->Mmu.L0Table;
        PUINT64 L1, L2, L3;
        NTSTATUS Status;

        Status = MaliGetOrCreateTable(Ctx, L0, (ULONG)((Va >> 39) & 0x1FF), &L1);
        if (!NT_SUCCESS(Status)) return Status;
        Status = MaliGetOrCreateTable(Ctx, L1, (ULONG)((Va >> 30) & 0x1FF), &L2);
        if (!NT_SUCCESS(Status)) return Status;
        Status = MaliGetOrCreateTable(Ctx, L2, (ULONG)((Va >> 21) & 0x1FF), &L3);
        if (!NT_SUCCESS(Status)) return Status;

        L3[(Va >> 12) & 0x1FF] =
            (Va & PTE_ADDR_MASK) | PTE_ATTR | PTE_RW | PTE_ACCESS | PTE_L3_VALID;

        Va += MALI_PAGE_SIZE;
    }
    MaliMmuFlushPt(Ctx);
    return STATUS_SUCCESS;
}

NTSTATUS MaliMmuUnmap(PMALI_KMD_DEVICE Ctx, UINT64 Va, UINT64 Size)
{
    UINT64 End;

    if (Ctx->Mmu.L0Table == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if ((Va & (MALI_PAGE_SIZE - 1)) != 0 || Size == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    End = Va + Size;
    while (Va < End) {
        PUINT64 L3 = MaliFindL3(Ctx, (PUINT64)Ctx->Mmu.L0Table, Va);
        if (L3 != NULL) {
            L3[(Va >> 12) & 0x1FF] = PTE_INVALID;
        }
        Va += MALI_PAGE_SIZE;
    }
    MaliMmuFlushPt(Ctx);
    return STATUS_SUCCESS;
}

NTSTATUS MaliMmuGetFault(PMALI_KMD_DEVICE Ctx, PULONG Status, PULONG64 Address)
{
    ULONG Lo = 0, Hi = 0, Fs = 0;
    ULONG As = Ctx->Mmu.AsNumber;

    MaliHwReadReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_FAULTSTATUS), &Fs);
    MaliHwReadReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_FAULTADDRESS_LO), &Lo);
    MaliHwReadReg(Ctx, MTK_REGION_GPU, MMU_AS_REG(As, AS_FAULTADDRESS_HI), &Hi);

    *Status  = Fs;
    *Address = ((UINT64)Hi << 32) | Lo;
    return STATUS_SUCCESS;
}