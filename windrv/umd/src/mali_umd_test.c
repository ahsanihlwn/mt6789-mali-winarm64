/*
 * mali_umd_test.c  -  Round-trip test for the Mali UMD winsys (windrv/umd).
 *
 * Exercises the whole user-mode path against a running MaliKmd:
 *   open -> version -> caps -> power on -> MMU enable ->
 *   alloc BO -> CPU write/read the mapping -> BO info ->
 *   job self test (null job) -> free BO -> close
 *
 * CRT-free, same style as tools/malitest.c: own entry point, own formatting,
 * only kernel32/setupapi/advapi32 imported.
 *
 * Usage:
 *   mali_umd_test.exe [--list] [--no-power]
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "mali_umd.h"

static HANDLE g_out;

static void Out(const char *s) { DWORD n; WriteFile(g_out, s, (DWORD)lstrlenA(s), &n, NULL); }
static void OutU(unsigned v, unsigned base, int width)
{
    char b[24]; const char *h = "0123456789ABCDEF"; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v) { b[i++] = h[v % base]; v /= base; }
    while (i < width) b[i++] = '0';
    while (i) { DWORD n; WriteFile(g_out, &b[--i], 1, &n, NULL); }
}
static void OutHex(unsigned v) { Out("0x"); OutU(v, 16, 8); }
static void OutHex64(unsigned long long v)
{
    Out("0x"); OutU((unsigned)(v >> 32), 16, 8); OutU((unsigned)v, 16, 8);
}
static void OutDec(unsigned v) { OutU(v, 10, 0); }
/* KMD BuildTag is a WCHAR[32]; print the ASCII subset. */
static void OutTag(const WCHAR *tag)
{
    char b[33]; int i;
    for (i = 0; i < 32 && tag[i]; i++) b[i] = (char)(tag[i] & 0xFF);
    b[i] = 0;
    Out(b);
}
static int Has(const char *hay, const char *needle)
{
    int n = lstrlenA(needle), h = lstrlenA(hay), i, j;
    for (i = 0; i + n <= h; i++) { for (j = 0; j < n && hay[i + j] == needle[j]; j++) {} if (j == n) return 1; }
    return 0;
}

static void PrintCaps(PMALI_UMD_DEVICE dev)
{
    MALI_KMD_CAPS c;
    if (!MaliUmdGetCaps(dev, &c)) { Out("GET_CAPS failed\r\n"); return; }
    Out("SoC "); OutHex(c.SocId);
    Out("  model "); OutHex(c.GpuModel);
    Out("  GPU_ID "); OutHex(c.GpuIdRegister); Out("\r\n");
    Out("CORE "); OutHex(c.CoreFeatures);
    Out("  MMU "); OutHex(c.MmuFeatures);
    Out("  SHADER "); OutHex(c.ShaderPresent);
    Out("  IRQs "); OutDec(c.IrqCount);
    Out("  flags "); OutHex(c.Flags); Out("\r\n");
}

static int ListDevices(void)
{
    /* Static (not stack) to avoid emitting a stack-probe call in a CRT-free
     * image: clang adds __chkstk for large frames, which we do not link. */
    static WCHAR paths[16][260];
    ULONG n, i;
    n = MaliUmdEnumerate(&paths[0][0], 16);
    Out("found "); OutDec(n); Out(" device interface(s)\r\n");
    for (i = 0; i < n; i++) { Out("  "); OutTag(paths[i]); Out("\r\n"); }
    return 0;
}

static int Run(void)
{
    const char *cmd = GetCommandLineA();
    PMALI_UMD_DEVICE dev;
    MALI_KMD_VERSION ver;
    MALI_UMD_BO bo;
    MALI_KMD_BO_INFO info;
    UINT32 js = 0xFFFFFFFFu, irq = 0;
    int rc = 0;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);

    if (Has(cmd, "--list")) return ListDevices();

    dev = MaliUmdOpen();
    if (!dev) { Out("Mali KMD device not found (is MaliKmd installed/started?)\r\n"); return 1; }

    if (MaliUmdGetVersion(dev, &ver)) {
        Out("ABI "); OutDec(ver.AbiVersion);
        Out("  driver "); OutDec(ver.DriverMajor); Out("."); OutDec(ver.DriverMinor);
        Out("  ("); OutTag(ver.BuildTag); Out(")\r\n");
    } else {
        Out("GET_VERSION failed: "); OutHex(GetLastError()); Out("\r\n");
    }

    PrintCaps(dev);

    if (!Has(cmd, "--no-power")) {
        if (MaliUmdPowerOn(dev)) Out("power on\r\n");
        else { Out("power on failed: "); OutHex(GetLastError()); Out("\r\n"); rc = 2; }
    }

    if (!MaliUmdMmuEnable(dev)) {
        Out("MMU enable failed: "); OutHex(GetLastError()); Out("\r\n"); rc = 2;
    } else {
        Out("MMU enabled\r\n");
    }

    /* Buffer object: allocate, touch the CPU mapping, read it back. */
    if (!MaliUmdBoAlloc(dev, 4096, MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD | MALI_BO_GPU_WR, &bo)) {
        Out("BO alloc failed: "); OutHex(GetLastError()); Out("\r\n"); rc = 2;
    } else {
        volatile UINT32 *w = (volatile UINT32 *)bo.CpuVa;
        UINT32 got;
        Out("BO #"); OutDec(bo.Handle);
        Out(" GpuVa "); OutHex64(bo.GpuVa);
        Out(" CpuVa "); OutHex64((unsigned long long)(uintptr_t)bo.CpuVa); Out("\r\n");

        if (bo.CpuVa) {
            w[0] = 0xA5A5A5A5u;
            got  = w[0];
            Out("BO CPU r/w "); OutHex(got);
            Out(got == 0xA5A5A5A5u ? "  OK\r\n" : "  MISMATCH\r\n");
            if (got != 0xA5A5A5A5u) rc = 3;
        }

        if (MaliUmdBoInfo(dev, bo.Handle, &info)) {
            Out("BO info: size "); OutHex64(info.Size);
            Out(" phys "); OutHex64(info.PhysBase); Out("\r\n");
        }

        /* Exercise the submit path end to end with the null-job self test,
         * which the KMD drives from a fresh BO of its own. */
        if (MaliUmdJobSelfTest(dev, 0, &js, &irq)) {
            Out("job selftest: JS_STATUS "); OutHex(js);
            Out("  IRQ "); OutHex(irq);
            Out(js == 0 ? "  PASS\r\n" : "  (non-zero)\r\n");
            if (js != 0) rc = 4;
        } else {
            Out("job selftest failed: "); OutHex(GetLastError()); Out("\r\n");
        }

        MaliUmdBoFree(dev, bo.Handle);
    }

    MaliUmdClose(dev);
    Out(rc == 0 ? "RESULT: PASS\r\n" : "RESULT: FAIL\r\n");
    return rc;
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)Run());
}
