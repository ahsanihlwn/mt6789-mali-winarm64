/*
 * mtk_gpu_soc.h  -  Per-SoC descriptor for MediaTek Mali GPUs.
 *
 * The whole point: the driver logic is identical for every SoC; only this
 * table changes. MT6789 is filled from the vendor kernel DT
 * (arch/arm64/boot/dts/mediatek/mt6789.dts) and gpufreq/v2/gpufreq_mt6789.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MTK_GPU_SOC_H
#define MTK_GPU_SOC_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#pragma pack(push, 8)

#define MTK_GPU_MAX_REGIONS 4
#define MTK_GPU_MAX_IRQS    8

/* Named MMIO regions (indices into MTK_GPU_SOC_DESC.Regions). */
enum {
    MTK_REGION_GPU = 0,      /* Mali-G57 register block, e.g. 0x13000000      */
    MTK_REGION_MFG_TOP,      /* MFG_TOP_CONFIG / G3D_CONFIG                    */
    MTK_REGION_APMIXED,      /* PLL controller (MFGPLL_CON*)                   */
    MTK_REGION_SLEEP,        /* SPM / power status                             */
    MTK_REGION_TOPCKGEN,     /* top clock gate/mux (CLK_CFG_*, mfg_pll_sel)    */
    MTK_REGION_INFRACFG_AO,  /* infra_ao (AXI bus-protect registers)           */
    MTK_REGION_BCRM,         /* infra_bcrm (way_en)                            */
    MTK_REGION_COUNT
};

typedef struct _MTK_GPU_REGION {
    UINT64 Base;
    UINT32 Size;
} MTK_GPU_REGION;

/* One step of the data-driven power-on / power-off script. */
typedef struct _MTK_GPU_REG_OP {
    UINT32 Region;           /* index into Regions[]                    */
    UINT32 Offset;           /* byte offset                             */
    UINT32 OrMask;           /* bits to set   (applied first)           */
    UINT32 AndMask;          /* bits to clear (applied second)          */
    UINT32 DelayUs;          /* delay after the write                   */
} MTK_GPU_REG_OP;

/*
 * SPM (sleep controller) MTCMOS power domain. Values from
 * drivers/soc/mediatek/mtk-scpsys-mt6789.c, drivers/soc/mediatek/mtk-scpsys.c
 * and dt-bindings/power/mt6789-power.h. Registers live at the SLEEP region
 * base (0x10006000); each domain's control register is at MfgDomains[].CtlOfs
 * (0x308/0x30C/0x310/0x314 for MT6789 MFG0..3).
 *
 * Register bit convention (verified against mtk-scpsys.c scpsys_power_on/off):
 *   bit 0  : PWR_RST_B      (1 = out of reset)
 *   bit 1  : PWR_ISO        (1 = bus isolated)
 *   bit 2  : PWR_ON         (1 = domain on)
 *   bit 3  : PWR_ON_2ND
 *   bit 4  : PWR_CLK_DIS    (1 = clock disabled)
 *   bit 5  : PWR_SRAM_CLKISO
 *   bit 6  : PWR_SRAM_ISOINT_B
 *   bit 8  : SRAM_PDN       (1 = SRAM powered down)
 *   bit 12 : SRAM_PDN_ACK
 *
 * Power status lives in the SLEEP region at PwrStatusOfs/PwrStatus2ndOfs
 * (0x16C/0x170 for MT6789), one bit per domain (MFG0..3 = bits 2..5).
 */
#define MTK_SPM_PWR_RST_B_BIT    (1u << 0)
#define MTK_SPM_PWR_ISO_BIT      (1u << 1)
#define MTK_SPM_PWR_ON_BIT       (1u << 2)
#define MTK_SPM_PWR_ON_2ND_BIT   (1u << 3)
#define MTK_SPM_PWR_CLK_DIS_BIT  (1u << 4)
#define MTK_SPM_SRAM_PDN_BIT     (1u << 8)
#define MTK_SPM_SRAM_PDN_ACK_BIT (1u << 12)

typedef struct _MTK_GPU_PWR_DOMAIN {
    UINT32 CtlOfs;           /* SPM control register offset             */
    UINT32 StaMask;          /* power-status bit for this domain        */
    UINT32 SramPdnBit;       /* SRAM power-down bit                     */
    UINT32 SramPdnAckBit;    /* SRAM power-down ack bit                 */
} MTK_GPU_PWR_DOMAIN;

#define MTK_GPU_MAX_PWR_DOMAINS 4

typedef struct _MTK_GPU_SOC_DESC {
    PCSTR  Name;             /* "MT6789"                                */
    UINT32 SocId;            /* MALI_SOC_*                              */
    UINT32 GpuModel;         /* MALI_MODEL_*                            */

    MTK_GPU_REGION Regions[MTK_REGION_COUNT];

    /* IRQs (ACPI GSIs where available, else vendor GIC SPIs). */
    UINT32 Irqs[MTK_GPU_MAX_IRQS];   /* GPU, MMU, JOB, EVENT, PWR       */
    UINT32 IrqCount;

    /* Power / clock constants harvested from mtk-scpsys-mt6789.c. */
    UINT32 PwrStatusOfs;     /* 0x16C (SPM PWR_STATUS)                  */
    UINT32 PwrStatus2ndOfs;  /* 0x170 (SPM PWR_STATUS_2ND)              */
    UINT32 MfgPwrMask;       /* 0x3C  (MFG0..3 = status bits 2..5)      */

    UINT32 MfPllConOfs[4];   /* 0x268, 0x26C, 0x270, 0x274 (APMIXED)    */
    UINT32 ClkMuxOfs;        /* 0 = skip; per-SoC clock mux register    */
    UINT32 ClkMuxRefCore;    /* 4                                       */
    UINT32 ClkMuxRefPark;    /* 5                                       */
    UINT32 ShaderPresentOfs; /* 0x100 (SHADER_PRESENT_LO)               */

    /*
     * Core power-up. Read the *PRESENT bitmap, write it to *PWRON, wait for
     * *READY. Set the PwrOn offsets to 0 to skip (unproven SoC).
     */
    UINT32 ShaderPwrOnOfs;   /* 0x180 (SHADER_PWRON_LO)                 */
    UINT32 ShaderReadyOfs;   /* 0x140 (SHADER_READY_LO)                 */
    UINT32 ShaderPwrOffOfs;  /* 0x1C0 (SHADER_PWROFF_LO)                */
    UINT32 TilerPresentOfs;  /* 0x110 (TILER_PRESENT_LO)                */
    UINT32 TilerPwrOnOfs;    /* 0x190 (TILER_PWRON_LO)                  */
    UINT32 L2PresentOfs;     /* 0x120 (L2_PRESENT_LO)                   */
    UINT32 L2PwrOnOfs;       /* 0x1A0 (L2_PWRON_LO)                     */

    UINT32 MaxFreqKhz;       /* 1100000                                 */

    MTK_GPU_PWR_DOMAIN MfgDomains[MTK_GPU_MAX_PWR_DOMAINS];
    UINT32 MfgDomainCount;

    const MTK_GPU_REG_OP *PowerOnOps;
    UINT32 PowerOnOpCount;
    const MTK_GPU_REG_OP *PowerOffOps;
    UINT32 PowerOffOpCount;
} MTK_GPU_SOC_DESC;

/* Returns NULL if unknown. */
const MTK_GPU_SOC_DESC *MtkGpuGetSocDescByName(_In_ PCSTR Name);
const MTK_GPU_SOC_DESC *MtkGpuGetSocDescById(_In_ UINT32 SocId);

#pragma pack(pop)

#endif /* MTK_GPU_SOC_H */
