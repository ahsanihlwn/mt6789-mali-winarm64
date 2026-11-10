/*
 * disp_soc_test.c  -  Host test for the display SoC descriptor table.
 *
 * Pure data checks: no device, no driver, no GPU. The point is that the
 * numbers that were measured from the vendor device tree stay internally
 * consistent (aligned, non-overlapping, IRQ mapping closed, scan-out chain
 * complete), so a future display miniport can trust the table.
 *
 * CRT-free, like the other host tools: own entry point, only kernel32 imported.
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdint.h>
#include "mtk_disp_soc.h"
#include "mtk_disp_panel.h"

static int g_Failures = 0;
static int g_Checks   = 0;

static void t_puts(const char *s)
{
    DWORD written = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(h, s, (DWORD)lstrlenA(s), &written, NULL);
}

static void t_putu(uint32_t v)
{
    char buf[16];
    int  n = 0;
    if (v == 0) {
        buf[n++] = '0';
    }
    while (v) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[16];
    for (int i = 0; i < n; ++i) {
        out[i] = buf[n - 1 - i];
    }
    out[n] = 0;
    t_puts(out);
}

static void check(int cond, const char *what)
{
    g_Checks++;
    if (!cond) {
        g_Failures++;
        t_puts("FAIL: ");
        t_puts(what);
        t_puts("\n");
    }
}

static void check_u32(int cond, const char *what, uint32_t got, uint32_t want)
{
    if (!cond) {
        g_Failures++;
        t_puts("FAIL: ");
        t_puts(what);
        t_puts(" got=");
        t_putu(got);
        t_puts(" want=");
        t_putu(want);
        t_puts("\n");
    }
    g_Checks++;
}

int main(void)
{
    const MTK_DISP_SOC_DESC *d = MtkDispGetSocDescByName("MT6789");
    check(d != NULL, "MT6789 descriptor found");
    check(MtkDispGetSocDescByName("MT9999") == NULL, "unknown SoC rejected");
    check(MtkDispGetSocDescByName(NULL) == NULL, "NULL name rejected");

    if (!d) {
        t_puts("cannot continue without the descriptor\n");
        return 1;
    }

    check_u32(d->GsiBase == 32, "GSI base is 32 (GICD GSI base 0)", d->GsiBase, 32);
    check_u32(d->RegionCount == MTK_DISP_REGION_COUNT, "region count matches the enum",
              d->RegionCount, MTK_DISP_REGION_COUNT);
    check_u32(d->RegionCount <= MTK_DISP_MAX_REGIONS, "region count fits the table",
              d->RegionCount, MTK_DISP_MAX_REGIONS);
    check_u32(d->PathLength > 0, "scan-out path is not empty", d->PathLength, 1);
    check_u32(d->PathLength <= MTK_DISP_MAX_PATH, "path fits the table",
              d->PathLength, MTK_DISP_MAX_PATH);

    /* Every region is filled, page aligned and page sized. */
    for (uint32_t i = 0; i < d->RegionCount; ++i) {
        const MTK_DISP_REGION *r = &d->Regions[i];
        check(r->Base != 0, "region base non-zero");
        check((r->Base & 0xFFFu) == 0, "region base 4 KiB aligned");
        check(r->Size != 0, "region size non-zero");
        check((r->Size & (r->Size - 1)) == 0, "region size is a power of two");
        check(r->Name != NULL && r->Name[0] != 0, "region name present");
        if (r->HasIrq) {
            check(r->Spi != 0, "region with IRQ has a non-zero SPI");
        }
    }

    /* No two regions overlap. */
    for (uint32_t i = 0; i < d->RegionCount; ++i) {
        for (uint32_t j = i + 1; j < d->RegionCount; ++j) {
            const MTK_DISP_REGION *a = &d->Regions[i];
            const MTK_DISP_REGION *b = &d->Regions[j];
            int overlap = (a->Base < b->Base + b->Size) && (b->Base < a->Base + a->Size);
            check(!overlap, "regions do not overlap");
        }
    }

    /* The scan-out chain starts at the overlay and ends at the DSI host. */
    check_u32(d->Path[0].Region == MTK_DISP_REGION_OVL0, "scan-out path starts at OVL0",
              d->Path[0].Region, MTK_DISP_REGION_OVL0);
    check_u32(d->Path[d->PathLength - 1].Region == MTK_DISP_REGION_DSI0,
              "scan-out path ends at the DSI host",
              d->Path[d->PathLength - 1].Region, MTK_DISP_REGION_DSI0);

    /* Every step points at a real region and names a clock. */
    for (uint32_t i = 0; i < d->PathLength; ++i) {
        check(d->Path[i].Region < d->RegionCount, "path step region in range");
        check(d->Path[i].Clock != NULL && d->Path[i].Clock[0] != 0, "path step has a clock");
        int found = 0;
        for (uint32_t c = 0; c < d->ClockCount; ++c) {
            const char *a = d->Clocks[c];
            const char *b = d->Path[i].Clock;
            while (*a && *a == *b) { ++a; ++b; }
            if (*a == 0 && *b == 0) { found = 1; break; }
        }
        check(found, "path step clock is in the clock list");
    }

    /* GSI mapping is closed: every SPI with an IRQ maps to SPI + GsiBase. */
    for (uint32_t i = 0; i < d->RegionCount; ++i) {
        const MTK_DISP_REGION *r = &d->Regions[i];
        if (!r->HasIrq) {
            continue;
        }
        check_u32(MtkDispGsiFromSpi(d, r->Spi) == r->Spi + d->GsiBase,
                  "GSI = SPI + GsiBase",
                  MtkDispGsiFromSpi(d, r->Spi), r->Spi + d->GsiBase);
    }
    check_u32(MtkDispGsiFromSpi(d, 0xFFFF) == 0xFFFFFFFFu, "unknown SPI has no GSI",
              MtkDispGsiFromSpi(d, 0xFFFF), 0xFFFFFFFFu);
    check_u32(MtkDispGsiFromSpi(NULL, 287) == 0xFFFFFFFFu, "NULL descriptor has no GSI",
              MtkDispGsiFromSpi(NULL, 287), 0xFFFFFFFFu);

    /* The DSI PHY is a different block from the DSI host. */
    check(d->Regions[MTK_DISP_REGION_DSI0].Base != d->Regions[MTK_DISP_REGION_DSI_PHY].Base,
          "DSI host and DSI PHY are distinct blocks");

    /* --- panel descriptor ------------------------------------------------- */
    const MTK_DISP_PANEL_DESC *p =
        MtkDispGetPanelByName("m100t_ft8205_jt130wuf001a01_pen_wuxga");
    check(p != NULL, "P13001L panel descriptor found");
    check(MtkDispGetPanelByName("m100t_nonexistent") == NULL, "unknown panel rejected");
    check(MtkDispGetPanelByName(NULL) == NULL, "NULL panel name rejected");

    if (p) {
        check(p->Lanes >= 1 && p->Lanes <= 8, "panel lane count is sane");
        check(p->ColorFormat == MTK_DISP_COLOR_RGB888 ||
              p->ColorFormat == MTK_DISP_COLOR_RGB666 ||
              p->ColorFormat == MTK_DISP_COLOR_RGB565, "panel colour format is known");
        check(p->RefreshMilliHz > 0, "panel refresh rate is set");
        check(p->Timing.Width > 0 && p->Timing.Height > 0, "panel active size is set");
        check(p->IcName != NULL && p->IcName[0] != 0, "panel IC name is set");
        check(p->PanelCode != NULL && p->PanelCode[0] != 0, "panel code is set");

        /* If the init blob is not captured, the flags must say so. */
        if (p->InitialCommand == NULL) {
            check((p->Flags & MTK_DISP_PANEL_FLAG_INIT_CAPTURED) == 0,
                  "uncaptured init sequence is flagged");
        } else {
            check(p->InitialCommandLen > 0, "captured init sequence has a length");
        }
        if (p->Timing.PixelClockHz == 0) {
            check((p->Flags & MTK_DISP_PANEL_FLAG_PHY_RATE_PENDING) != 0,
                  "unknown pixel clock is flagged");
        }
    }

    t_puts(g_Failures ? "disp_soc_test: FAIL\n" : "disp_soc_test: PASS\n");
    t_puts("checks=");
    t_putu((uint32_t)g_Checks);
    t_puts(" failures=");
    t_putu((uint32_t)g_Failures);
    t_puts("\n");
    return g_Failures ? 1 : 0;
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)main());
}
