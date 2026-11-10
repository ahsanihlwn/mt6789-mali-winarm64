/*
 * Mali-G57 (MT6789) accelerator node for the P13001L firmware.
 *
 * This file is the firmware-side half of windrv/ACPI-GPU-NODE.md. It is kept
 * here, not in edk2-UEFI-mtk, because that tree is read-only for this project:
 * copy it (or its generated MaliGpuAml.h) into
 *   Platform/Itel/P13001LPkg/Applications/MaliAcpi/
 * when the firmware change is approved.
 *
 * Contract, and where each value comes from:
 *   _HID "MTK06789"               windrv/kmd/MaliKmd.inf:35
 *   MMIO 0x13000000 / 0x4000      windrv/kmd/src/MaliSoc.c:29
 *   GSIV 428 / 429 / 430          MaliSoc.c:39 (DT GIC_SPI 396/397/398, +32)
 *   order GPU, MMU, JOB           windrv/kmd/src/MaliIrq.c:22-25
 *
 * Why _HID is MTK06789 and not MTKMALI: ACPI requires the suffix of a string
 * _HID (and _CID) to be all hex digits - iasl rejects "MTKMALI" with error
 * 6035, "_HID suffix must be all hex digits", and an edk2 build would stop
 * there. "MTK06789" satisfies the rule, and MaliKmd.inf already lists
 * ACPI\MTK06789, so the driver binds without any INF change. The INF's
 * ACPI\MTKMALI line is unreachable for the same reason and can be dropped
 * whenever the INF is next touched.
 *
 * The +32 is measured, not assumed: the MADT this platform builds has GICD
 * Global System Interrupt Base 0 (GICv3), and the UFS precedent on the same
 * board maps DT GIC_SPI 137 to GSIV 169.
 *
 * Deliberately absent:
 *   - _CCA: the KMD drives its own MMU; the UFS node's cache-coherency
 *     property does not apply to a non-coherent GPU.
 *   - _DOD / any display property: this is a non-display accelerator. Claiming
 *     display would pull the node into DWM paths that are not built yet.
 *   - _DSM: nothing negotiates power or DMA through ACPI yet.
 */
DefinitionBlock ("", "SSDT", 2, "P130  ", "MALIGPU", 0x00000001)
{
    Scope (\_SB)
    {
        Device (GPU0)
        {
            Name (_HID, "MTK06789")
            Name (_UID, Zero)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x13000000, 0x00004000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                {
                    428,    // GPU
                    429,    // MMU
                    430     // JOB
                }
            })
        }
    }
}
