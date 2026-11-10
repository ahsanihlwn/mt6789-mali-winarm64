/*
 * MtkDispSoc.c  -  Per-SoC descriptor table for the display subsystem.
 *
 * Add a new SoC by appending one MTK_DISP_SOC_DESC here; nothing else in the
 * display driver is SoC-specific. Every value is taken from the vendor device
 * tree arch/arm64/boot/dts/mediatek/mt6789.dts (Xiaomi "emerald" tree,
 * adomerle/android_kernel_xiaomi_emerald@6931719). The DTS line numbers are
 * quoted so the table can be re-checked when the vendor tree moves.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mtk_disp_soc.h"

/* ------------------------------------------------------------------ */
/* MT6789 (Helio G99)                                                  */
/* ------------------------------------------------------------------ */
static const MTK_DISP_SOC_DESC g_Mt6789 = {
    .Name     = "MT6789",
    .GsiBase  = 32,   /* GICD GSI base = 0 -> GSI = SPI + 32 */

    .Regions = {
        /* DTS L4502 */ [MTK_DISP_REGION_SYS_CONFIG] = { 0x14000000ull, 0x1000, "dispsys_config",    0,    0 },
        /* DTS L4686 */ [MTK_DISP_REGION_MUTEX]      = { 0x14001000ull, 0x1000, "disp_mutex0",       0,    0 },
        /* DTS L4344 */ [MTK_DISP_REGION_SMI_LARB0]  = { 0x14003000ull, 0x1000, "smi_larb0",         0,    0 },
        /* DTS L4694 */ [MTK_DISP_REGION_OVL0]       = { 0x14005000ull, 0x1000, "disp_ovl0",       287,    1 },
        /* DTS L4716 */ [MTK_DISP_REGION_OVL0_2L]    = { 0x14006000ull, 0x1000, "disp_ovl0_2l",    288,    1 },
        /* DTS L4738 */ [MTK_DISP_REGION_RDMA0]      = { 0x14007000ull, 0x1000, "disp_rdma0",      289,    1 },
        /* DTS L4756 */ [MTK_DISP_REGION_RSZ0]       = { 0x14008000ull, 0x1000, "disp_rsz0",       290,    1 },
        /* DTS L4764 */ [MTK_DISP_REGION_COLOR0]     = { 0x14009000ull, 0x1000, "disp_color0",     291,    1 },
        /* DTS L4788 */ [MTK_DISP_REGION_AAL0]       = { 0x1400C000ull, 0x1000, "disp_aal0",       294,    1 },
        /* DTS L4798 */ [MTK_DISP_REGION_GAMMA0]     = { 0x1400D000ull, 0x1000, "disp_gamma0",     295,    1 },
        /* DTS L4869 */ [MTK_DISP_REGION_DSI0]       = { 0x14013000ull, 0x1000, "dsi0",            301,    1 },
        /* DTS L4859 */ [MTK_DISP_REGION_DSI_PHY]    = { 0x11F60000ull, 0x1000, "mipi_tx_config0",   0,    0 },
        /* DTS L4931 */ [MTK_DISP_REGION_IOMMU]      = { 0x14016000ull, 0x1000, "disp_iommu",      303,    1 },
    },
    .RegionCount = MTK_DISP_REGION_COUNT,

    /*
     * The scan-out chain. It is fixed wiring on this SoC: the blocks are
     * reached through DDP components in this order, and every one of them has
     * to be clocked and enabled even when its feature is unused.
     */
    .Path = {
        { MTK_DISP_REGION_OVL0,   "CLK_MM_DISP_OVL0"    },  /* L4699 */
        { MTK_DISP_REGION_COLOR0, "CLK_MM_DISP_COLOR0"  },  /* L4769 */
        { MTK_DISP_REGION_AAL0,   "CLK_MM_DISP_AAL0"    },  /* L4793 */
        { MTK_DISP_REGION_GAMMA0, "CLK_MM_DISP_GAMMA0"  },  /* L4803 */
        { MTK_DISP_REGION_RDMA0,  "CLK_MM_DISP_RDMA0"   },  /* L4743 */
        { MTK_DISP_REGION_DSI0,   "CLK_MM_DSI0"         },  /* L4874 */
    },
    .PathLength = 6,

    /*
     * The DISP domain lives in scpsys. The numeric id and the control-register
     * offset come from dt-bindings/mediatek/mt6789-power.h and
     * drivers/soc/mediatek/mtk-scpsys-mt6789.c, neither of which is part of
     * the vendor tree we have. Left unknown on purpose - see
     * DISPLAY-MMSYS-REGS.md section 4.
     */
    .PowerDomainName    = "DISP",
    .PowerDomainId      = 0xFFFFFFFFu,
    .PowerDomainCtlOfs  = 0,
    .PowerDomainStaMask = 0,

    .Clocks = {
        "CLK_MM_DISP_26M",
        "CLK_MM_APB_BUS",
        "CLK_MM_DISP_MUTEX0",
        "CLK_MM_SMI_IOMMU",
        "CLK_MM_DISP_OVL0",
        "CLK_MM_DISP_OVL0_2L",
        "CLK_MM_DISP_RDMA0",
        "CLK_MM_DISP_RSZ0",
        "CLK_MM_DISP_COLOR0",
        "CLK_MM_DISP_AAL0",
        "CLK_MM_DISP_GAMMA0",
        "CLK_MM_DSI0",
        "CLK_MM_DSI0_DSI_CK_DOMAIN",
    },
    .ClockCount = 13,
};

static const MTK_DISP_SOC_DESC *const g_Socs[] = {
    &g_Mt6789,
};

const MTK_DISP_SOC_DESC *MtkDispGetSocDescByName(_In_ PCSTR Name)
{
    if (!Name) {
        return NULL;
    }
    for (UINT32 i = 0; i < (UINT32)(sizeof(g_Socs) / sizeof(g_Socs[0])); ++i) {
        const MTK_DISP_SOC_DESC *d = g_Socs[i];
        PCSTR a = d->Name;
        PCSTR b = Name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) {
            return d;
        }
    }
    return NULL;
}

UINT32 MtkDispGsiFromSpi(_In_ const MTK_DISP_SOC_DESC *Desc, _In_ UINT32 Spi)
{
    if (!Desc) {
        return 0xFFFFFFFFu;
    }
    for (UINT32 i = 0; i < Desc->RegionCount; ++i) {
        const MTK_DISP_REGION *r = &Desc->Regions[i];
        if (r->HasIrq && r->Spi == Spi) {
            return r->Spi + Desc->GsiBase;
        }
    }
    return 0xFFFFFFFFu;
}
