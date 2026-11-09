# ACPI GPU node + real interrupts — handoff

Status: **not started**. Everything on the Windows side is already in place; the
only missing piece lives in the UEFI firmware (edk2), outside this repo.

This document is written so someone else can pick it up and finish it without
re-deriving anything. Every value below is either measured on the tablet or read
out of source, and each claim names its file.

---

## 1. What to do, in one paragraph

Add an ACPI device node for the Mali-G57 (MT6789) GPU to the P13001L UEFI
firmware, with `_HID = "MTKMALI"`, a `Memory32Fixed` for `0x13000000`, and
**three level-triggered interrupt resources (GSIV 428, 429, 430)**. Nothing in
this repo (`MT6789__WINDRIVER`) needs to change. When the node boots, the
already-installed KMD stops polling and starts taking real GPU/MMU/JOB
interrupts.

---

## 2. Why (measured current state)

On the tablet today, the KMD is installed as a **root-enumerated** device:

```
ROOT\SYSTEM\0001      MaliKmd.sys   0.2.2.0   Started     (oem8.inf)
```

Because a `ROOT\` device has no resource list, the KMD sees **zero interrupt
lines** and falls back to polling:

```
IRQ lines 0  counts GPU 0 MMU 0 JOB 0 EVENT 0 PWR 0
JOB_IRQ_RAWSTAT is polled for completion
```

Source of that behaviour:

- `windrv/kmd/src/MaliIrq.c:1-13` — three lines (GPU, MMU, JOB) are expected, in
  that order, each with its own `WDFINTERRUPT` + DPC.
- `windrv/kmd/src/Driver.c:58-63` — interrupts are taken from
  `ResourcesTranslated` and initialised **last**; the comment states
  `Zero lines is a valid outcome (ROOT device without an ACPI node)`.
- `windrv/kmd/src/MaliSoc.c:38-40` — the SoC table already carries the GIC SPIs
  and states the mapping: `Windows GSI = SPI + 32 when published via MADT`.
- `windrv/kmd/MaliKmd.inf` — the driver already advertises
  `ACPI\MTKMALI` and `ACPI\MTK06789`, so **no INF change is needed**.

So the KMD is complete and waiting; the firmware is the only gap.

---

## 3. The contract

| Item | Value | Where it comes from |
| --- | --- | --- |
| `_HID` | `MTKMALI` (alternate accepted: `MTK06789`) | `windrv/kmd/MaliKmd.inf` |
| MMIO region | base `0x13000000`, size `0x4000` | `windrv/kmd/src/MaliSoc.c:29` |
| Interrupt 0 — GPU | DT `GIC_SPI 396` → **GSIV 428** | `MaliSoc.c:39` (`Irqs[0]`) |
| Interrupt 1 — MMU | DT `GIC_SPI 397` → **GSIV 429** | `MaliSoc.c:39` (`Irqs[1]`) |
| Interrupt 2 — JOB | DT `GIC_SPI 398` → **GSIV 430** | `MaliSoc.c:39` (`Irqs[2]`) |
| (reserved) EVENT / PWR | SPI 399 / 400 → GSIV 431 / 432 | `MaliSoc.c:39` (`Irqs[3..4]`) |
| Trigger | Level, ActiveHigh, Exclusive | `MaliIrq.c:8-13` (level-triggered) |

**The `+32` is proven on this exact platform, not assumed.** The UFS node was
brought up the same way and records the mapping explicitly:

- `edk2-UEFI-mtk/Platform/Itel/P13001LPkg/Applications/UfsAcpi/UfsIrqContract.c`
  — verifies DT `GIC_SPI 137` and returns `*Gsiv = 169` (137 + 32).
- The matching node is `UfsExperimental.asl` with `Interrupt (...) { 169 }`.

The order matters: the KMD wires line 0 = GPU, line 1 = MMU, line 2 = JOB
(`MaliIrq.c:22-25`). EVENT and PWR are declared in the SoC table but not wired,
so publishing only three interrupts is correct.

---

## 4. Where the firmware change goes (edk2-UEFI-mtk)

`edk2-UEFI-mtk` is **read-only**. Copy the relevant files into
`windrv/acpi/reference/` and do the work on the copies; treat the firmware repo
as reference material only.

Reference files (current shape):

| File | Role |
| --- | --- |
| `Platform/Itel/P13001LPkg/Drivers/PlatformTablesDxe/DSDT.asl` | DSDT source — **8 CPU nodes only, no GPU** |
| `Platform/Itel/P13001LPkg/Drivers/PlatformTablesDxe/CoreTables.h` | generated AML blobs; `mDsdt` is the compiled `DSDT.asl` |
| `Platform/Itel/P13001LPkg/Drivers/PlatformTablesDxe/PlatformTablesDxe.c` | installs `{mDsdt, mFadt, mMadt, mGtdt}` via `Acpi->InstallAcpiTable` |
| `Tools/generate_p13001l_core_tables.py` | generates `CoreTables.h` (`CoreTables.h:2`) |
| `Platform/Itel/P13001LPkg/Applications/UfsAcpi/*` | **working precedent**: SSDT + embedded AML + installer app |

### Recommended: new SSDT, modelled on UfsAcpi

This is the lowest-risk option — it mirrors a node that already boots on this
board, and it can be toggled off without touching the core DSDT.

`MaliExperimental.asl`:

```asl
DefinitionBlock ("", "SSDT", 2, "P130  ", "MALIGPU", 0x00000001)
{
    Scope (\_SB)
    {
        Device (GPU0)
        {
            Name (_HID, "MTKMALI")          // matches MaliKmd.inf
            Name (_UID, Zero)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x13000000, 0x00004000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive)
                { 428, 429, 430 }           // GPU, MMU, JOB  (SPI 396/397/398 + 32)
            })
        }
    }
}
```

Then follow the UFS pattern exactly:

1. Compile: `iasl MaliExperimental.asl` → `MaliExperimental.aml`.
2. Convert to a C array `MaliExperimentalAml.h` (same shape as
   `UfsExperimentalAml.h`).
3. Install it from a small UEFI app or from `PlatformTablesDxe` with
   `Acpi->InstallAcpiTable(Acpi, Copy, sizeof(Aml), &Key)` —
   see `UfsAcpi.c:49-55`.

Drop the FDT gate: the UFS app validates against the boot FDT
(`UfsAcpi.c:20-38`), but the vendor boot DT for P13001L has **no Mali node**, so
the values above are taken from the SoC table in this repo instead. Do not block
installation on an FDT lookup for `mali@...`.

### Alternative: extend the DSDT

Add the same `Device (GPU0)` block inside the existing `Scope (\_SB)` in
`DSDT.asl`, then regenerate `CoreTables.h` with
`Tools/generate_p13001l_core_tables.py`. Higher blast radius (it rewrites the
core table the platform already validates against), so the SSDT above is
preferred.

---

## 5. Windows side: why nothing else changes

- The KMD maps MMIO from its own SoC table, not from ACPI
  (`Driver.c:27-32`: *"Phase 2 maps straight from the SoC descriptor because the
  GPU is not described in ACPI yet"*). So `Memory32Fixed` is present for clean
  PnP binding; the driver does not depend on it.
- The only consumer of the resource list is `MaliIrqInitialize(Ctx,
  ResourcesTranslated)` (`Driver.c:63`). Publishing the three interrupts is
  sufficient; no INF or source edit.
- The KMD is an **accelerator / hardware broker**, not a display or WDDM driver
  (`MaliKmd.inf` header comment). This node does not change scan-out.

---

## 6. Build and flash (needs explicit approval)

These steps touch firmware and the tablet; do them only with the user's go-ahead,
and never flash from an unverified build.

1. Build the edk2 firmware image for P13001L (see the firmware repo's own
   `build.sh` / `Boot-Windows-P13001L.cmd`).
2. Confirm the table is present **before** flashing: dump the built ACPI set and
   check for `SSDT "MALIGPU"` with GSIV 428/429/430.
3. Flash, then boot Windows.

Because this changes boot firmware, keep a known-good image to restore.

---

## 7. Verification on the tablet

Bridge (USB serial, COM8):

```
python .\tools\bridge\p13.py --script <file> --wait 20 --ready-timeout 15 --timeout 240
```

Pass criteria, in order:

1. **Enumeration** — the device appears as
   `ACPI\MTKMALI\...` (not `ROOT\SYSTEM\0001`):
   ```
   pnputil /enum-devices /class System
   ```
2. **Driver binds** — `MaliKmd` service is `Running` against the ACPI node.
3. **Interrupts are live** — `malitest.exe` reports:
   - `IRQ lines 3` (was `0`),
   - the `MALI_CAP_IRQ_CONNECTED` flag set (`mali_kmd_ioctl.h:63`),
   - `counts GPU/MMU/JOB` non-zero and **incrementing** while jobs run.
4. **No regression** — `GPU_ID 0x90930010`, MMU enabled, BO alloc/readback OK,
   `JS_STATUS 0`, and job counters `done` rising / `error` staying `0`.

Before/after reference numbers today: `job done 0 error 0`, `IRQ lines 0`.

---

## 8. Risks and gotchas

- **Wrong GSI = interrupt storm.** The KMD is level-triggered; a GSI that maps
  to some other device would either spin or steal another driver's interrupt.
  `MaliIrq.c:125-132` mitigates this by checking the line's own `STATUS`
  register and returning `FALSE` when it reads 0, but the correct number is
  still mandatory. Confirm the MADT's GIC distributor `Global System Interrupt
  Base` is 0 (so SPI `n` → GSIV `n + 32`).
- **GSIV base assumption.** If the platform ever declares a non-zero GSI base,
  every value in §3 shifts by that base. Verify against the built MADT before
  flashing.
- **Order is part of the contract.** GPU, MMU, JOB — not sorted by number and not
  interchangeable. `MaliIrq.c:22-25`.
- **Do not mark it a display device.** No `_DOD`/display `_HID`; this is a
  non-display accelerator. Wrong class ties into GPU/DWM paths that are not
  built yet.
- **`_CCA` is optional here.** The KMD manages its own MMU; do not copy the UFS
  node's `_CCA` blindly.
- **This repo stays portable.** No host-specific paths in committed files; the
  firmware copy lives under `windrv/acpi/reference/` (git-ignored if it is
  large).

---

## 9. What this unlocks

- **Removes polling.** Real completions instead of `JOB_IRQ_RAWSTAT` spins.
- **Prerequisite for L3 (WDDM render-only).** A display/compute miniport needs an
  interrupt object; without an IRQ resource it cannot be created. L3 is what
  makes the GPU show up in Task Manager / Device Manager. Note L3 still also
  needs the WDDM miniport itself — see `WDDM-PATH.md`. This node is necessary,
  not sufficient.
- **Unblocks the MMU page-table work** (fault reporting via the MMU line)
  tracked in `GRAPHICS-STACK.md` and `ROADMAP.md`.

It does **not** by itself make shaders or DX12 run; that is still the Valhall v9
Vulkan backend question (`GRAPHICS-STACK.md` §2.2/§8).

---

## 10. Definition of done

- ACPI SSDT `MALIGPU` with `_HID MTKMALI`, `Memory32Fixed(0x13000000, 0x4000)`,
  and interrupts `{428, 429, 430}` (Level/ActiveHigh/Exclusive) builds into the
  firmware.
- On the tablet: `ACPI\MTKMALI` binds `MaliKmd`; `malitest` shows `IRQ lines 3`
  with the IRQ-connected capability and non-zero, incrementing per-line counts;
  no job errors.
- Firmware change and any copy of it documented under `windrv/acpi/` in this
  repo; `MT6789__WINDRIVER` source unchanged.
