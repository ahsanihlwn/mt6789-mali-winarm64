/*
 * malitest.c  -  User-mode bring-up tool for the MediaTek Mali KMD (ARM64).
 *
 * CRT-free on purpose: this machine has clang + the Windows SDK but no MSVC
 * C runtime libraries, so the tool provides its own entry point, string/number
 * helpers and heap access. It only imports kernel32/setupapi/advapi32.
 *
 * Usage:
 *   malitest.exe [--on] [--bo] [--jstatus] [--job] [--irq]
 *   malitest.exe --clkdump                       (read-only clock/power state)
 *   malitest.exe --rd <block> <offset>           (raw MMIO read)
 *   malitest.exe --wr <block> <offset> <value>   (raw MMIO write)
 *
 * Blocks: 0=GPU 1=MFG_TOP 2=APMIXED 3=SLEEP 4=TOPCKGEN 5=INFRACFG_AO 6=BCRM
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdint.h>
#include "mali_kmd_ioctl.h"

/* {7c1f9a20-6d2b-4f4a-9b0e-1a2b3c4d5e6f} */
static const GUID GUID_DEVINTERFACE_MALI_KMD =
    { 0x7c1f9a20, 0x6d2b, 0x4f4a, { 0x9b, 0x0e, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f } };

/* ---- minimal freestanding runtime -------------------------------- */
void *memset(void *d, int v, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

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
static void OutHex(unsigned v)   { Out("0x"); OutU(v, 16, 8); }
static void OutHex64(unsigned long long v) { Out("0x"); OutU((unsigned)(v >> 32), 16, 8); OutU((unsigned)v, 16, 8); }
static void OutDec(unsigned v)   { OutU(v, 10, 0); }
static int Has(const char *hay, const char *needle)
{
    int n = lstrlenA(needle), h = lstrlenA(hay), i, j;
    for (i = 0; i + n <= h; i++) { for (j = 0; j < n && hay[i + j] == needle[j]; j++) {} if (j == n) return 1; }
    return 0;
}
/* Return the substring just after `needle`, or NULL. */
static const char *Find(const char *hay, const char *needle)
{
    int n = lstrlenA(needle), h = lstrlenA(hay), i, j;
    for (i = 0; i + n <= h; i++) { for (j = 0; j < n && hay[i + j] == needle[j]; j++) {} if (j == n) return hay + i + n; }
    return NULL;
}
static const char *SkipSpace(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
/* Parse one unsigned decimal or 0x-hex number; NULL if none. */
static const char *ParseU(const char *p, unsigned *out)
{
    unsigned v = 0; int any = 0, d;
    p = SkipSpace(p);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        for (;;) {
            int c = *p;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + (unsigned)d; p++; any = 1;
        }
    } else {
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); p++; any = 1; }
    }
    if (!any) return NULL;
    *out = v;
    return p;
}
static void *Alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void  Free(void *p)   { HeapFree(GetProcessHeap(), 0, p); }

static HANDLE OpenMaliDevice(void)
{
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifd;
    DWORD i, needed;
    HANDLE h = INVALID_HANDLE_VALUE;

    devs = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MALI_KMD, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    ifd.cbSize = sizeof(ifd);
    for (i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &GUID_DEVINTERFACE_MALI_KMD, i, &ifd); i++) {
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, NULL, 0, &needed, NULL);
        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)Alloc(needed);
        if (!detail) break;
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, needed, &needed, NULL)) {
            h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_EXISTING, 0, NULL);
        }
        Free(detail);
        if (h != INVALID_HANDLE_VALUE) break;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return h;
}

static BOOL DoIoctl(HANDLE h, DWORD code, void *in, DWORD inLen, void *out, DWORD outLen)
{
    DWORD ret = 0;
    return DeviceIoControl(h, code, in, inLen, out, outLen, &ret, NULL);
}

/* Raw MMIO helpers, used by --rawjob so the whole submit sequence can be
 * exercised from user mode without rebuilding the driver. */
static unsigned RdReg(HANDLE h, unsigned blk, unsigned ofs)
{
    MALI_KMD_REG_ACCESS ra; memset(&ra, 0, sizeof(ra));
    ra.Block = blk; ra.Offset = ofs;
    if (DoIoctl(h, IOCTL_MALI_REG_READ, &ra, sizeof(ra), &ra, sizeof(ra))) return ra.Value;
    return 0xFFFFFFFFu;
}
static BOOL WrReg(HANDLE h, unsigned blk, unsigned ofs, unsigned val)
{
    MALI_KMD_REG_ACCESS ra; memset(&ra, 0, sizeof(ra));
    ra.Block = blk; ra.Offset = ofs; ra.Value = val;
    return DoIoctl(h, IOCTL_MALI_REG_WRITE, &ra, sizeof(ra), &ra, sizeof(ra));
}
/* Poll until (reg & mask) == want; returns elapsed ms or -1. */
static int WaitMask(HANDLE h, unsigned ofs, unsigned mask, unsigned want, unsigned ms)
{
    unsigned i;
    for (i = 0; i <= ms; i++) {
        if ((RdReg(h, MALI_REG_BLOCK_GPU, ofs) & mask) == want) return (int)i;
        Sleep(1);
    }
    return -1;
}

/* The KMD refreshes the cached ID registers on power-on, so this is worth
 * calling again after --on to see the real GPU rather than the off-state 0. */
static void PrintCaps(HANDLE h)
{
    MALI_KMD_CAPS caps; memset(&caps, 0, sizeof(caps));
    if (DoIoctl(h, IOCTL_MALI_GET_CAPS, NULL, 0, &caps, sizeof(caps))) {
        Out("SoC "); OutHex(caps.SocId);
        Out("  GPU model "); OutHex(caps.GpuModel);
        Out("  GPU_ID "); OutHex(caps.GpuIdRegister); Out("\r\n");
        Out("CORE "); OutHex(caps.CoreFeatures);
        Out("  MMU "); OutHex(caps.MmuFeatures);
        Out("  SHADER "); OutHex(caps.ShaderPresent);
        Out("  IRQs "); OutDec(caps.IrqCount);
        Out("  Flags "); OutHex(caps.Flags); Out("\r\n");
    } else {
        Out("GET_CAPS failed: "); OutHex(GetLastError()); Out("\r\n");
    }
}

/* Raw dump of the JM state so a stuck/active slot can be diagnosed. */
static void DumpJobRegs(HANDLE h, const char *tag)
{
    static const struct { unsigned ofs; const char *name; } t[] = {
        { 0x1000, "JOB_IRQ_RAWSTAT  " },
        { 0x1004, "JOB_IRQ_CLEAR    " },
        { 0x1008, "JOB_IRQ_MASK     " },
        { 0x100C, "JOB_IRQ_STATUS   " },
        { 0x1010, "JOB_IRQ_JS_STATE " },
        { 0x1800, "JS0 HEAD_LO      " },
        { 0x1804, "JS0 HEAD_HI      " },
        { 0x1808, "JS0 TAIL_LO      " },
        { 0x1810, "JS0 AFFINITY_LO  " },
        { 0x1814, "JS0 AFFINITY_HI  " },
        { 0x1818, "JS0 CONFIG       " },
        { 0x1820, "JS0 JS_COMMAND   " },
        { 0x1824, "JS0 JS_STATUS    " },
        { 0x1840, "JS0 HEAD_NEXT_LO " },
        { 0x1850, "JS0 AFFIN_NEXT_LO" },
        { 0x1858, "JS0 CONFIG_NEXT  " },
        { 0x1860, "JS0 CMD_NEXT     " },
    };
    unsigned k;
    Out("-- JM regs ("); Out(tag); Out(") --\r\n");
    for (k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        MALI_KMD_REG_ACCESS ra; memset(&ra, 0, sizeof(ra));
        ra.Block = MALI_REG_BLOCK_GPU; ra.Offset = t[k].ofs;
        Out("  "); Out(t[k].name); Out(" ");
        if (DoIoctl(h, IOCTL_MALI_REG_READ, &ra, sizeof(ra), &ra, sizeof(ra))) OutHex(ra.Value);
        else Out("err");
        Out("\r\n");
    }
}

static int Run(void)
{
    const char *cmd = GetCommandLineA();
    HANDLE h;
    MALI_KMD_VERSION ver;
    MALI_KMD_PWR_STATUS pwr;
    int i;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    memset(&ver, 0, sizeof(ver));
    memset(&pwr, 0, sizeof(pwr));

    h = OpenMaliDevice();
    if (h == INVALID_HANDLE_VALUE) {
        Out("KMD device not found. Is MaliKmd installed and started?\r\n");
        return 1;
    }

    if (DoIoctl(h, IOCTL_MALI_GET_VERSION, NULL, 0, &ver, sizeof(ver))) {
        Out("ABI "); OutDec(ver.AbiVersion);
        Out("  driver "); OutDec(ver.DriverMajor); Out("."); OutDec(ver.DriverMinor);
        Out("  ("); Out((const char *)ver.BuildTag); Out(")\r\n");
    } else {
        Out("GET_VERSION failed: "); OutHex(GetLastError()); Out("\r\n");
    }

    PrintCaps(h);

    /*
     * Anything that touches the Mali register block needs the MTCMOS domain
     * on. Bring it up on demand here so callers cannot read a dead block.
     * --irq is pure counter reading and deliberately does not power on.
     */
    if (Has(cmd, "--bo") || Has(cmd, "--job") || Has(cmd, "--rawjob") || Has(cmd, "--jstatus") ||
        Has(cmd, "--greset") || Has(cmd, "--ghard") || Has(cmd, "--cores")) {
        MALI_KMD_POWER p; memset(&p, 0, sizeof(p)); p.State = MALI_POWER_ON;
        if (!DoIoctl(h, IOCTL_MALI_POWER, &p, sizeof(p), NULL, 0)) {
            Out("auto POWER ON failed: "); OutHex(GetLastError()); Out("\r\n");
        }
    }

    if (Has(cmd, "--off")) {
        MALI_KMD_POWER p; memset(&p, 0, sizeof(p)); p.State = MALI_POWER_OFF;
        if (DoIoctl(h, IOCTL_MALI_POWER, &p, sizeof(p), NULL, 0)) Out("POWER OFF ok\r\n");
        else { Out("POWER OFF failed: "); OutHex(GetLastError()); Out("\r\n"); }
    }

    if (Has(cmd, "--on")) {
        MALI_KMD_POWER p; memset(&p, 0, sizeof(p)); p.State = MALI_POWER_ON;
        if (DoIoctl(h, IOCTL_MALI_POWER, &p, sizeof(p), NULL, 0)) {
            Out("POWER ON ok\r\n");
            PrintCaps(h);   /* ID registers are only valid once powered */
        } else {
            Out("POWER failed: "); OutHex(GetLastError()); Out("\r\n");
        }
    }

    /*
     * --greset: GPU soft reset, exactly like kbase_pm_do_reset(). kbase does
     * this on every GPU power-up; the JM may not run without it.
     */
    if (Has(cmd, "--greset") || Has(cmd, "--ghard")) {
        unsigned cmdv = Has(cmd, "--ghard") ? 0x02u : 0x01u, i;
        unsigned raw = 0;
        WrReg(h, MALI_REG_BLOCK_GPU, 0x024, 0xFFFFFFFFu);        /* GPU_IRQ_CLEAR  */
        WrReg(h, MALI_REG_BLOCK_GPU, 0x028, 0x00000100u);        /* MASK: RESET_COMPLETED */
        WrReg(h, MALI_REG_BLOCK_GPU, 0x030, cmdv);               /* GPU_COMMAND    */
        for (i = 0; i < 200; i++) {
            raw = RdReg(h, MALI_REG_BLOCK_GPU, 0x020);
            if (raw & 0x00000100u) break;
            Sleep(1);
        }
        Out("gpu reset("); OutHex(cmdv); Out("): waited "); OutDec(i);
        Out(" ms  GPU_IRQ_RAWSTAT "); OutHex(raw); Out("\r\n");
        WrReg(h, MALI_REG_BLOCK_GPU, 0x024, 0xFFFFFFFFu);
    }

    /*
     * --cores: power up the shader / tiler / L2 stacks again. The GPU soft
     * reset clears PWRON, so this has to follow --greset, exactly like
     * kbase_pm_cores_enable() does after the reset in the power-up sequence.
     */
    if (Has(cmd, "--cores")) {
        unsigned sp = RdReg(h, MALI_REG_BLOCK_GPU, 0x100);
        unsigned tp = RdReg(h, MALI_REG_BLOCK_GPU, 0x110);
        unsigned l2 = RdReg(h, MALI_REG_BLOCK_GPU, 0x120);
        int r1, r2, r3;

        Out("cores: present S "); OutHex(sp);
        Out(" T "); OutHex(tp);
        Out(" L2 "); OutHex(l2); Out("\r\n");

        if (l2 != 0 && l2 != 0xFFFFFFFFu) WrReg(h, MALI_REG_BLOCK_GPU, 0x1A0, l2);
        if (tp != 0 && tp != 0xFFFFFFFFu) WrReg(h, MALI_REG_BLOCK_GPU, 0x190, tp);
        if (sp != 0 && sp != 0xFFFFFFFFu) WrReg(h, MALI_REG_BLOCK_GPU, 0x180, sp);

        r1 = WaitMask(h, 0x140, sp, sp, 200);
        r2 = WaitMask(h, 0x150, tp, tp, 200);
        r3 = WaitMask(h, 0x160, l2, l2, 200);
        Out("cores: ready S "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x140));
        Out(" T "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x150));
        Out(" L2 "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x160));
        Out("  wait S "); OutDec((unsigned)r1);
        Out(" T "); OutDec((unsigned)r2);
        Out(" L2 "); OutDec((unsigned)r3);
        Out("  pwrtrans "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x200));
        Out("\r\n");
    }

    if (Has(cmd, "--asdump")) {
        Out("MMU  RAWSTAT "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2000));
        Out("  MASK "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2008));
        Out("  STATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x200C));
        Out("  MMU_STATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2010));
        Out("  DTE "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2014));
        Out("\r\n");
        Out("AS0  TRANSTAB "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2404));
        Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2400));
        Out("  MEMATTR "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x240C));
        Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2408));
        Out("\r\n");
        Out("AS0  TRANSCFG "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2434));
        Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2430));
        Out("  STATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2428));
        Out("  FAULTSTATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x241C));
        Out("\r\n");
        Out("AS0  FAULTADDR "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2424));
        Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2420));
        Out("  FAULTEXTRA "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x243C));
        Out("\r\n");
    }

    if (Has(cmd, "--rd")) {
        const char *ap = Find(cmd, "--rd"); unsigned b = 0, o = 0;
        ap = ParseU(ap, &b); if (ap) ap = ParseU(ap, &o);
        if (ap) {
            MALI_KMD_REG_ACCESS ra; memset(&ra, 0, sizeof(ra)); ra.Block = b; ra.Offset = o;
            if (DoIoctl(h, IOCTL_MALI_REG_READ, &ra, sizeof(ra), &ra, sizeof(ra))) {
                Out("RD blk "); OutDec(b); Out(" +"); OutHex(o); Out(" = "); OutHex(ra.Value); Out("\r\n");
            } else { Out("REG_READ failed: "); OutHex(GetLastError()); Out("\r\n"); }
        } else { Out("usage: --rd <block> <offset>\r\n"); }
    }

    if (Has(cmd, "--wr")) {
        const char *ap = Find(cmd, "--wr"); unsigned b = 0, o = 0, v = 0;
        ap = ParseU(ap, &b); if (ap) ap = ParseU(ap, &o); if (ap) ap = ParseU(ap, &v);
        if (ap) {
            MALI_KMD_REG_ACCESS ra; memset(&ra, 0, sizeof(ra)); ra.Block = b; ra.Offset = o; ra.Value = v;
            if (DoIoctl(h, IOCTL_MALI_REG_WRITE, &ra, sizeof(ra), &ra, sizeof(ra))) {
                Out("WR blk "); OutDec(b); Out(" +"); OutHex(o); Out(" <- "); OutHex(v); Out("\r\n");
            } else { Out("REG_WRITE failed: "); OutHex(GetLastError()); Out("\r\n"); }
        } else { Out("usage: --wr <block> <offset> <value>\r\n"); }
    }

    if (Has(cmd, "--clkdump")) {
        static const struct { unsigned blk; unsigned ofs; const char *name; } tbl[] = {
            { MALI_REG_BLOCK_MFG_TOP,     0x000, "MFG_TOP   BG3D_GATE_STA         " },
            { MALI_REG_BLOCK_MFG_TOP,     0x004, "MFG_TOP   BG3D_GATE_SET         " },
            { MALI_REG_BLOCK_TOPCKGEN,    0x050, "TOPCKGEN  CLK_CFG_4 (bit18=mfg) " },
            { MALI_REG_BLOCK_TOPCKGEN,    0x004, "TOPCKGEN  CLK_CFG_UPDATE        " },
            { MALI_REG_BLOCK_APMIXED,     0x268, "APMIXED   MFGPLL_CON0           " },
            { MALI_REG_BLOCK_APMIXED,     0x26C, "APMIXED   MFGPLL_CON1 (pd/pcw)  " },
            { MALI_REG_BLOCK_APMIXED,     0x270, "APMIXED   MFGPLL_CON2           " },
            { MALI_REG_BLOCK_APMIXED,     0x274, "APMIXED   MFGPLL_CON3 (rstb)    " },
            { MALI_REG_BLOCK_INFRACFG_AO, 0x2A0, "ICFG_AO   PROTECTEN             " },
            { MALI_REG_BLOCK_INFRACFG_AO, 0x228, "ICFG_AO   PROTECTSTA1           " },
            { MALI_REG_BLOCK_INFRACFG_AO, 0x2A8, "ICFG_AO   PROTECTEN_1           " },
            { MALI_REG_BLOCK_INFRACFG_AO, 0x258, "ICFG_AO   PROTECTSTA1_1         " },
            { MALI_REG_BLOCK_INFRACFG_AO, 0x714, "ICFG_AO   PROTECTEN_2           " },
            { MALI_REG_BLOCK_INFRACFG_AO, 0x724, "ICFG_AO   PROTECTSTA1_2         " },
            { MALI_REG_BLOCK_BCRM,        0x04C, "BCRM      way_en                " },
            { MALI_REG_BLOCK_GPU,         0x000, "GPU       GPU_ID                " },
            { MALI_REG_BLOCK_GPU,         0x004, "GPU       L2_FEATURES           " },
            { MALI_REG_BLOCK_GPU,         0x008, "GPU       CORE_FEATURES         " },
            { MALI_REG_BLOCK_GPU,         0x00C, "GPU       TILER_FEATURES        " },
            { MALI_REG_BLOCK_GPU,         0x010, "GPU       MEM_FEATURES          " },
            { MALI_REG_BLOCK_GPU,         0x014, "GPU       MMU_FEATURES          " },
            { MALI_REG_BLOCK_GPU,         0x018, "GPU       AS_PRESENT            " },
            { MALI_REG_BLOCK_GPU,         0x01C, "GPU       JS_PRESENT            " },
            { MALI_REG_BLOCK_GPU,         0x0C0, "GPU       JS0_FEATURES          " },
            { MALI_REG_BLOCK_GPU,         0x0C4, "GPU       JS1_FEATURES          " },
            { MALI_REG_BLOCK_GPU,         0x0C8, "GPU       JS2_FEATURES          " },
            { MALI_REG_BLOCK_GPU,         0x100, "GPU       SHADER_PRESENT        " },
            { MALI_REG_BLOCK_GPU,         0x110, "GPU       TILER_PRESENT         " },
            { MALI_REG_BLOCK_GPU,         0x120, "GPU       L2_PRESENT            " },
            { MALI_REG_BLOCK_GPU,         0x1800, "GPU       JS0_SLOT base         " },
            { MALI_REG_BLOCK_GPU,         0x1824, "GPU       JS0 JS_STATUS         " },
            { MALI_REG_BLOCK_GPU,         0x1858, "GPU       JS0 JS_CONFIG_NEXT    " },
            { MALI_REG_BLOCK_GPU,         0x1860, "GPU       JS0 JS_COMMAND_NEXT   " },
        };
        unsigned k;
        Out("== clock / power state ==\r\n");
        for (k = 0; k < sizeof(tbl) / sizeof(tbl[0]); k++) {
            MALI_KMD_REG_ACCESS ra; memset(&ra, 0, sizeof(ra));
            ra.Block = tbl[k].blk; ra.Offset = tbl[k].ofs;
            Out(tbl[k].name); Out(" ");
            if (DoIoctl(h, IOCTL_MALI_REG_READ, &ra, sizeof(ra), &ra, sizeof(ra))) OutHex(ra.Value);
            else { Out("err "); OutHex(GetLastError()); }
            Out("\r\n");
        }
    }

    if (Has(cmd, "--bo")) {
        MALI_KMD_BO_ALLOC bo; memset(&bo, 0, sizeof(bo));

        if (DoIoctl(h, IOCTL_MALI_MMU_ENABLE, NULL, 0, NULL, 0)) Out("MMU enabled\r\n");
        else { Out("MMU_ENABLE failed: "); OutHex(GetLastError()); Out("\r\n"); }

        bo.Size  = 1024 * 1024;
        bo.Flags = MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD | MALI_BO_GPU_WR;
        if (DoIoctl(h, IOCTL_MALI_ALLOC_BO, &bo, sizeof(bo), &bo, sizeof(bo))) {
            Out("BO #"); OutDec(bo.Handle);
            Out("  size "); OutHex64(bo.Size);
            Out("  GpuVa "); OutHex64(bo.GpuVa);
            Out("  UserVa "); OutHex64(bo.UserVa); Out("\r\n");
            if (bo.UserVa != 0) {
                volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)bo.UserVa;
                int ok = 1;
                for (i = 0; i < 64; i++) p[i] = (unsigned char)i;
                for (i = 0; i < 64; i++) if (p[i] != (unsigned char)i) ok = 0;
                Out("BO CPU readback "); Out(ok ? "OK" : "MISMATCH"); Out("\r\n");
            }
            {
                MALI_KMD_BO_FREE fr; memset(&fr, 0, sizeof(fr)); fr.Handle = bo.Handle;
                if (DoIoctl(h, IOCTL_MALI_FREE_BO, &fr, sizeof(fr), NULL, 0)) Out("BO freed\r\n");
                else { Out("BO free failed: "); OutHex(GetLastError()); Out("\r\n"); }
            }
        } else {
            Out("ALLOC_BO failed: "); OutHex(GetLastError()); Out("\r\n");
        }
    }

    if (Has(cmd, "--jstatus") || Has(cmd, "--job")) {
        MALI_KMD_JOB_STATUS js; memset(&js, 0, sizeof(js));
        if (DoIoctl(h, IOCTL_MALI_JOB_STATUS, &js, sizeof(js), &js, sizeof(js))) {
            Out("JS slot 0: present "); OutHex(js.Present);
            Out(" slots "); OutDec(js.SlotCount);
            Out(" status "); OutHex(js.JsStatus);
            Out(" config "); OutHex(js.JsConfig);
            Out(" features "); OutHex(js.JsFeatures);
            Out(" head "); OutHex64(js.JsHead);
            Out(" tail "); OutHex64(js.JsTail); Out("\r\n");
        } else {
            Out("JOB_STATUS failed: "); OutHex(GetLastError()); Out("\r\n");
        }
    }

    if (Has(cmd, "--job")) {
        /*
         * Build the job chain ourselves in a BO and submit its GPU VA. The
         * KMD only programs the job slot, so the encoding lives here and can
         * be iterated without reloading the driver.
         *
         * JM "job descriptor header" (32 bytes), see panfrost mali_job_type:
         *   word 0 : exception_status
         *   word 1 : first_incomplete_task
         *   word 2 : fault_pointer lo
         *   word 3 : fault_pointer hi
         *   word 4 : [0]=job_descriptor_size(64-bit), [7:1]=job_type, [31:16]=job_index
         *   word 5 : dependency indices
         *   word 6 : next job lo
         *   word 7 : next job hi
         * A zero header is job_type 0 = JOB_NOT_STARTED, which is NOT runnable.
         */
        MALI_KMD_BO_ALLOC bo; memset(&bo, 0, sizeof(bo));
        const char *tp = Find(cmd, "--jtype");
        unsigned jobType = 1;   /* JOB_TYPE_NULL */
        if (tp) { const char *np = ParseU(tp, &jobType); (void)np; }

        if (Has(cmd, "--prestop")) {
            MALI_KMD_JOB_STOP st; memset(&st, 0, sizeof(st));
            st.Slot = 0; st.Hard = 1;
            if (DoIoctl(h, IOCTL_MALI_JOB_STOP, &st, sizeof(st), NULL, 0)) {
                Out("pre hard-stop ok\r\n");
            } else {
                Out("pre hard-stop failed: "); OutHex(GetLastError()); Out("\r\n");
            }
            {
                MALI_KMD_JOB_STATUS ds; memset(&ds, 0, sizeof(ds)); ds.Slot = 0;
                if (DoIoctl(h, IOCTL_MALI_JOB_STATUS, &ds, sizeof(ds), &ds, sizeof(ds))) {
                    Out("after stop: status "); OutHex(ds.JsStatus);
                    Out(" config "); OutHex(ds.JsConfig);
                    Out(" head "); OutHex64(ds.JsHead);
                    Out(" tail "); OutHex64(ds.JsTail); Out("\r\n");
                }
            }
            DumpJobRegs(h, "after stop");
        }

        if (DoIoctl(h, IOCTL_MALI_MMU_ENABLE, NULL, 0, NULL, 0)) Out("MMU enabled\r\n");
        else { Out("MMU_ENABLE failed: "); OutHex(GetLastError()); Out("\r\n"); }

        bo.Size  = 4096;
        bo.Flags = MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD;
        if (!DoIoctl(h, IOCTL_MALI_ALLOC_BO, &bo, sizeof(bo), &bo, sizeof(bo))) {
            Out("job BO alloc failed: "); OutHex(GetLastError()); Out("\r\n");
        } else {
            Out("chain BO #"); OutDec(bo.Handle);
            Out(" GpuVa "); OutHex64(bo.GpuVa);
            Out(" UserVa "); OutHex64(bo.UserVa); Out("\r\n");

            if (bo.UserVa != 0) {
                volatile unsigned int *w = (volatile unsigned int *)(uintptr_t)bo.UserVa;
                w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0;
                w[4] = (1u << 0) | ((jobType & 0x7Fu) << 1) | (1u << 16);
                w[5] = 0; w[6] = 0; w[7] = 0;
                Out("job_type "); OutDec(jobType);
                Out("  header word4 "); OutHex(w[4]); Out("\r\n");
            } else {
                Out("no UserVa; cannot build chain\r\n");
            }

            {
                MALI_KMD_JOB_SUBMIT s; memset(&s, 0, sizeof(s));
                s.Slot = 0;
                s.Flags = 0;
                s.Jc = bo.GpuVa;
                s.Affinity = 0;
                s.AsNumber = 0;
                s.TimeoutMs = 2000;
                if (Has(cmd, "--nommu")) s.Flags |= MALI_JOB_FLAG_NO_MMU;
                Out("Submitting job...\r\n");
                if (DoIoctl(h, IOCTL_MALI_JOB_SUBMIT, &s, sizeof(s), &s, sizeof(s))) {
                    Out("job done: JS_STATUS "); OutHex(s.JsStatus);
                    Out("  IRQ "); OutHex(s.IrqStat); Out("\r\n");
                } else {
                    Out("JOB_SUBMIT returned "); OutHex(GetLastError());
                    Out("  JS_STATUS "); OutHex(s.JsStatus);
                    Out("  IRQ "); OutHex(s.IrqStat); Out("\r\n");
                }
            }

            {
                MALI_KMD_JOB_STATUS ds; memset(&ds, 0, sizeof(ds)); ds.Slot = 0;
                if (DoIoctl(h, IOCTL_MALI_JOB_STATUS, &ds, sizeof(ds), &ds, sizeof(ds))) {
                    Out("post: status "); OutHex(ds.JsStatus);
                    Out(" config "); OutHex(ds.JsConfig);
                    Out(" head "); OutHex64(ds.JsHead);
                    Out(" tail "); OutHex64(ds.JsTail); Out("\r\n");
                }
            }
            DumpJobRegs(h, "after submit");

            {
                MALI_KMD_BO_FREE fr; memset(&fr, 0, sizeof(fr)); fr.Handle = bo.Handle;
                if (DoIoctl(h, IOCTL_MALI_FREE_BO, &fr, sizeof(fr), NULL, 0)) Out("chain BO freed\r\n");
                else { Out("BO free failed: "); OutHex(GetLastError()); Out("\r\n"); }
            }
        }
    }

    /*
     * --rawjob: identical to --job, but the slot registers are programmed
     * through raw REG_WRITE IOCTLs so JS_CONFIG can be varied freely. Lets us
     * bisect flush bits / descriptor write-back / MMU without a driver reload.
     */
    if (Has(cmd, "--rawjob")) {
        MALI_KMD_BO_ALLOC bo; memset(&bo, 0, sizeof(bo));
        const char *tp = Find(cmd, "--jtype");
        const char *cp = Find(cmd, "--cfg");
        const char *pp = Find(cmd, "--poll");
        const char *np = Find(cmd, "--next");
        const char *ip = Find(cmd, "--index");
        unsigned jobType = 1, cfg = 0x00008000u, pollMs = 500, index = 1;
        unsigned nextLo = 0, nextHi = 0, i;
        unsigned rawstat = 0, jsstat = 0;

        if (tp) ParseU(tp, &jobType);
        if (cp) ParseU(cp, &cfg);
        if (pp) ParseU(pp, &pollMs);
        if (ip) ParseU(ip, &index);
        if (np) { const char *q = ParseU(np, &nextLo); if (q) ParseU(q, &nextHi); }

        ULONG mmuOut[2] = {0, 0};
        if (DoIoctl(h, IOCTL_MALI_MMU_ENABLE, NULL, 0, mmuOut, sizeof(mmuOut))) {
            Out("MMU enabled (UsePageTables="); OutDec(mmuOut[0]);
            Out(", TranscfgLo=0x"); OutHex(mmuOut[1]); Out(")\r\n");
        } else { Out("MMU_ENABLE failed: "); OutHex(GetLastError()); Out("\r\n"); }

        /* Read back TRANSCFG immediately after enable to verify mode. */
        Out("TRANSCFG after enable "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2430)); Out("\r\n");

        bo.Size  = 4096;
        bo.Flags = MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD | MALI_BO_GPU_WR;
        if (!DoIoctl(h, IOCTL_MALI_ALLOC_BO, &bo, sizeof(bo), &bo, sizeof(bo))) {
            Out("rawjob BO alloc failed: "); OutHex(GetLastError()); Out("\r\n");
        } else {
            volatile unsigned *w = (volatile unsigned *)(uintptr_t)bo.UserVa;
            Out("rawjob BO #"); OutDec(bo.Handle);
            Out(" GpuVa "); OutHex64(bo.GpuVa); Out("\r\n");

            /* Optional address-mode override, applied after MMU_ENABLE so it
             * is not overwritten. 2 = IDENTITY (bypass page tables). */
            if (Has(cmd, "--adrmode")) {
                const char *mp = Find(cmd, "--adrmode");
                unsigned mode = 6;
                if (mp) ParseU(mp, &mode);
                WrReg(h, MALI_REG_BLOCK_GPU, 0x2430,
                      (mode & 0xFu) | (2u << 24) | (2u << 28) | (1u << 30));
                WrReg(h, MALI_REG_BLOCK_GPU, 0x2434, 0);
                WrReg(h, MALI_REG_BLOCK_GPU, 0x2418, 1u);
                Sleep(2);
                Out("adrmode "); OutDec(mode);
                Out("  AS_STATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2428));
                Out("  TRANSCFG "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2430));
                Out("\r\n");
            }

            if (Has(cmd, "--chain")) {
                /* --chain N: N WRITE_VALUE jobs linked through next_job, each
                 * zeroing its own 8-byte sentinel at GpuVa+0x800+k*8. Proves
                 * the JM walks the chain, not just the head descriptor.
                 *
                 * Knobs for bisecting the chain encoding:
                 *   --chainidx  B   first job_index (default 1)
                 *   --chain2type T  job type of every job after the first (2)
                 *   --chaingap  N   descriptor stride in bytes (default 128)
                 *   --chaindump     print each descriptor from the CPU view
                 *
                 * Hardware rule found on silicon (Mali-G57 tNAx r0p1): every
                 * descriptor in a chain must be 128-byte aligned. 128/256 work,
                 * 64/96/192/320 fail with JS_STATUS 0x58 (DATA_INVALID_FAULT).
                 * A 64-byte descriptor "size" is therefore not the chain stride.
                 */
                const char *chp = Find(cmd, "--chain");
                const char *c2p = Find(cmd, "--chain2type");
                const char *cip = Find(cmd, "--chainidx");
                const char *cgp = Find(cmd, "--chaingap");
                unsigned n = 4, k, t2 = 2, idxBase = 1, stride = 128;
                if (chp) ParseU(chp, &n);
                if (c2p) ParseU(c2p, &t2);
                if (cip) ParseU(cip, &idxBase);
                if (cgp) ParseU(cgp, &stride);
                if (n < 1) n = 1;
                if (n > 32) n = 32;
                if (stride < 128) stride = 128;
                stride = (stride + 127u) & ~127u;   /* next_job must be 128-aligned */
                if ((unsigned long long)n * stride > 0x7C0) n = 0x7C0 / stride;
                for (i = 0; i < 1024; i++) w[i] = 0;
                for (k = 0; k < n; k++) {
                    volatile unsigned *d = w + (k * (stride / 4));
                    unsigned long long nxt = bo.GpuVa + (unsigned long long)(k + 1) * stride;
                    unsigned long long tgt = bo.GpuVa + 0x800 + (unsigned long long)k * 8;
                    unsigned jt = (k == 0) ? 2u : t2;
                    d[4] = (1u << 0) | ((jt & 0x7Fu) << 1) | (((idxBase + k) & 0xFFFFu) << 16);
                    d[5] = 0;
                    d[6] = (k + 1 < n) ? (unsigned)(nxt & 0xFFFFFFFFu) : 0;
                    d[7] = (k + 1 < n) ? (unsigned)(nxt >> 32) : 0;
                    d[8] = (unsigned)(tgt & 0xFFFFFFFFu);
                    d[9] = (unsigned)(tgt >> 32);
                    d[10] = 3;      /* MALI_WRITE_VALUE_ZERO */
                    d[11] = 0; d[12] = 0; d[13] = 0;
                    *(volatile unsigned *)((volatile unsigned char *)w + 0x800 + k * 8) =
                        0xDEADBEEFu + k;
                }
                Out("chain: "); OutDec(n); Out(" jobs, type "); OutDec(t2);
                Out(" idxbase "); OutDec(idxBase);
                Out(" stride "); OutDec(stride);
                Out(", sentinels 0xDEADBEEF+n\r\n");
            } else {
                for (i = 0; i < 8; i++) w[i] = 0;
                w[4] = (1u << 0) | ((jobType & 0x7Fu) << 1) | ((index & 0xFFFFu) << 16);
                w[6] = nextLo; w[7] = nextHi;

                /* Optional WRITE_VALUE payload (job_type 2): header is 32 bytes,
                 * payload follows at +32. struct mali_payload_write_value:
                 *   u64 address; u32 value_descriptor; u32 reserved; u64 immediate;
                 * MALI_WRITE_VALUE_ZERO == 3. */
                if (Has(cmd, "--wv")) {
                    const char *dp = Find(cmd, "--wvdesc");
                    unsigned desc = 3;
                    unsigned long long tgt = bo.GpuVa + 0x100;
                    if (dp) ParseU(dp, &desc);
                    w[8]  = (unsigned)(tgt & 0xFFFFFFFFu);
                    w[9]  = (unsigned)(tgt >> 32);
                    w[10] = desc;
                    w[11] = 0;
                    w[12] = 0;
                    w[13] = 0;
                    *(volatile unsigned *)((volatile unsigned char *)w + 0x100) = 0xDEADBEEFu;
                    Out("write_value: addr "); OutHex64(tgt);
                    Out(" desc "); OutDec(desc);
                    Out(" sentinel "); OutHex(w[0x40]); Out("\r\n");
                }
            }

            Out("rawjob: type "); OutDec(jobType);
            Out(" index "); OutDec(index);
            Out(" cfg "); OutHex(cfg);
            Out(" hdr4 "); OutHex(w[4]); Out("\r\n");

            /* Fresh slot state: clear stale completion, enable both IRQ bits. */
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1004, 0x00010001u);
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1008,
                  RdReg(h, MALI_REG_BLOCK_GPU, 0x1008) | 0x00010001u);

            WrReg(h, MALI_REG_BLOCK_GPU, 0x1840, (unsigned)(bo.GpuVa & 0xFFFFFFFFu));
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1844, (unsigned)(bo.GpuVa >> 32));
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1850, RdReg(h, MALI_REG_BLOCK_GPU, 0x100));
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1854, 0);
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1858, cfg);
            WrReg(h, MALI_REG_BLOCK_GPU, 0x1860, 1u);   /* JS_COMMAND_START */

            for (i = 0; i <= pollMs; i++) {
                rawstat = RdReg(h, MALI_REG_BLOCK_GPU, 0x1000);
                if (rawstat & 0x00010001u) break;
                Sleep(1);
            }

            jsstat = RdReg(h, MALI_REG_BLOCK_GPU, 0x1824);
            Out("rawjob: waited "); OutDec(i); Out(" ms");
            Out("  RAWSTAT "); OutHex(rawstat);
            Out("  JS_STATUS "); OutHex(jsstat); Out("\r\n");
            Out("  HEAD "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x1800));
            Out("  TAIL "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x1808));
            Out("  CFG "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x1818));
            Out("  CMD_NEXT "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x1860));
            Out("  CFG_NEXT "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x1858)); Out("\r\n");
            Out("  hdr[0] "); OutHex(w[0]);
            Out("  hdr[1] "); OutHex(w[1]);
            Out("  hdr[2] "); OutHex(w[2]);
            Out("  hdr[3] "); OutHex(w[3]); Out("\r\n");
            Out("  SH_READY "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x140));
            Out("  T_READY "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x150));
            Out("  L2_READY "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x160));
            Out("  SH_PWRTRANS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x200));
            Out("\r\n  AS_STATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2428));
            Out("  GPU_IRQ_RAWSTAT "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x020));
            Out("  JS_STATE "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x1010));
            Out("\r\n");
            Out("  GPU_FAULTSTATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x03C));
            Out("  GPU_FAULTADDR "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x040));
            Out("  MMU_IRQ_RAWSTAT "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2000));
            Out("  AS_FAULTSTATUS "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x241C));
            Out("\r\n");
            Out("  AS_FAULTADDR "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2420));
            Out("  SH_PWRACTIVE "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x240));
            Out("  T_PWRACTIVE "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x250));
            Out("  L2_PWRACTIVE "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x260));
            Out("\r\n");
            Out("  TRANSTAB "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2404));
            Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2400));
            Out("  MEMATTR "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x240C));
            Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2408));
            Out("  TRANSCFG "); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2434));
            Out(":"); OutHex(RdReg(h, MALI_REG_BLOCK_GPU, 0x2430));
            Out("\r\n");
            if (Has(cmd, "--chain")) {
                const char *chp = Find(cmd, "--chain");
                const char *cgp2 = Find(cmd, "--chaingap");
                unsigned n = 4, k, ok = 0, stride2 = 128;
                if (chp) ParseU(chp, &n);
                if (cgp2) ParseU(cgp2, &stride2);
                if (n < 1) n = 1;
                if (n > 32) n = 32;
                if (stride2 < 128) stride2 = 128;
                stride2 = (stride2 + 127u) & ~127u;
                Out("  chain sentinels:");
                for (k = 0; k < n; k++) {
                    unsigned v = *(volatile unsigned *)((volatile unsigned char *)w + 0x800 + k * 8);
                    Out(" "); OutHex(v);
                    if (v == 0) ok++;
                }
                Out("\r\n  chain cleared "); OutDec(ok); Out("/"); OutDec(n); Out("\r\n");
                if (Has(cmd, "--chaindump")) {
                    for (k = 0; k < n; k++) {
                        volatile unsigned *d = w + (k * (stride2 / 4));
                        Out("  desc["); OutDec(k); Out("] w4 "); OutHex(d[4]);
                        Out(" w6 "); OutHex(d[6]);
                        Out(" w7 "); OutHex(d[7]);
                        Out(" tgt "); OutHex(d[8]);
                        Out(":"); OutHex(d[9]); Out("\r\n");
                    }
                }
            }
            if (Has(cmd, "--wv")) {
                Out("  write_value result "); OutHex(w[0x40]); Out("\r\n");
            }

            {
                MALI_KMD_BO_FREE fr; memset(&fr, 0, sizeof(fr)); fr.Handle = bo.Handle;
                if (DoIoctl(h, IOCTL_MALI_FREE_BO, &fr, sizeof(fr), NULL, 0)) Out("rawjob BO freed\r\n");
                else { Out("rawjob BO free failed: "); OutHex(GetLastError()); Out("\r\n"); }
            }
        }
    }

    if (Has(cmd, "--irq")) {
        MALI_KMD_IRQ_STATS is; memset(&is, 0, sizeof(is));
        if (DoIoctl(h, IOCTL_MALI_IRQ_STATS, NULL, 0, &is, sizeof(is))) {
            Out("IRQ lines "); OutDec(is.LinesConnected);
            Out("  counts GPU "); OutDec(is.LineCount[0]);
            Out(" MMU "); OutDec(is.LineCount[1]);
            Out(" JOB "); OutDec(is.LineCount[2]);
            Out(" EVENT "); OutDec(is.LineCount[3]);
            Out(" PWR "); OutDec(is.LineCount[4]); Out("\r\n");
            Out("  job done "); OutDec(is.JobDoneCount);
            Out(" error "); OutDec(is.JobErrorCount);
            Out("  gpu faults "); OutDec(is.GpuFaultCount);
            Out("  mmu faults "); OutDec(is.MmuFaultCount); Out("\r\n");
            if (is.MmuFaultCount) {
                Out("  last MMU fault status "); OutHex(is.LastMmuFaultStatus);
                Out(" addr "); OutHex64(is.LastMmuFaultAddress); Out("\r\n");
            }
            if (is.GpuFaultCount) {
                Out("  last GPU fault status "); OutHex(is.LastGpuFaultStatus);
                Out(" addr "); OutHex64(is.LastGpuFaultAddress); Out("\r\n");
            }
        } else {
            Out("IRQ_STATS failed: "); OutHex(GetLastError()); Out("\r\n");
        }
    }

    if (DoIoctl(h, IOCTL_MALI_GET_PWR_STATUS, NULL, 0, &pwr, sizeof(pwr))) {
        Out("PWR_STATUS "); OutHex(pwr.RawPwrStatus);
        Out(" (2nd "); OutHex(pwr.RawPwrStatus2nd);
        Out(")  mask "); OutHex(pwr.MfgMaskExpected);
        Out(" actual "); OutHex(pwr.MfgMaskActual);
        Out("  powered="); OutDec(pwr.Powered); Out("\r\n");
    }

    CloseHandle(h);
    return 0;
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)Run());
}
