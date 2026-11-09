/*
 * mali_mmu.h  -  Mali GPU MMU (AArch64 4K mode) for the Windows KMD.
 *
 * Identity mapping for bring-up: GPU VA == physical address. BOs are mapped
 * page by page into the GPU page tables as they are allocated.
 *
 * Format taken from the vendor kbase r32p1 tree:
 *   mmu/mali_kbase_mmu_mode_aarch64.c
 *   mmu/mali_kbase_mmu_hw_direct.c
 *   include/uapi/.../gpu/mali_kbase_gpu_regmap.h
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <ntddk.h>

#define MALI_MMU_ASN         0
#define MALI_PAGE_SHIFT      12
#define MALI_PAGE_SIZE       (1ull << MALI_PAGE_SHIFT)
#define MALI_PT_ENTRIES      512
#define MALI_VA_LIMIT        (1ull << 40)   /* MMU_HW_OUTA_BITS = 40 */

/*
 * Bring-up uses the MMU's IDENTITY address mode: BOs are identity mapped
 * (GPU VA == PA), so a translation table buys us nothing today. With
 * AARCH64_4K tables the GPU raised AS_FAULTSTATUS=0x7C0002C0 (translation
 * fault, read) on the very first descriptor fetch even though TRANSTAB,
 * MEMATTR and TRANSCFG read back correctly, while IDENTITY completes the
 * same job in 0 ms. Flip this to 1 once the table path is fixed; per-page
 * cache attributes will be needed for peak throughput.
 *
 * 2026-11-09: the table path is implemented (L0 allocation + PtPages tracking
 * below, L3 fill in MaliMmu.c) but still unvalidated. With the flag at 1 the
 * MMU_ENABLE IOCTL read back UsePageTables=0 and TRANSCFG latched IDENTITY
 * (0x62000002), so it is kept but disabled. main stays on the validated
 * IDENTITY mode until the table path passes on silicon.
 *
 * 2026-11-09 (later): page-table pages are now allocated MmNonCached first
 * (cached only as a fallback). The GPU's table walker is not guaranteed to
 * snoop CPU caches, and AS_MEMATTR index 5 — the policy the PTEs carry — is
 * NON_CACHEABLE (0x4C) per the vendor regmap, not write-back. A cached CPU
 * mapping could therefore leave freshly written PTEs in the cache, which is
 * the most plausible cause of the translation fault at the first descriptor
 * fetch. This change cannot regress IDENTITY mode; it only affects the
 * disabled table path.
 */
#define MALI_MMU_USE_PAGE_TABLES 0

#define MALI_PT_TRACK_MAX     1024   /* enough for bring-up; page tables are few */

typedef struct _MALI_PT_PAGE {
    UINT64 Phys;
    PVOID  Va;          /* non-cached VA from MmAllocateContiguousMemorySpecifyCache */
} MALI_PT_PAGE, *PMALI_PT_PAGE;

typedef struct _MALI_MMU {
    PVOID           L0Table;      /* kernel VA of the top-level table (non-cached) */
    UINT64          L0Phys;
    ULONG           AsNumber;
    BOOLEAN         Enabled;
    BOOLEAN         UsePageTables;
    KSPIN_LOCK      TblLock;      /* guards page-table creation + L3 entries */
    ULONG           PtPageCount;
    MALI_PT_PAGE    PtPages[MALI_PT_TRACK_MAX];
} MALI_MMU, *PMALI_MMU;

struct _MALI_KMD_DEVICE;

NTSTATUS MaliMmuInitialize(struct _MALI_KMD_DEVICE *Ctx);
VOID     MaliMmuCleanup(struct _MALI_KMD_DEVICE *Ctx);

NTSTATUS MaliMmuEnable(struct _MALI_KMD_DEVICE *Ctx);
NTSTATUS MaliMmuDisable(struct _MALI_KMD_DEVICE *Ctx);

/* Identity-map [Pa, Pa+Size) so the GPU can reach it at VA == Pa. */
NTSTATUS MaliMmuMap(struct _MALI_KMD_DEVICE *Ctx, UINT64 Pa, UINT64 Size);
NTSTATUS MaliMmuUnmap(struct _MALI_KMD_DEVICE *Ctx, UINT64 Va, UINT64 Size);

NTSTATUS MaliMmuGetFault(struct _MALI_KMD_DEVICE *Ctx, PULONG Status, PULONG64 Address);