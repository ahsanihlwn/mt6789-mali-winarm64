/*
 * mali_kmd_ioctl.h  -  Shared IOCTL ABI for the MediaTek Mali-G57 Windows KMD.
 *
 * Used by both the kernel driver (KMDF) and the future user-mode Vulkan ICD.
 * Keep this file self-contained: no kernel-only types, fixed-width ints only.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_KMD_IOCTL_H
#define MALI_KMD_IOCTL_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#pragma pack(push, 8)

/* {7c1f9a20-6d2b-4f4a-9b0e-1a2b3c4d5e6f} */
#define MALI_KMD_DEVICE_INTERFACE_GUID \
    { 0x7c1f9a20, 0x6d2b, 0x4f4a, \
      { 0x9b, 0x0e, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f } }

#define MALI_KMD_DEVICE_TYPE    0x8C00   /* FILE_DEVICE_UNKNOWN range we own */

#define MALI_KMD_ABI_VERSION    2u

/* ------------------------------------------------------------------ */
/* Caps / identification                                               */
/* ------------------------------------------------------------------ */
#define MALI_SOC_MT6789   0x6789u
#define MALI_SOC_MT6833   0x6833u
#define MALI_SOC_MT6853   0x6853u
#define MALI_SOC_MT6879   0x6879u
#define MALI_SOC_MT6895   0x6895u

#define MALI_MODEL_UNKNOWN 0u
#define MALI_MODEL_G57     0x9003u   /* GPU_ID2_PRODUCT_TNAX (Valhall, MT6789) */

typedef struct _MALI_KMD_VERSION {
    UINT32 AbiVersion;
    UINT32 DriverMajor;
    UINT32 DriverMinor;
    WCHAR  BuildTag[32];
} MALI_KMD_VERSION;

typedef struct _MALI_KMD_CAPS {
    UINT32 SocId;            /* MALI_SOC_* */
    UINT32 GpuModel;         /* MALI_MODEL_* */
    UINT32 GpuIdRegister;    /* raw 0x13000000 + 0x00, as read back */
    UINT32 CoreFeatures;
    UINT32 MmuFeatures;
    UINT32 ShaderPresent;    /* stack mask */
    UINT32 MaxFreqKhz;
    UINT32 IrqCount;
    UINT32 Flags;            /* MALI_CAP_* */
} MALI_KMD_CAPS;

#define MALI_CAP_MMIO_MAPPED    0x00000001u
#define MALI_CAP_POWER_ON       0x00000002u
#define MALI_CAP_IRQ_CONNECTED  0x00000004u
#define MALI_CAP_SMMU_READY     0x00000008u
#define MALI_CAP_MEM_ALLOC      0x00000010u   /* BO allocator + page tables ready */
#define MALI_CAP_MMU            0x00000020u   /* GPU MMU programmed and enabled   */
#define MALI_CAP_JOBS           0x00000040u   /* job submission available         */

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */
#define MALI_POWER_OFF  0u
#define MALI_POWER_ON   1u

typedef struct _MALI_KMD_POWER {
    UINT32 State;            /* MALI_POWER_* */
} MALI_KMD_POWER;

typedef struct _MALI_KMD_PWR_STATUS {
    UINT32 RawPwrStatus;     /* SPM PWR_STATUS (sleep base + desc->PwrStatusOfs) */
    UINT32 RawPwrStatus2nd;
    UINT32 MfgMaskExpected;
    UINT32 MfgMaskActual;
    UINT32 Powered;          /* 1 if all MFG domains up */
} MALI_KMD_PWR_STATUS;

/* ------------------------------------------------------------------ */
/* Raw register access (bring-up / diagnostics only)                   */
/* ------------------------------------------------------------------ */
#define MALI_REG_BLOCK_GPU       0u   /* 0x13000000 */
#define MALI_REG_BLOCK_MFG_TOP   1u   /* 0x13FBF000 */
#define MALI_REG_BLOCK_APMIXED   2u   /* 0x1000C000 (MFGPLL_CON*)   */
#define MALI_REG_BLOCK_SLEEP     3u   /* 0x10006000 (SPM/scpsys)    */
#define MALI_REG_BLOCK_TOPCKGEN  4u   /* 0x10000000 (CLK_CFG_*)     */
#define MALI_REG_BLOCK_INFRACFG_AO 5u /* 0x10001000 (AXI bus-protect) */
#define MALI_REG_BLOCK_BCRM      6u   /* 0x10215000 (way_en)        */

typedef struct _MALI_KMD_REG_ACCESS {
    UINT32 Block;            /* MALI_REG_BLOCK_* */
    UINT32 Offset;           /* byte offset into the block */
    UINT32 Value;
} MALI_KMD_REG_ACCESS;

/* ------------------------------------------------------------------ */
/* GPU memory (buffer objects)                                         */
/* ------------------------------------------------------------------ */
#define MALI_BO_CPU_RD  0x01u
#define MALI_BO_CPU_WR  0x02u
#define MALI_BO_GPU_RD  0x04u
#define MALI_BO_GPU_WR  0x08u

typedef struct _MALI_KMD_BO_ALLOC {
    UINT32 Size;             /* input: bytes                                  */
    UINT32 Flags;            /* input: MALI_BO_*                              */
    UINT32 Handle;           /* output: BO handle                             */
    UINT32 Reserved;
    UINT64 GpuVa;            /* output: GPU address (identity == physical)    */
    UINT64 UserVa;           /* output: user-mode mapping in the caller       */
} MALI_KMD_BO_ALLOC;

typedef struct _MALI_KMD_BO_FREE {
    UINT32 Handle;
    UINT32 Reserved;
} MALI_KMD_BO_FREE;

typedef struct _MALI_KMD_BO_INFO {
    UINT32 Handle;
    UINT32 Flags;
    UINT64 Size;
    UINT64 GpuVa;
    UINT64 PhysBase;
    UINT64 UserVa;
} MALI_KMD_BO_INFO;

typedef struct _MALI_KMD_MMU_FAULT {
    UINT32 Status;
    UINT32 Reserved;
    UINT64 Address;
} MALI_KMD_MMU_FAULT;

/* ------------------------------------------------------------------ */
/* Jobs (Job Manager submission)                                       */
/* ------------------------------------------------------------------ */
#define MALI_JOB_FLAG_NO_MMU    0x00000002u   /* omit JS_CONFIG_START_MMU      */
#define MALI_JOB_FLAG_JOB_CHAIN 0x00000004u   /* set JS_CONFIG_JOB_CHAIN_FLAG  */

typedef struct _MALI_KMD_JOB_SUBMIT {
    UINT32 Slot;         /* input                                                 */
    UINT32 Flags;        /* input: MALI_JOB_FLAG_*                                */
    UINT64 Jc;           /* input: GPU VA of the job chain head, 16-byte aligned  */
    UINT64 Affinity;     /* input: shader core mask (0 = driver default)          */
    UINT32 AsNumber;     /* input: address space (0)                              */
    UINT32 TimeoutMs;    /* input: 0 = fire and forget, >0 = wait this long       */
    UINT32 JsStatus;     /* output: JS_STATUS                                     */
    UINT32 IrqStat;      /* output: JOB_IRQ bits observed                         */
} MALI_KMD_JOB_SUBMIT;

typedef struct _MALI_KMD_JOB_STATUS {
    UINT32 Slot;         /* input                                                 */
    UINT32 Present;      /* output: JS_PRESENT bitmask                            */
    UINT32 JsStatus;     /* output                                                */
    UINT32 JsConfig;     /* output                                                */
    UINT32 JsFeatures;   /* output                                                */
    UINT32 SlotCount;    /* output: implemented slot count                        */
    UINT64 JsHead;       /* output                                                */
    UINT64 JsTail;       /* output                                                */
} MALI_KMD_JOB_STATUS;

typedef struct _MALI_KMD_JOB_STOP {
    UINT32 Slot;         /* input */
    UINT32 Hard;         /* input: 1 = hard stop, 0 = soft stop */
} MALI_KMD_JOB_STOP;

typedef struct _MALI_KMD_JOB_SELFTEST {
    UINT32 Slot;         /* input                                                 */
    UINT32 Reserved;
    UINT32 JsStatus;     /* output                                                */
    UINT32 IrqStat;      /* output                                                */
} MALI_KMD_JOB_SELFTEST;

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
#define MALI_IRQ_LINE_COUNT 5   /* GPU, MMU, JOB, EVENT, PWR */

typedef struct _MALI_KMD_IRQ_STATS {
    UINT32 LinesConnected;
    UINT32 Reserved;
    UINT32 LineCount[MALI_IRQ_LINE_COUNT];  /* GPU, MMU, JOB, EVENT, PWR */
    UINT32 GpuFaultCount;
    UINT32 MmuFaultCount;
    UINT32 JobDoneCount;
    UINT32 JobErrorCount;
    UINT32 LastGpuFaultStatus;
    UINT32 LastMmuFaultStatus;
    UINT64 LastGpuFaultAddress;
    UINT64 LastMmuFaultAddress;
} MALI_KMD_IRQ_STATS;

/* ------------------------------------------------------------------ */
/* Page-table inspection (bring-up diagnostics)                        */
/* ------------------------------------------------------------------ */
#define MALI_PT_DUMP_MAX 256

/*
 * Read raw 64-bit entries out of a live page table so the table hierarchy
 * (L0..L3) that MaliMmuMap built can be compared against what the GPU's
 * walker actually fetches. TablePa == 0 selects the L0 (root) table; any
 * other value selects a tracked page-table page by its physical address
 * (as returned in a previous dump's Entries[]).
 */
typedef struct _MALI_KMD_PT_DUMP {
    UINT64 TablePa;                    /* input: 0 = L0 (root) table         */
    UINT32 Index;                      /* input: first entry to return       */
    UINT32 Count;                      /* input: number of entries (<= MAX)  */
    UINT64 Entries[MALI_PT_DUMP_MAX];  /* output: raw 64-bit PTEs            */
} MALI_KMD_PT_DUMP;

/* ------------------------------------------------------------------ */
/* IOCTL codes                                                         */
/* ------------------------------------------------------------------ */
#define MALI_IOCTL_BASE 0x800

#define IOCTL_MALI_GET_VERSION \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_GET_CAPS \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_POWER \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_GET_PWR_STATUS \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_REG_READ \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_REG_WRITE \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 5, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_ALLOC_BO \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_FREE_BO \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 7, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_BO_INFO \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_MMU_ENABLE \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 9, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_MMU_DISABLE \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 10, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_MMU_FAULT \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 11, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_JOB_SUBMIT \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 12, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_JOB_STATUS \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 13, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_JOB_STOP \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 14, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_JOB_SELFTEST \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 15, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_IRQ_STATS \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 16, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_MALI_PT_DUMP \
    CTL_CODE(MALI_KMD_DEVICE_TYPE, MALI_IOCTL_BASE + 17, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(pop)

#endif /* MALI_KMD_IOCTL_H */
