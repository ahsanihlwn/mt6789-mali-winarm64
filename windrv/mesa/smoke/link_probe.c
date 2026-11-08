/*
 * link_probe.c  -  prove that an ARM64 C executable can be linked on a host
 *                  that has the Windows SDK but no MSVC ARM64 CRT.
 *
 * tools/link-probe.ps1 compiles this for aarch64-pc-windows-msvc and links it
 * with /nodefaultlib against only what the SDK ships:
 *
 *     ucrt.lib             dynamic UCRT (ucrtbase.dll is on Windows 11 ARM64)
 *     BufferOverflow.lib   __security_cookie / __security_check_cookie
 *     kernel32.lib
 *     ntdll.lib
 *
 * It deliberately exercises the things that pull in CRT pieces: printf/malloc,
 * floating point (which would need _fltused with a static CRT) and a >4 KiB
 * stack frame (which can need __chkstk). If this links, the recipe is sound.
 *
 * The output is never run here: the only ARM64 machine is the tablet. Linking
 * is the thing being proved.
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern int cpp_probe_value(void);   /* link_probe.cpp, linked into the same exe */

static double big_frame(double x)
{
    volatile unsigned char pad[16384];   /* forces a stack probe */
    memset((void *)pad, 0, sizeof(pad));
    return x + pad[0];
}

void mainCRTStartup(void)
{
    char *heap = (char *)malloc(128);
    double r = sqrt(2.0) + big_frame(1.5) + (double)cpp_probe_value();
    if (heap) {
        snprintf(heap, 128, "arm64 link probe: r=%.6f", r);
        printf("%s len=%d\n", heap, (int)strlen(heap));
    }
    fflush(stdout);
    free(heap);
    ExitProcess(0);
}
