/*
 * mtk_disp_soc.h  -  Per-SoC descriptor for the MediaTek display subsystem
 *                   (MMSYS: OVL -> COLOR -> AAL -> GAMMA -> RDMA -> DSI).
 *
 * The same idea as mtk_gpu_soc.h: the driver logic is identical for every SoC,
 * only this table changes. Every address and interrupt below is taken from the
 * vendor kernel device tree
 * (arch/arm64/boot/dts/mediatek/mt6789.dts, Xiaomi "emerald" tree) - nothing
 * here is guessed. The DTS line each value comes from is quoted in the
 * matching comment.
 *
 * This is the data half of the display work (ladder L5 in DISPLAY-ANDROID.md):
 * it is what a Windows display miniport needs before it can touch a single
 * register, and it is what makes the driver portable to other MediaTek parts.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MTK_DISP_SOC_H
#define MTK_DISP_SOC_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#pragma pack(push, 8)

#define MTK_DISP_MAX_REGIONS 16
#define MTK_DISP_MAX_PATH     8
#define MTK_DISP_MAX_CLOCKS  16

/*
 * Named MMIO regions. Indices are stable: they are used by the path table and
 * by the register-access helpers.
 */
enum {
    MTK_DISP_REGION_SYS_CONFIG = 0, /* 0x14000000 dispsys_config (MUTEX/clock glue) */
    MTK_DISP_REGION_MUTEX,          /* 0x14001000 disp_mutex0                     */
    MTK_DISP_REGION_SMI_LARB0,      /* 0x14003000 smi_larb0                       */
    MTK_DISP_REGION_OVL0,           /* 0x14005000 disp_ovl0   (primary overlay)   */
    MTK_DISP_REGION_OVL0_2L,        /* 0x14006000 disp_ovl0_2l (2-layer variant)  */
    MTK_DISP_REGION_RDMA0,          /* 0x14007000 disp_rdma0  (memory -> stream)   */
    MTK_DISP_REGION_RSZ0,           /* 0x14008000 disp_rsz0                      */
    MTK_DISP_REGION_COLOR0,         /* 0x14009000 disp_color0                    */
    MTK_DISP_REGION_AAL0,           /* 0x1400C000 disp_aal0                      */
    MTK_DISP_REGION_GAMMA0,         /* 0x1400D000 disp_gamma0                    */
    MTK_DISP_REGION_DSI0,           /* 0x14013000 dsi0 host                      */
    MTK_DISP_REGION_DSI_PHY,        /* 0x11F60000 mipi_tx_config0 (DSI PHY/PLL)   */
    MTK_DISP_REGION_IOMMU,          /* 0x14016000 disp_iommu (M4U display)        */
    MTK_DISP_REGION_COUNT
};

typedef struct _MTK_DISP_REGION {
    UINT64 Base;      /* physical base address                              */
    UINT32 Size;      /* window size (0x1000 for every block measured so far) */
    PCSTR  Name;      /* DTS node name, for logs and for cross-checking     */
    UINT32 Spi;       /* vendor GIC SPI of this block, 0 if it has none      */
    UINT32 HasIrq;    /* 1 when Spi is meaningful                           */
} MTK_DISP_REGION;

/*
 * One step of the fixed DDP ("path") that carries a frame from memory to the
 * panel. On MT6789 the chain is fixed wiring, not runtime routing, so the
 * steps are data, not branches.
 */
typedef struct _MTK_DISP_PATH_STEP {
    UINT32 Region;    /* index into Regions[]                                */
    PCSTR  Clock;     /* clock that must be on for this step (DTS name)     */
} MTK_DISP_PATH_STEP;

typedef struct _MTK_DISP_SOC_DESC {
    PCSTR  Name;      /* "MT6789"                                            */
    UINT32 GsiBase;   /* Windows GSI = SPI + GsiBase (32 when GICD base = 0) */

    MTK_DISP_REGION Regions[MTK_DISP_MAX_REGIONS];
    UINT32 RegionCount;

    MTK_DISP_PATH_STEP Path[MTK_DISP_MAX_PATH];
    UINT32 PathLength;

    /*
     * Power and clock. The numeric identifiers live in dt-bindings
     * (mt6789-power.h / mt6789-disp.h) that are not part of the vendor tree
     * we have, so only the names are carried here and the control-register
     * offsets stay 0 ("unknown") until the scpsys source is available.
     */
    PCSTR  PowerDomainName;    /* "DISP"                                   */
    UINT32 PowerDomainId;      /* 0xFFFFFFFF when unknown                   */
    UINT32 PowerDomainCtlOfs;  /* SPM control register offset, 0 = unknown   */
    UINT32 PowerDomainStaMask; /* power-status bit mask, 0 = unknown        */

    PCSTR  Clocks[MTK_DISP_MAX_CLOCKS];
    UINT32 ClockCount;
} MTK_DISP_SOC_DESC;

/* Returns NULL if the SoC is unknown. */
const MTK_DISP_SOC_DESC *MtkDispGetSocDescByName(_In_ PCSTR Name);

/* Windows GSI for a vendor SPI, or 0xFFFFFFFF when the block has no IRQ. */
UINT32 MtkDispGsiFromSpi(_In_ const MTK_DISP_SOC_DESC *Desc, _In_ UINT32 Spi);

#pragma pack(pop)

#endif /* MTK_DISP_SOC_H */
