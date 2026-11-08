/*
 * jc_test.c  -  Host unit test for the Mali job-chain builder (mali_jc).
 *
 * Runs on the development machine (x64), not the target: the builder is pure C
 * and its output is a byte layout, so the whole 128-byte-alignment / job_index /
 * next_job encoding can be checked without a GPU. This is what keeps the ICD and
 * (later) the PanVK backend on one tested code path.
 *
 * CRT-free: own entry point, only kernel32 imported.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "mali_jc.h"

static HANDLE g_out;
static int    g_fail;

static void Out(const char *s) { DWORD n; WriteFile(g_out, s, (DWORD)lstrlenA(s), &n, NULL); }

static void Check(const char *name, int cond)
{
    Out(cond ? "  ok   " : "  FAIL ");
    Out(name);
    Out("\r\n");
    if (!cond) g_fail = 1;
}

static unsigned char g_buf[128 * 4];

int main(void)
{
    MaliJcBuilder b;
    uint32_t *d0, *d1, *d2;
    uint32_t i1, i2, i3, cap;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* ---- basic chain: NULL, WRITE_VALUE ZERO, CACHE_FLUSH ---- */
    MaliJcInit(&b, g_buf, 0x100000ull, sizeof(g_buf));
    Check("empty chain head is 0", MaliJcHead(&b) == 0);
    Check("empty count is 0", MaliJcCount(&b) == 0);

    i1 = MaliJcNull(&b);
    i2 = MaliJcWriteValueZero(&b, 0x200000ull);
    i3 = MaliJcCacheFlush(&b);

    Check("job indices are 1,2,3", i1 == 1 && i2 == 2 && i3 == 3);
    Check("count is 3", MaliJcCount(&b) == 3);
    Check("head is base GPU VA", MaliJcHead(&b) == 0x100000ull);

    d0 = (uint32_t *)(void *)g_buf;
    d1 = (uint32_t *)(void *)(g_buf + MALI_JC_STRIDE);
    d2 = (uint32_t *)(void *)(g_buf + 2 * MALI_JC_STRIDE);

    Check("descriptor stride is 128", ((unsigned char *)d1 - (unsigned char *)d0) == 128);

    /* word4 = 1 | (type<<1) | (index<<16) */
    Check("desc0 header = NULL idx1",        d0[4] == 0x00010003u);  /* 1 | 2<<1 | 1<<16 */
    Check("desc1 header = WRITE_VALUE idx2", d1[4] == 0x00020005u);  /* 1 | 2<<1 | 2<<16 */
    Check("desc2 header = CACHE_FLUSH idx3", d2[4] == 0x00030007u);  /* 1 | 3<<1 | 3<<16 */

    /* next_job links: d0 -> d1 -> d2 -> 0 */
    Check("desc0 next -> desc1", d0[6] == 0x100080u && d0[7] == 0u);
    Check("desc1 next -> desc2", d1[6] == 0x100100u && d1[7] == 0u);
    Check("desc2 next = 0 (end)", d2[6] == 0u && d2[7] == 0u);

    /* WRITE_VALUE payload at +32 */
    Check("wv address lo",      d1[8]  == 0x00200000u);
    Check("wv address hi",      d1[9]  == 0u);
    Check("wv value_descriptor = ZERO", d1[10] == MALI_JC_WV_ZERO);
    Check("wv reserved = 0",    d1[11] == 0u);
    Check("wv immediate lo/hi", d1[12] == 0u && d1[13] == 0u);

    /* ---- 64-bit address split ---- */
    MaliJcInit(&b, g_buf, 0x100000ull, sizeof(g_buf));
    MaliJcWriteValueZero(&b, 0x123456789ull);
    d0 = (uint32_t *)(void *)g_buf;
    Check("64-bit wv address lo", d0[8] == 0x23456789u);
    Check("64-bit wv address hi", d0[9] == 0x1u);

    /* ---- capacity limit ---- */
    MaliJcInit(&b, g_buf, 0x100000ull, MALI_JC_STRIDE * 2u);
    Check("room for 2", MaliJcHasRoom(&b, 2));
    Check("not room for 3", !MaliJcHasRoom(&b, 3));
    Check("emit 1 ok", MaliJcNull(&b) == 1);
    Check("emit 2 ok", MaliJcNull(&b) == 2);
    cap = MaliJcNull(&b);
    Check("emit 3 rejected", cap == 0);
    Check("count stays 2", MaliJcCount(&b) == 2);
    Check("no room left", !MaliJcHasRoom(&b, 1));

    Out(g_fail ? "RESULT: FAIL\r\n" : "RESULT: PASS\r\n");
    return g_fail ? 1 : 0;
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)main());
}
