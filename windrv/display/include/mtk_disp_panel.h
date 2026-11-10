/*
 * mtk_disp_panel.h  -  Per-board panel descriptor for the MediaTek display path.
 *
 * Same idea as mtk_gpu_soc.h: the driver logic is SoC- and board-independent,
 * only data changes. The panel on this device is a FocalTech FT8205 DSI bridge
 * driving a WUXGA panel, and everything the driver needs about it is a number
 * or a byte blob - never code.
 *
 * That is not a stylistic choice: it is how the vendor does it (device tree
 * properties, see DISPLAY-MMSYS-REGS.md) and how Qualcomm does it on Windows
 * (base INF + OEM extension INF). A Windows driver that hard-codes one panel
 * is not portable; one that reads a descriptor is.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MTK_DISP_PANEL_H
#define MTK_DISP_PANEL_H

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#pragma pack(push, 8)

#define MTK_DISP_MAX_PANELS 8

/* Colour formats we can describe. */
typedef enum _MTK_DISP_COLOR_FORMAT {
    MTK_DISP_COLOR_RGB888 = 0,
    MTK_DISP_COLOR_RGB666,
    MTK_DISP_COLOR_RGB565
} MTK_DISP_COLOR_FORMAT;

/* What is known and what is still missing for this panel. */
#define MTK_DISP_PANEL_FLAG_TIMING_FROM_LK      0x0001u /* timing comes from the bootloader */
#define MTK_DISP_PANEL_FLAG_INIT_CAPTURED       0x0002u /* init command blob is present    */
#define MTK_DISP_PANEL_FLAG_PORCHES_PENDING     0x0004u /* porches/sync not captured yet   */
#define MTK_DISP_PANEL_FLAG_PHY_RATE_PENDING    0x0008u /* DSI data rate not captured yet  */

typedef struct _MTK_DISP_PANEL_TIMING {
    UINT32 Width;           /* active pixels, portrait as the panel is mounted */
    UINT32 Height;
    UINT32 HBackPorch;
    UINT32 HFrontPorch;
    UINT32 HSyncLen;
    UINT32 VBackPorch;
    UINT32 VFrontPorch;
    UINT32 VSyncLen;
    UINT32 PixelClockHz;    /* 0 = not known yet                              */
} MTK_DISP_PANEL_TIMING;

typedef struct _MTK_DISP_PANEL_DESC {
    PCSTR  Name;            /* vendor LCM name, e.g. "m100t_ft8205_..._wuxga" */
    PCSTR  IcName;          /* "FT8205"                                        */
    PCSTR  PanelCode;       /* "JT130WUF001A01"                                */
    UINT32 Lanes;           /* DSI lane count                                  */
    UINT32 ColorFormat;     /* MTK_DISP_COLOR_FORMAT                           */
    UINT32 LpmEnable;       /* 1 = low-power mode enabled (the vendor default) */
    UINT32 RefreshMilliHz;  /* 60060 = 60.06 Hz                                */
    UINT32 Flags;           /* MTK_DISP_PANEL_FLAG_*                           */

    MTK_DISP_PANEL_TIMING Timing;

    /* DCS command blobs. NULL while not captured (see the flags above). */
    const UINT8 *PowerOnSequence;
    UINT32       PowerOnSequenceLen;
    const UINT8 *PowerOffSequence;
    UINT32       PowerOffSequenceLen;
    const UINT8 *InitialCommand;
    UINT32       InitialCommandLen;
} MTK_DISP_PANEL_DESC;

/* Returns NULL if the panel is unknown. */
const MTK_DISP_PANEL_DESC *MtkDispGetPanelByName(_In_ PCSTR Name);

#pragma pack(pop)

#endif /* MTK_DISP_PANEL_H */
