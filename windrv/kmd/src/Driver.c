/*
 * Driver.c  -  KMDF entry points for the MediaTek Mali KMD.
 *
 * SPDX-License-Identifier: MIT
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include "MaliKmd.h"

/* {7c1f9a20-6d2b-4f4a-9b0e-1a2b3c4d5e6f} */
DEFINE_GUID(GUID_DEVINTERFACE_MALI_KMD,
    0x7c1f9a20, 0x6d2b, 0x4f4a, 0x9b, 0x0e, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f);

DRIVER_INITIALIZE DriverEntry;

static NTSTATUS
MaliEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PMALI_KMD_DEVICE Ctx = MaliKmdGetDeviceContext(Device);
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(ResourcesRaw);

    /*
     * Phase 2 maps straight from the SoC descriptor because the GPU is not
     * described in ACPI yet. When ACPI publishes the device, parse
     * ResourcesTranslated here and prefer those addresses/GSIs.
     */
    Status = MaliHwInitialize(Ctx);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = MaliMemInitialize(Ctx);
    if (!NT_SUCCESS(Status)) {
        MaliHwCleanup(Ctx);
        return Status;
    }

    Status = MaliMmuInitialize(Ctx);
    if (!NT_SUCCESS(Status)) {
        MaliMemCleanup(Ctx);
        MaliHwCleanup(Ctx);
        return Status;
    }

    Status = MaliJobInitialize(Ctx);
    if (!NT_SUCCESS(Status)) {
        MaliMmuCleanup(Ctx);
        MaliMemCleanup(Ctx);
        MaliHwCleanup(Ctx);
        return Status;
    }

    /*
     * Interrupts come from the translated resource list, so this must be the
     * last step. Zero lines is a valid outcome (ROOT device without an ACPI
     * node); job completion then falls back to polling.
     */
    Status = MaliIrqInitialize(Ctx, ResourcesTranslated);
    if (!NT_SUCCESS(Status)) {
        MaliJobCleanup(Ctx);
        MaliMmuCleanup(Ctx);
        MaliMemCleanup(Ctx);
        MaliHwCleanup(Ctx);
        return Status;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
MaliEvtDeviceReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PMALI_KMD_DEVICE Ctx = MaliKmdGetDeviceContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    MaliIrqCleanup(Ctx);     /* deletes the interrupt objects first */
    MaliJobCleanup(Ctx);
    MaliHwPowerOff(Ctx);
    MaliMemCleanup(Ctx);     /* frees BOs and unmaps them from the GPU MMU */
    MaliMmuCleanup(Ctx);     /* frees the page tables */
    MaliHwCleanup(Ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS
MaliEvtDriverDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS Status;
    WDFDEVICE Device;
    PMALI_KMD_DEVICE Ctx;
    WDF_OBJECT_ATTRIBUTES Attrs;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, MaliEvtIoInCallerContext);
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = MaliEvtDevicePrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = MaliEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attrs, MALI_KMD_DEVICE);
    Attrs.EvtCleanupCallback = NULL;

    Status = WdfDeviceCreate(&DeviceInit, &Attrs, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Ctx = MaliKmdGetDeviceContext(Device);
    Ctx->Device = Device;

    /*
     * Pick the SoC. For bring-up we default to MT6789; a real build selects
     * this from the ACPI _DSD / compatible ID once the device node exists.
     */
    Ctx->Soc = MtkGpuGetSocDescById(MALI_SOC_MT6789);
    if (Ctx->Soc == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Ctx->PowerState = MALI_POWER_OFF;

    Status = MaliIoCreateQueues(Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_MALI_KMD, NULL);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    /* Bring-up is driven explicitly via IOCTL; no auto idle/power policy. */

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
               "MaliKmd: device added for %s\n", Ctx->Soc->Name);
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;
    WDFDRIVER Driver;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "MaliKmd: DriverEntry\n");

    WDF_DRIVER_CONFIG_INIT(&Config, MaliEvtDriverDeviceAdd);

    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                             &Config, &Driver);
    if (!NT_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "MaliKmd: WdfDriverCreate failed 0x%08X\n", Status);
        return Status;
    }
    return STATUS_SUCCESS;
}