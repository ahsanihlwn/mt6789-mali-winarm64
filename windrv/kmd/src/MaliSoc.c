/*
 * MaliSoc.c  -  Per-SoC descriptor table.
 *
 * Add a new SoC by appending one MTK_GPU_SOC_DESC here. Nothing else in the
 * driver is SoC-specific. Values are taken from the vendor kernel tree:
 *   arch/arm64/boot/dts/mediatek/<soc>.dts
 *   drivers/soc/mediatek/mtk-scpsys-<soc>.c
 *   drivers/gpu/mediatek/gpufreq/v2/gpufreq_<soc>.c
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include "mtk_gpu_soc.h"
#include "mali_kmd_ioctl.h"

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

/* ------------------------------------------------------------------ */
/* MT6789 (Helio G99) — full, verified against the vendor tree          */
/* ------------------------------------------------------------------ */
static const MTK_GPU_SOC_DESC g_Mt6789 = {
    .Name     = "MT6789",
    .SocId    = MALI_SOC_MT6789,
    .GpuModel = MALI_MODEL_G57,

    .Regions = {
        [MTK_REGION_GPU]      = { 0x13000000ull, 0x00004000 },
        [MTK_REGION_MFG_TOP]  = { 0x13FBF000ull, 0x00001000 },
        [MTK_REGION_APMIXED]  = { 0x1000C000ull, 0x00001000 }, /* apmixedsys */
        [MTK_REGION_SLEEP]    = { 0x10006000ull, 0x00001000 }, /* SPM/scpsys */
        [MTK_REGION_TOPCKGEN] = { 0x10000000ull, 0x00001000 }, /* topckgen  */
        [MTK_REGION_INFRACFG_AO] = { 0x10001000ull, 0x00001000 }, /* infra_ao */
        [MTK_REGION_BCRM]     = { 0x10215000ull, 0x00001000 }, /* infra_bcrm */
    },

    /* Vendor GIC SPIs. Windows GSI = SPI + 32 when published via MADT. */
    .Irqs     = { 396, 397, 398, 399, 400 },
    .IrqCount = 5,

    .PwrStatusOfs    = 0x16C,   /* SPM PWR_STATUS      (mtk-scpsys-mt6789) */
    .PwrStatus2ndOfs = 0x170,   /* SPM PWR_STATUS_2ND                     */
    .MfgPwrMask      = 0x3C,    /* MFG0..3 = status bits 2..5             */

    .MfPllConOfs  = { 0x268, 0x26C, 0x270, 0x274 },  /* APMIXED MFGPLL_CON0..3 */
    /*
     * The MFG clock mux is TOPCKGEN CLK_CFG_4 (bit 18 = mfg_pll_sel), not an
     * MFG_TOP register, and the MFG PLL lives in APMIXED. Enabling that clock
     * tree (CCF equivalent) is Phase 2h; until then leave the mux untouched so
     * power-on cannot write a bogus clock register.
     */
    .ClkMuxOfs    = 0,
    .ClkMuxRefCore = 0,
    .ClkMuxRefPark = 0,
    .ShaderPresentOfs = 0x100,

    .ShaderPwrOnOfs  = 0x180,
    .ShaderReadyOfs  = 0x140,
    .ShaderPwrOffOfs = 0x1C0,
    .TilerPresentOfs = 0x110,
    .TilerPwrOnOfs   = 0x190,
    .L2PresentOfs    = 0x120,
    .L2PwrOnOfs      = 0x1A0,

    .MaxFreqKhz = 1100000,

    .MfgDomains = {
        { 0x308, BIT(2), BIT(8), BIT(12) },   /* MFG0 */
        { 0x30C, BIT(3), BIT(8), BIT(12) },   /* MFG1 */
        { 0x310, BIT(4), BIT(8), BIT(12) },   /* MFG2 */
        { 0x314, BIT(5), BIT(8), BIT(12) },   /* MFG3 */
    },
    .MfgDomainCount = 4,

    .PowerOnOps = NULL, .PowerOnOpCount = 0,
    .PowerOffOps = NULL, .PowerOffOpCount = 0,
};

/* ------------------------------------------------------------------ */
/* Sibling Valhall MTK SoCs. GPU block layout is shared (mali@13000000,  */
/* g3d_config@13fbf000); the SPM offsets differ and are left empty until */
/* confirmed, so power-on refuses instead of poking wrong registers.     */
/* ------------------------------------------------------------------ */
#define MTK_COMMON_VALHALL_REGIONS                                                \
    .Regions = {                                                                  \
        [MTK_REGION_GPU]     = { 0x13000000ull, 0x00004000 },                     \
        [MTK_REGION_MFG_TOP] = { 0x13FBF000ull, 0x00001000 },                     \
        [MTK_REGION_APMIXED] = { 0x1000C000ull, 0x00001000 },                     \
        [MTK_REGION_SLEEP]   = { 0x10006000ull, 0x00001000 },                     \
    },                                                                            \
    .Irqs = { 396, 397, 398, 399, 400 }, .IrqCount = 5,                           \
    /* SPM offsets are unverified for these siblings: leave the mask zero so  */  \
    /* identification is skipped and power-on refuses instead of poking them. */  \
    .PwrStatusOfs = 0x16C, .PwrStatus2ndOfs = 0x170, .MfgPwrMask = 0,             \
    .MfPllConOfs = { 0x268, 0x26C, 0x270, 0x274 },                                \
    .ClkMuxOfs = 0, .ClkMuxRefCore = 0, .ClkMuxRefPark = 0,                       \
    .ShaderPresentOfs = 0x100,                                                    \
    .ShaderPwrOnOfs = 0x180, .ShaderReadyOfs = 0x140, .ShaderPwrOffOfs = 0x1C0,   \
    .TilerPresentOfs = 0x110, .TilerPwrOnOfs = 0x190,                             \
    .L2PresentOfs = 0x120, .L2PwrOnOfs = 0x1A0

static const MTK_GPU_SOC_DESC g_Mt6833 = {
    .Name = "MT6833", .SocId = MALI_SOC_MT6833, .GpuModel = MALI_MODEL_G57,
    MTK_COMMON_VALHALL_REGIONS,
    .MaxFreqKhz = 950000,
    .MfgDomainCount = 0,          /* TODO: fill from mtk-scpsys-mt6833.c */
};

static const MTK_GPU_SOC_DESC g_Mt6853 = {
    .Name = "MT6853", .SocId = MALI_SOC_MT6853, .GpuModel = MALI_MODEL_G57,
    MTK_COMMON_VALHALL_REGIONS,
    .MaxFreqKhz = 850000,
    .MfgDomainCount = 0,          /* TODO */
};

static const MTK_GPU_SOC_DESC g_Mt6879 = {
    .Name = "MT6879", .SocId = MALI_SOC_MT6879, .GpuModel = MALI_MODEL_G57,
    MTK_COMMON_VALHALL_REGIONS,
    .MaxFreqKhz = 950000,
    .MfgDomainCount = 0,          /* TODO */
};

static const MTK_GPU_SOC_DESC g_Mt6895 = {
    .Name = "MT6895", .SocId = MALI_SOC_MT6895, .GpuModel = MALI_MODEL_G57,
    MTK_COMMON_VALHALL_REGIONS,
    .MaxFreqKhz = 1100000,
    .MfgDomainCount = 0,          /* TODO */
};

static const MTK_GPU_SOC_DESC *const g_SocTable[] = {
    &g_Mt6789, &g_Mt6833, &g_Mt6853, &g_Mt6879, &g_Mt6895,
};

const MTK_GPU_SOC_DESC *MtkGpuGetSocDescById(_In_ UINT32 SocId)
{
    UINT32 i;
    for (i = 0; i < RTL_NUMBER_OF(g_SocTable); i++) {
        if (g_SocTable[i]->SocId == SocId) {
            return g_SocTable[i];
        }
    }
    return NULL;
}

/* Small case-insensitive ASCII compare: no CRT in kernel mode. */
static BOOLEAN MaliNameEq(_In_ PCSTR A, _In_ PCSTR B)
{
    while (*A && *B) {
        CHAR ca = (*A >= 'a' && *A <= 'z') ? (CHAR)(*A - 'a' + 'A') : *A;
        CHAR cb = (*B >= 'a' && *B <= 'z') ? (CHAR)(*B - 'a' + 'A') : *B;
        if (ca != cb) {
            return FALSE;
        }
        A++;
        B++;
    }
    return (*A == 0 && *B == 0);
}

const MTK_GPU_SOC_DESC *MtkGpuGetSocDescByName(_In_ PCSTR Name)
{
    UINT32 i;
    if (Name == NULL) {
        return NULL;
    }
    for (i = 0; i < RTL_NUMBER_OF(g_SocTable); i++) {
        if (MaliNameEq(g_SocTable[i]->Name, Name)) {
            return g_SocTable[i];
        }
    }
    return NULL;
}