# `windrv/acpi` - the firmware-side half of the GPU node

`windrv/ACPI-GPU-NODE.md` explains *why* the Mali-G57 needs an ACPI node and
what the KMD does with it. This directory holds the actual table, kept here
because `edk2-UEFI-mtk` is read-only for this project: the change is prepared,
verified and versioned in this repo, and copied into the firmware only when it
is approved.

| File | What it is |
| --- | --- |
| `MaliGpu.asl` | the SSDT source (`SSDT "MALIGPU"`, device `GPU0`) |
| `MaliGpu.aml` | its compiled AML, 120 bytes, iasl 20251212 |
| `MaliGpuAml.h` | the same bytes as `STATIC CONST UINT8 mMaliGpuAml[]` for edk2 |
| `gen-aml-header.py` | regenerates `MaliGpuAml.h` from the `.asl` |

## The contract

| Item | Value | Source of the value |
| --- | --- | --- |
| `_HID` | `MTK06789` | `windrv/kmd/MaliKmd.inf:35` |
| `_UID` / `_STA` | `0` / `0x0F` | new node |
| MMIO | `0x13000000`, `0x4000` | `windrv/kmd/src/MaliSoc.c:29` |
| Interrupts | `428, 429, 430` = GPU, MMU, JOB | `MaliSoc.c:39`, `MaliIrq.c:22-25` |
| Trigger | level, active-high, exclusive | `MaliIrq.c:8-13` |

The order of the three interrupts is part of the contract: the KMD wires line 0
to GPU, line 1 to MMU, line 2 to JOB, and Windows preserves the order of the
`Interrupt()` list.

The GSIV numbers are DT `GIC_SPI 396/397/398` plus 32. That offset is measured,
not assumed: the MADT this platform builds has a GICD entry with Global System
Interrupt Base `0` (GICv3, distributor `0x0C000000`, redistributor `0x0C040000`),
and the UFS precedent on the same board maps `GIC_SPI 137` to GSIV 169.

### Why `_HID` is `MTK06789` and not `MTKMALI`

ACPI requires the suffix of a string `_HID` (and `_CID`) to be all hex digits.
`MTKMALI` fails that rule - iasl reports

```
Error 6035 - _HID suffix must be all hex digits ^  (MALI)
```

and an edk2 build stops there. `MTK06789` satisfies the rule and is already a
hardware ID in `MaliKmd.inf`, so the driver binds with no INF change. The INF's
`ACPI\MTKMALI` line is unreachable for the same reason and can be dropped
whenever the INF is next touched.

Deliberately absent from the node: `_CCA` (the KMD drives its own MMU), `_DOD`
or any display property (this is a non-display accelerator), and `_DSM`
(nothing negotiates power or DMA through ACPI yet).

## Regenerating

With iasl available (`acpica-tools`, or the binary under the firmware build's
`HostTools/root/usr/bin/iasl`):

```
python gen-aml-header.py MaliGpu.asl --iasl /path/to/iasl --out MaliGpuAml.h
```

Without iasl, compile by hand and feed the result in:

```
iasl -p MaliGpu -ve -f MaliGpu.asl
python gen-aml-header.py MaliGpu.asl --aml MaliGpu.aml --out MaliGpuAml.h
```

The generator refuses to emit a header if the AML signature is not `SSDT`/`DSDT`
or if the header's `Length` field disagrees with the file size - the two
mistakes that produce a table Windows silently ignores. The emitted header
records the AML size and SHA-256 so a regeneration can be diffed meaningfully.

`MaliGpuAml.h` as committed:

```
120 bytes, sha256 292d6c0ecf832781b3067ce0f6c1e6a61ebddd220b194b535025503d348a1a37
```

## Verifying without a tablet

```
iasl -d MaliGpu.aml
```

must show `Signature "SSDT"`, `Length 0x78`, `OEM Table ID "MALIGPU"`,
`_HID "MTK06789"`, `Memory32Fixed (ReadWrite, 0x13000000, 0x00004000)` and an
`Interrupt` list of `0x1AC, 0x1AD, 0x1AE`. That is the check that matters
before any firmware is built, let alone flashed.

A byte-level cross-check against the node that already boots on this board: the
interrupt descriptor here is `89 0E 00 01 03 AC 01 00 00 AD 01 00 00 AE 01 00 00`
(flags `0x00`, table length `1`, count `3`), and `UfsAcpi`'s is
`89 06 00 01 01 A9 00 00 00` (flags `0x00`, table length `1`, count `1`). Same
encoding, only the count and the values differ. `0x00` is the ASL default set -
`ResourceConsumer, Level, ActiveHigh, Exclusive` - which is what `MaliIrq.c`
expects.

## Putting it into the firmware

Two routes, both described in `windrv/ACPI-GPU-NODE.md` §4:

1. **Opt-in app, like `UfsAcpi`** (preferred). Copy `MaliGpu.asl` and
   `MaliGpuAml.h` into
   `Platform/Itel/P13001LPkg/Applications/MaliAcpi/`, write a small
   `MaliAcpi.c` modelled on `UfsAcpi.c` (`LocateProtocol` on
   `gEfiAcpiTableProtocolGuid`, `InstallAcpiTable`, install a marker
   configuration table so it is idempotent), and drop the FDT gate: the vendor
   boot DT for P13001L has no Mali node, so there is nothing to validate
   against. This is the lowest-blast-radius route and can be disabled without
   touching any core table.
2. **From `PlatformTablesDxe`**. Add `mMaliGpuAml` to the `Tables[]`/`Sizes[]`
   arrays in `PlatformTablesDxe.c:84`. Simpler, but it puts the node in the
   path of every boot instead of making it opt-in.

Nothing here has been flashed. The firmware image that would carry it has not
been built, and the tablet has not been touched.
