/*
 * MaliIo.c  -  IOCTL dispatch for the Mali KMD.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include "MaliKmd.h"

static VOID MaliComplete(_In_ WDFREQUEST Request, _In_ NTSTATUS Status, _In_ size_t Info)
{
    WdfRequestCompleteWithInformation(Request, Status, Info);
}

static NTSTATUS MaliHandleGetVersion(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_VERSION *Out;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Out->AbiVersion  = MALI_KMD_ABI_VERSION;
    Out->DriverMajor = 0;
    Out->DriverMinor = 1;
    RtlStringCchCopyW(Out->BuildTag, RTL_NUMBER_OF(Out->BuildTag), L"mtk-mali-kmd/0.1");
    MaliComplete(Request, STATUS_SUCCESS, sizeof(*Out));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleGetCaps(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_CAPS *Out;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    RtlZeroMemory(Out, sizeof(*Out));
    Out->SocId          = Ctx->Soc->SocId;

    /* Product model lives in bits 16..31 of GPU_ID (kbase GPU_ID2 format). */
    Out->GpuModel = (Ctx->GpuIdRegister >> 16) & 0xFFFFu;
    if (Out->GpuModel == 0u || Out->GpuModel == 0xFFFFu) {
        Out->GpuModel = Ctx->Soc->GpuModel;   /* fallback when not yet powered */
    }

    Out->GpuIdRegister  = Ctx->GpuIdRegister;
    Out->CoreFeatures   = Ctx->CoreFeatures;
    Out->MmuFeatures    = Ctx->MmuFeatures;
    Out->ShaderPresent  = Ctx->ShaderPresent;
    Out->MaxFreqKhz     = Ctx->Soc->MaxFreqKhz;
    Out->IrqCount       = Ctx->Soc->IrqCount;

    if (Ctx->Base[MTK_REGION_GPU] != NULL) {
        Out->Flags |= MALI_CAP_MMIO_MAPPED;
    }
    if (Ctx->PowerState == MALI_POWER_ON) {
        Out->Flags |= MALI_CAP_POWER_ON;
    }
    if (Ctx->IrqConnected) {
        Out->Flags |= MALI_CAP_IRQ_CONNECTED;
    }
    if (Ctx->Mmu.L0Table != NULL) {
        Out->Flags |= MALI_CAP_MEM_ALLOC;
    }
    if (Ctx->Mmu.Enabled) {
        Out->Flags |= MALI_CAP_MMU;
    }
    Out->Flags |= MALI_CAP_JOBS;
    MaliComplete(Request, STATUS_SUCCESS, sizeof(*Out));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandlePower(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_POWER *In;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*In), (PVOID *)&In, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = (In->State == MALI_POWER_ON) ? MaliHwPowerOn(Ctx) : MaliHwPowerOff(Ctx);
    MaliComplete(Request, Status, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandlePwrStatus(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_PWR_STATUS *Out;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    MaliHwGetPwrStatus(Ctx, Out);
    MaliComplete(Request, STATUS_SUCCESS, sizeof(*Out));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleRegAccess(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request, _In_ BOOLEAN Write)
{
    MALI_KMD_REG_ACCESS *Buf;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (Write) {
        Status = MaliHwWriteReg(Ctx, Buf->Block, Buf->Offset, Buf->Value);
    } else {
        ULONG V = 0;
        Status = MaliHwReadReg(Ctx, Buf->Block, Buf->Offset, &V);
        if (NT_SUCCESS(Status)) {
            Buf->Value = V;
        }
    }
    MaliComplete(Request, Status, sizeof(*Buf));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleBoInfo(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_BO_INFO *Buf;
    size_t Len;
    PMALI_BO Bo = NULL;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = MaliMemFind(Ctx, Buf->Handle, &Bo);
    if (NT_SUCCESS(Status)) {
        Buf->Flags    = Bo->Flags;
        Buf->Size     = Bo->Size;
        Buf->GpuVa    = Bo->PhysBase;
        Buf->PhysBase = Bo->PhysBase;
        Buf->UserVa   = (UINT64)(ULONG_PTR)Bo->UserVa;
    }
    MaliComplete(Request, Status, NT_SUCCESS(Status) ? sizeof(*Buf) : 0);
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleMmuEnable(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    NTSTATUS Status;
    ULONG TranscfgLo = 0, TranscfgHi = 0;
    ULONG OutData[2] = {0, 0};
    PULONG Out = NULL;
    size_t OutLen = 0;

    /* The GPU must be powered before the MMU can be programmed. */
    Status = MaliHwPowerOn(Ctx);
    if (NT_SUCCESS(Status)) {
        Status = MaliMmuEnable(Ctx);
        /* Read back TRANSCFG to verify mode. */
        MaliHwReadReg(Ctx, MTK_REGION_GPU, 0x2430, &TranscfgLo);
        MaliHwReadReg(Ctx, MTK_REGION_GPU, 0x2434, &TranscfgHi);
    }
    OutData[0] = Ctx->Mmu.UsePageTables;
    OutData[1] = TranscfgLo;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: MMU_ENABLE IOCTL - Status=0x%08X UsePageTables=%d TranscfgLo=0x%08X\n",
               Status, Ctx->Mmu.UsePageTables, TranscfgLo);
    /*
     * METHOD_BUFFERED shares one system buffer between input and output; copy
     * the result out explicitly (this used to report the length only, so the
     * caller always read zeros and had to fall back to a raw TRANSCFG read).
     */
    if (NT_SUCCESS(Status) &&
        NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(OutData), (PVOID *)&Out, &OutLen))) {
        Out[0] = OutData[0];
        Out[1] = OutData[1];
    }
    MaliComplete(Request, Status, NT_SUCCESS(Status) ? sizeof(OutData) : 0);
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleMmuDisable(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    NTSTATUS Status = MaliMmuDisable(Ctx);
    MaliComplete(Request, Status, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleMmuFault(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_MMU_FAULT *Out;
    size_t Len;
    ULONG Fs = 0;
    UINT64 Addr = 0;
    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    MaliMmuGetFault(Ctx, &Fs, &Addr);
    Out->Status  = Fs;
    Out->Address = Addr;
    MaliComplete(Request, STATUS_SUCCESS, sizeof(*Out));
    return STATUS_SUCCESS;
}

/* ---- job submission ---------------------------------------------- */

static NTSTATUS MaliHandleJobSubmit(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_JOB_SUBMIT *Buf;
    size_t Len;
    NTSTATUS Status;
    ULONG Stat = 0, Irq = 0;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MaliJobSubmit(Ctx, Buf->Slot, Buf->Jc, Buf->Affinity, Buf->AsNumber,
                           Buf->Flags, Buf->TimeoutMs, &Stat, &Irq);
    Buf->JsStatus = Stat;
    Buf->IrqStat  = Irq;

    /*
     * Report the hardware detail even on a timeout or a slot error: the caller
     * wants JS_STATUS, and STATUS_IO_TIMEOUT / STATUS_DEVICE_HARDWARE_ERROR
     * both still carry a full output buffer.
     */
    MaliComplete(Request, Status, sizeof(*Buf));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleJobStatus(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_JOB_STATUS *Buf;
    size_t Len;
    NTSTATUS Status;
    ULONG St = 0, Cfg = 0, Present = 0, Feat = 0;
    UINT64 Head = 0, Tail = 0;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MaliJobStatus(Ctx, Buf->Slot, &St, &Cfg, &Present, &Head, &Tail, &Feat);
    if (NT_SUCCESS(Status)) {
        Buf->Present    = Present;
        Buf->JsStatus   = St;
        Buf->JsConfig   = Cfg;
        Buf->JsFeatures = Feat;
        Buf->SlotCount  = Ctx->Job.SlotCount;
        Buf->JsHead     = Head;
        Buf->JsTail     = Tail;
    }
    MaliComplete(Request, Status, NT_SUCCESS(Status) ? sizeof(*Buf) : 0);
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleJobStop(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_JOB_STOP *Buf;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = MaliJobStop(Ctx, Buf->Slot, (BOOLEAN)(Buf->Hard != 0));
    MaliComplete(Request, Status, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleJobSelfTest(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_JOB_SELFTEST *Buf;
    size_t Len;
    NTSTATUS Status;
    ULONG Stat = 0, Irq = 0;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MaliJobNullTest(Ctx, Buf->Slot, &Stat, &Irq);
    Buf->JsStatus = Stat;
    Buf->IrqStat  = Irq;
    MaliComplete(Request, Status, sizeof(*Buf));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandleIrqStats(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_IRQ_STATS *Out;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Out), (PVOID *)&Out, &Len);
    ULONG i;

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    RtlZeroMemory(Out, sizeof(*Out));
    Out->LinesConnected = Ctx->Irq.LineCount;
    for (i = 0; i < MALI_IRQ_LINE_COUNT; i++) {
        Out->LineCount[i] = Ctx->Irq.Count[i];
    }
    Out->GpuFaultCount       = Ctx->Irq.GpuFaultCount;
    Out->MmuFaultCount       = Ctx->Irq.MmuFaultCount;
    Out->JobDoneCount        = Ctx->Irq.JobDoneCount;
    Out->JobErrorCount       = Ctx->Irq.JobErrorCount;
    Out->LastGpuFaultStatus  = Ctx->Irq.LastGpuFaultStatus;
    Out->LastMmuFaultStatus  = Ctx->Irq.LastMmuFaultStatus;
    Out->LastGpuFaultAddress = Ctx->Irq.LastGpuFaultAddress;
    Out->LastMmuFaultAddress = Ctx->Irq.LastMmuFaultAddress;
    MaliComplete(Request, STATUS_SUCCESS, sizeof(*Out));
    return STATUS_SUCCESS;
}

static NTSTATUS MaliHandlePtDump(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_PT_DUMP *Buf;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = MaliMmuPtDump(Ctx, Buf->TablePa, Buf->Index, Buf->Count, Buf->Entries);
    MaliComplete(Request, Status, NT_SUCCESS(Status) ? sizeof(*Buf) : 0);
    return STATUS_SUCCESS;
}

/* ---- caller-context BO handling ---------------------------------- */

static NTSTATUS MaliBoAllocInContext(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_BO_ALLOC *Buf;
    size_t Len;
    PMALI_BO Bo = NULL;
    PVOID UserVa = NULL;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MaliMemAlloc(Ctx, Buf->Size, Buf->Flags, &Bo);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MaliMemMapUser(Ctx, Bo, &UserVa);
    if (!NT_SUCCESS(Status)) {
        MaliMemFree(Ctx, Bo->Handle);
        return Status;
    }

    Buf->Handle = Bo->Handle;
    Buf->GpuVa  = Bo->PhysBase;
    Buf->UserVa = (UINT64)(ULONG_PTR)UserVa;
    return STATUS_SUCCESS;
}

static NTSTATUS MaliBoFreeInContext(_In_ PMALI_KMD_DEVICE Ctx, _In_ WDFREQUEST Request)
{
    MALI_KMD_BO_FREE *Buf;
    size_t Len;
    NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Buf), (PVOID *)&Buf, &Len);

    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return MaliMemFree(Ctx, Buf->Handle);
}

VOID MaliHandleBoInCallerContext(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PMALI_KMD_DEVICE Ctx = MaliKmdGetDeviceContext(Device);
    WDF_REQUEST_PARAMETERS Params;
    NTSTATUS Status;

    WDF_REQUEST_PARAMETERS_INIT(&Params);
    WdfRequestGetParameters(Request, &Params);

    if (Params.Parameters.DeviceIoControl.IoControlCode == IOCTL_MALI_ALLOC_BO) {
        size_t Info = sizeof(MALI_KMD_BO_ALLOC);
        Status = MaliBoAllocInContext(Ctx, Request);
        WdfRequestCompleteWithInformation(Request, Status, NT_SUCCESS(Status) ? Info : 0);
    } else if (Params.Parameters.DeviceIoControl.IoControlCode == IOCTL_MALI_FREE_BO) {
        Status = MaliBoFreeInContext(Ctx, Request);
        WdfRequestComplete(Request, Status);
    } else {
        WdfDeviceEnqueueRequest(Device, Request);
    }
}

VOID MaliEvtIoInCallerContext(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    WDF_REQUEST_PARAMETERS Params;

    WDF_REQUEST_PARAMETERS_INIT(&Params);
    WdfRequestGetParameters(Request, &Params);

    if (Params.Type == WdfRequestTypeDeviceControl &&
        (Params.Parameters.DeviceIoControl.IoControlCode == IOCTL_MALI_ALLOC_BO ||
         Params.Parameters.DeviceIoControl.IoControlCode == IOCTL_MALI_FREE_BO)) {
        MaliHandleBoInCallerContext(Device, Request);
        return;
    }
    WdfDeviceEnqueueRequest(Device, Request);
}

VOID MaliEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PMALI_KMD_DEVICE Ctx = MaliKmdGetDeviceContext(Device);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_MALI_GET_VERSION:
        Status = MaliHandleGetVersion(Ctx, Request);
        break;
    case IOCTL_MALI_GET_CAPS:
        Status = MaliHandleGetCaps(Ctx, Request);
        break;
    case IOCTL_MALI_POWER:
        Status = MaliHandlePower(Ctx, Request);
        break;
    case IOCTL_MALI_GET_PWR_STATUS:
        Status = MaliHandlePwrStatus(Ctx, Request);
        break;
    case IOCTL_MALI_REG_READ:
        Status = MaliHandleRegAccess(Ctx, Request, FALSE);
        break;
    case IOCTL_MALI_REG_WRITE:
        Status = MaliHandleRegAccess(Ctx, Request, TRUE);
        break;
    case IOCTL_MALI_BO_INFO:
        Status = MaliHandleBoInfo(Ctx, Request);
        break;
    case IOCTL_MALI_MMU_ENABLE:
        Status = MaliHandleMmuEnable(Ctx, Request);
        break;
    case IOCTL_MALI_MMU_DISABLE:
        Status = MaliHandleMmuDisable(Ctx, Request);
        break;
    case IOCTL_MALI_MMU_FAULT:
        Status = MaliHandleMmuFault(Ctx, Request);
        break;
    case IOCTL_MALI_JOB_SUBMIT:
        Status = MaliHandleJobSubmit(Ctx, Request);
        break;
    case IOCTL_MALI_JOB_STATUS:
        Status = MaliHandleJobStatus(Ctx, Request);
        break;
    case IOCTL_MALI_JOB_STOP:
        Status = MaliHandleJobStop(Ctx, Request);
        break;
    case IOCTL_MALI_JOB_SELFTEST:
        Status = MaliHandleJobSelfTest(Ctx, Request);
        break;
    case IOCTL_MALI_IRQ_STATS:
        Status = MaliHandleIrqStats(Ctx, Request);
        break;
    case IOCTL_MALI_PT_DUMP:
        Status = MaliHandlePtDump(Ctx, Request);
        break;
    default:
        break;
    }

    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
    }
}

VOID MaliEvtIoCanceledOnQueue(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(Queue);
    WdfRequestComplete(Request, STATUS_CANCELLED);
}

NTSTATUS MaliIoCreateQueues(_In_ WDFDEVICE Device)
{
    WDFQUEUE Queue;
    WDF_IO_QUEUE_CONFIG Config;
    NTSTATUS Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Config, WdfIoQueueDispatchParallel);
    Config.EvtIoDeviceControl = MaliEvtIoDeviceControl;
    Config.EvtIoCanceledOnQueue = MaliEvtIoCanceledOnQueue;

    Status = WdfIoQueueCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &Queue);
    return Status;
}