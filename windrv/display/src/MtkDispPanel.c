/*
 * MtkDispPanel.c  -  Per-board panel descriptor table.
 *
 * Sources for every value below (all measured, nothing guessed):
 *
 *   * LK boot log of the stock device (firmware workspace evidence
 *     hardware-stock-r4/pl_lk.log):
 *       lcm.driver.name = m100t_ft8205_jt130wuf001a01_pen_wuxga
 *       mt_disp_init ... found:1, lcm:"m100t_ft8205_jt130wuf001a01_pen_wuxga"
 *       physical_screen.width=1200  physical_screen.height=1920
 *       mt_disp_get_lcd_time, fps=6006        -> 60.06 Hz
 *
 *   * Stock device tree (firmware workspace evidence stock-contract,
 *     read with windrv\display\tools\dtb_read.py):
 *       /lcds/<panel>/cust,dsi-lane-number     = 4
 *       /lcds/<panel>/cust,dsi-color-format    = "rgb888"
 *       /lcds/<panel>/cust,mipi_dsi_mode_lpm   = 1
 *
 *   * Vendor panel module cust_panel.ko (kernel module of the stock device):
 *       the panel's DCS init sequence lives in cm_init_cmd and the descriptor
 *       flags in common_desc. Both are still to be captured; the flags below
 *       say so instead of pretending.
 *
 * SPDX-License-Identifier: MIT
 */
#include "mtk_disp_panel.h"

static const MTK_DISP_PANEL_DESC g_Panels[] = {
    {
        .Name            = "m100t_ft8205_jt130wuf001a01_pen_wuxga",
        .IcName          = "FT8205",
        .PanelCode       = "JT130WUF001A01",
        .Lanes           = 4,
        .ColorFormat     = MTK_DISP_COLOR_RGB888,
        .LpmEnable       = 1,
        .RefreshMilliHz  = 60060,
        .Flags           = MTK_DISP_PANEL_FLAG_TIMING_FROM_LK |
                          MTK_DISP_PANEL_FLAG_PORCHES_PENDING |
                          MTK_DISP_PANEL_FLAG_PHY_RATE_PENDING,
        /*
         * Active area as the bootloader drives it. The device tree advertises
         * 1200x2000 for the "wuxga2000" panels, but the panel itself scans out
         * 1200x1920 - the extra 80 rows are a virtual area the touch stack
         * also disagrees with (see the firmware workspace notes on the touch
         * coordinate extent being (0,0,1200,2000) while the scanout is
         * 1200x1920). The bootloader is the authority for what is on the glass.
         */
        .Timing = {
            .Width          = 1200,
            .Height         = 1920,
            .HBackPorch     = 0,
            .HFrontPorch    = 0,
            .HSyncLen       = 0,
            .VBackPorch     = 0,
            .VFrontPorch    = 0,
            .VSyncLen       = 0,
            .PixelClockHz   = 0,
        },
        .PowerOnSequence     = NULL,
        .PowerOnSequenceLen  = 0,
        .PowerOffSequence    = NULL,
        .PowerOffSequenceLen = 0,
        .InitialCommand      = NULL,
        .InitialCommandLen   = 0,
    },
};

static const MTK_DISP_PANEL_DESC *const g_PanelList[] = {
    &g_Panels[0],
};

const MTK_DISP_PANEL_DESC *MtkDispGetPanelByName(_In_ PCSTR Name)
{
    if (!Name) {
        return NULL;
    }
    for (UINT32 i = 0; i < (UINT32)(sizeof(g_PanelList) / sizeof(g_PanelList[0])); ++i) {
        const MTK_DISP_PANEL_DESC *p = g_PanelList[i];
        PCSTR a = p->Name;
        PCSTR b = Name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == 0 && *b == 0) {
            return p;
        }
    }
    return NULL;
}
