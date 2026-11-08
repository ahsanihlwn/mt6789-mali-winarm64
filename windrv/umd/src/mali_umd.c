/*
 * mali_umd.c  -  Win32 implementation of the Mali UMD winsys (IOCTL ABI v2).
 *
 * CRT-free: only kernel32 and setupapi are imported, and no C runtime helper is
 * referenced, so this object links into a /nodefaultlib image (the ICD DLL and
 * the UMD test both build that way). Memory for the device object comes from the
 * process heap.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include "mali_umd.h"

struct _MALI_UMD_DEVICE {
    HANDLE Handle;
};

/* {7c1f9a20-6d2b-4f4a-9b0e-1a2b3c4d5e6f} */
static const GUID MaliUmdInterfaceGuid =
    { 0x7c1f9a20, 0x6d2b, 0x4f4a, { 0x9b, 0x0e, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f } };

#define MALI_UMD_PATH_CHARS 260

static void MaliUmdZero(void *p, SIZE_T n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

static void MaliUmdCopyW(PWSTR Dst, PCWSTR Src, ULONG Cap)
{
    ULONG i = 0;
    if (!Dst || Cap == 0) return;
    if (Src) {
        for (; i + 1 < Cap && Src[i]; i++) Dst[i] = Src[i];
    }
    Dst[i] = L'\0';
}

static PMALI_UMD_DEVICE MaliUmdWrap(HANDLE h)
{
    PMALI_UMD_DEVICE d;

    if (h == INVALID_HANDLE_VALUE) return NULL;

    d = (PMALI_UMD_DEVICE)HeapAlloc(GetProcessHeap(), 0, sizeof(*d));
    if (!d) {
        CloseHandle(h);
        return NULL;
    }
    d->Handle = h;
    return d;
}

PMALI_UMD_DEVICE MaliUmdOpenPath(PCWSTR DevicePath)
{
    HANDLE h;

    if (!DevicePath) return NULL;

    h = CreateFileW(DevicePath,
                    GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    return MaliUmdWrap(h);
}

PMALI_UMD_DEVICE MaliUmdOpen(void)
{
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifd;
    PMALI_UMD_DEVICE result = NULL;
    DWORD i, needed;

    devs = SetupDiGetClassDevsW(&MaliUmdInterfaceGuid, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return NULL;

    ifd.cbSize = sizeof(ifd);
    for (i = 0; SetupDiEnumDeviceInterfaces(devs, NULL, &MaliUmdInterfaceGuid, i, &ifd); i++) {
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;

        needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), 0, needed);
        if (!detail) break;
        detail->cbSize = sizeof(*detail);

        if (SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, needed, &needed, NULL)) {
            result = MaliUmdOpenPath(detail->DevicePath);
        }
        HeapFree(GetProcessHeap(), 0, detail);

        if (result) break;
    }

    SetupDiDestroyDeviceInfoList(devs);
    return result;
}

ULONG MaliUmdEnumerate(PWSTR Paths, ULONG Capacity)
{
    HDEVINFO devs;
    SP_DEVICE_INTERFACE_DATA ifd;
    ULONG count = 0;
    DWORD i, needed;

    if (!Paths || Capacity == 0) return 0;

    devs = SetupDiGetClassDevsW(&MaliUmdInterfaceGuid, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return 0;

    ifd.cbSize = sizeof(ifd);
    for (i = 0; count < Capacity &&
                SetupDiEnumDeviceInterfaces(devs, NULL, &MaliUmdInterfaceGuid, i, &ifd); i++) {
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;

        needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devs, &ifd, NULL, 0, &needed, NULL);
        if (needed == 0) continue;

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), 0, needed);
        if (!detail) break;
        detail->cbSize = sizeof(*detail);

        if (SetupDiGetDeviceInterfaceDetailW(devs, &ifd, detail, needed, &needed, NULL)) {
            MaliUmdCopyW(Paths + (count * MALI_UMD_PATH_CHARS),
                         detail->DevicePath, MALI_UMD_PATH_CHARS);
            count++;
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }

    SetupDiDestroyDeviceInfoList(devs);
    return count;
}

void MaliUmdClose(PMALI_UMD_DEVICE Dev)
{
    if (!Dev) return;
    if (Dev->Handle && Dev->Handle != INVALID_HANDLE_VALUE) {
        CloseHandle(Dev->Handle);
    }
    HeapFree(GetProcessHeap(), 0, Dev);
}

BOOL MaliUmdIoctl(PMALI_UMD_DEVICE Dev, DWORD Code,
                  const void *In, DWORD InLen, void *Out, DWORD OutLen)
{
    DWORD ret = 0;

    if (!Dev || !Dev->Handle) return FALSE;
    return DeviceIoControl(Dev->Handle, Code,
                           (LPVOID)In, InLen, Out, OutLen, &ret, NULL);
}

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */

BOOL MaliUmdGetVersion(PMALI_UMD_DEVICE Dev, MALI_KMD_VERSION *Version)
{
    MALI_KMD_VERSION v;
    MaliUmdZero(&v, sizeof(v));
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_GET_VERSION, NULL, 0, &v, sizeof(v))) return FALSE;
    *Version = v;
    return TRUE;
}

BOOL MaliUmdGetCaps(PMALI_UMD_DEVICE Dev, MALI_KMD_CAPS *Caps)
{
    MALI_KMD_CAPS c;
    MaliUmdZero(&c, sizeof(c));
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_GET_CAPS, NULL, 0, &c, sizeof(c))) return FALSE;
    *Caps = c;
    return TRUE;
}

BOOL MaliUmdGetIrqStats(PMALI_UMD_DEVICE Dev, MALI_KMD_IRQ_STATS *Stats)
{
    MALI_KMD_IRQ_STATS s;
    MaliUmdZero(&s, sizeof(s));
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_IRQ_STATS, NULL, 0, &s, sizeof(s))) return FALSE;
    *Stats = s;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Power / MMIO                                                        */
/* ------------------------------------------------------------------ */

BOOL MaliUmdPower(PMALI_UMD_DEVICE Dev, UINT32 State)
{
    MALI_KMD_POWER p;
    MaliUmdZero(&p, sizeof(p));
    p.State = State;
    return MaliUmdIoctl(Dev, IOCTL_MALI_POWER, &p, sizeof(p), &p, sizeof(p));
}

BOOL MaliUmdRegRead(PMALI_UMD_DEVICE Dev, UINT32 Block, UINT32 Offset, UINT32 *Value)
{
    MALI_KMD_REG_ACCESS ra;
    MaliUmdZero(&ra, sizeof(ra));
    ra.Block = Block;
    ra.Offset = Offset;
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_REG_READ, &ra, sizeof(ra), &ra, sizeof(ra))) return FALSE;
    *Value = ra.Value;
    return TRUE;
}

BOOL MaliUmdRegWrite(PMALI_UMD_DEVICE Dev, UINT32 Block, UINT32 Offset, UINT32 Value)
{
    MALI_KMD_REG_ACCESS ra;
    MaliUmdZero(&ra, sizeof(ra));
    ra.Block = Block;
    ra.Offset = Offset;
    ra.Value = Value;
    return MaliUmdIoctl(Dev, IOCTL_MALI_REG_WRITE, &ra, sizeof(ra), &ra, sizeof(ra));
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

BOOL MaliUmdBoAlloc(PMALI_UMD_DEVICE Dev, UINT64 Size, UINT32 Flags, MALI_UMD_BO *Bo)
{
    MALI_KMD_BO_ALLOC a;

    /* ABI v2 carries the size as a UINT32; the KMD additionally caps a single
     * contiguous BO at 256 MB. Reject rather than silently truncate. */
    if (Size == 0 || Size > 0xFFFFFFFFull) return FALSE;

    MaliUmdZero(&a, sizeof(a));
    a.Size  = (UINT32)Size;
    a.Flags = Flags;
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_ALLOC_BO, &a, sizeof(a), &a, sizeof(a))) return FALSE;

    Bo->Handle = a.Handle;
    Bo->Flags  = a.Flags;
    Bo->Size   = Size;
    Bo->GpuVa  = a.GpuVa;
    Bo->CpuVa  = (void *)(uintptr_t)a.UserVa;
    return TRUE;
}

BOOL MaliUmdBoInfo(PMALI_UMD_DEVICE Dev, UINT32 Handle, MALI_KMD_BO_INFO *Info)
{
    MALI_KMD_BO_INFO i;
    MaliUmdZero(&i, sizeof(i));
    i.Handle = Handle;
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_BO_INFO, &i, sizeof(i), &i, sizeof(i))) return FALSE;
    *Info = i;
    return TRUE;
}

BOOL MaliUmdBoFree(PMALI_UMD_DEVICE Dev, UINT32 Handle)
{
    MALI_KMD_BO_FREE f;
    MaliUmdZero(&f, sizeof(f));
    f.Handle = Handle;
    return MaliUmdIoctl(Dev, IOCTL_MALI_FREE_BO, &f, sizeof(f), &f, sizeof(f));
}

/* ------------------------------------------------------------------ */
/* MMU                                                                 */
/* ------------------------------------------------------------------ */

BOOL MaliUmdMmuEnable(PMALI_UMD_DEVICE Dev)
{
    return MaliUmdIoctl(Dev, IOCTL_MALI_MMU_ENABLE, NULL, 0, NULL, 0);
}

BOOL MaliUmdMmuDisable(PMALI_UMD_DEVICE Dev)
{
    return MaliUmdIoctl(Dev, IOCTL_MALI_MMU_DISABLE, NULL, 0, NULL, 0);
}

BOOL MaliUmdMmuFault(PMALI_UMD_DEVICE Dev, MALI_KMD_MMU_FAULT *Fault)
{
    MALI_KMD_MMU_FAULT f;
    MaliUmdZero(&f, sizeof(f));
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_MMU_FAULT, NULL, 0, &f, sizeof(f))) return FALSE;
    *Fault = f;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Jobs                                                                */
/* ------------------------------------------------------------------ */

BOOL MaliUmdJobSubmit(PMALI_UMD_DEVICE Dev, const MALI_UMD_SUBMIT *Submit,
                      UINT32 *JsStatus, UINT32 *IrqStat)
{
    MALI_KMD_JOB_SUBMIT j;
    MaliUmdZero(&j, sizeof(j));
    j.Slot      = Submit->Slot;
    j.Flags     = Submit->Flags;
    j.Jc        = Submit->Jc;
    j.Affinity  = Submit->Affinity;
    j.AsNumber  = Submit->AsNumber;
    j.TimeoutMs = Submit->TimeoutMs;

    if (!MaliUmdIoctl(Dev, IOCTL_MALI_JOB_SUBMIT, &j, sizeof(j), &j, sizeof(j))) return FALSE;

    if (JsStatus) *JsStatus = j.JsStatus;
    if (IrqStat)  *IrqStat  = j.IrqStat;
    return TRUE;
}

BOOL MaliUmdJobStatus(PMALI_UMD_DEVICE Dev, UINT32 Slot, MALI_KMD_JOB_STATUS *Status)
{
    MALI_KMD_JOB_STATUS s;
    MaliUmdZero(&s, sizeof(s));
    s.Slot = Slot;
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_JOB_STATUS, &s, sizeof(s), &s, sizeof(s))) return FALSE;
    *Status = s;
    return TRUE;
}

BOOL MaliUmdJobStop(PMALI_UMD_DEVICE Dev, UINT32 Slot, BOOL Hard)
{
    MALI_KMD_JOB_STOP s;
    MaliUmdZero(&s, sizeof(s));
    s.Slot = Slot;
    s.Hard = Hard ? 1u : 0u;
    return MaliUmdIoctl(Dev, IOCTL_MALI_JOB_STOP, &s, sizeof(s), &s, sizeof(s));
}

BOOL MaliUmdJobSelfTest(PMALI_UMD_DEVICE Dev, UINT32 Slot, UINT32 *JsStatus, UINT32 *IrqStat)
{
    MALI_KMD_JOB_SELFTEST s;
    MaliUmdZero(&s, sizeof(s));
    s.Slot = Slot;
    if (!MaliUmdIoctl(Dev, IOCTL_MALI_JOB_SELFTEST, &s, sizeof(s), &s, sizeof(s))) return FALSE;
    if (JsStatus) *JsStatus = s.JsStatus;
    if (IrqStat)  *IrqStat  = s.IrqStat;
    return TRUE;
}
