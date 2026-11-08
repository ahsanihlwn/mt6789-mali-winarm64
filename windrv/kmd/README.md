# MaliKmd — MediaTek Mali-G57 Windows kernel driver (Phase 2)

KMDF driver that owns the MediaTek Mali GPU block so a user-mode Vulkan ICD can
accelerate DirectX 12/11/9 via vkd3d-proton / DXVK. See `../ARCHITECTURE.md` for
the layer map, `../ROADMAP.md` for the phase plan, and `../REFERENCES.md` for the
external Mali/Vulkan references (fxlin/mali, ARM libGPULayers, PanVK, …).

This is an **accelerator / hardware-broker driver, not a display driver**: it
does not touch scanout or WDDM. Display is already handled by GOP / the Microsoft
Basic Display Adapter.

Targets **P13001L / MT6789 first**, but every register/clock/power value lives in
`src/MaliSoc.c`, so other MediaTek Valhall SoCs (MT6833/MT6853/MT6879/MT6895) only
need a new descriptor entry.

## Layout

```
kmd/
  include/
    mali_kmd_ioctl.h   shared IOCTL ABI (kernel + user mode)
    mtk_gpu_soc.h      per-SoC descriptor + power-domain types
    mali_mmu.h         GPU MMU (AArch64 4K) interface
    mali_mem.h         GPU buffer-object interface
    mali_job.h         JM job/interrupt register map + job API
    mali_irq.h         interrupt context + prototypes
  src/
    Driver.c           DriverEntry, PnP, device/interface, caller-context hook
    MaliIo.c           IOCTL dispatch (incl. caller-context BO alloc/free)
    MaliHw.c           MMIO map, SPM/MTCMOS power, core power, clock mux
    MaliMmu.c          AArch64 4K page tables, identity map, MMU enable/fault
    MaliMem.c          contiguous BO alloc, MDL user mapping, free
    MaliJob.c          job-slot programming, completion fence, null-job test
    MaliIrq.c          GPU/MMU/JOB interrupts, fault capture, statistics
    MaliSoc.c          SoC table (MT6789 fully populated)
    MaliKmd.h          private context + prototypes
  tools/
    malitest.c         user-mode bring-up probe
  MaliKmd.inf
  build.ps1
```

## What it does today

- Maps `0x13000000` (GPU), `0x13FBF000` (MFG_TOP), `0x1000C000` (APMIXED),
  `0x10006000` (SPM), `0x10000000` (TOPCKGEN), `0x10001000` (INFRACFG_AO) and
  `0x10215000` (BCRM) straight from the descriptor (GPU is not ACPI-described yet).
- Reads identification registers (`GPU_ID`, `CORE_FEATURES`, `MMU_FEATURES`,
  `SHADER_PRESENT`) **only when the SPM power-status registers already report the
  MFG domains on**. A powered-off Mali block can raise a bus error on read, so
  load-time identification is gated and a cold boot simply records zeroes.
- Powers the MFG0..MFG3 domains through the SPM MTCMOS registers on demand via
  IOCTL, following the vendor `mtk-scpsys.c` order exactly: assert
  `PWR_ON`/`PWR_ON_2ND` (bits 2/3), wait for **both** `PWR_STATUS` (0x16C) and
  `PWR_STATUS_2ND` (0x170), release `PWR_CLK_DIS`/`PWR_ISO`, set `PWR_RST_B`,
  then un-gate SRAM and wait for the ACK. Parents come up first, children first
  on power-down. Then the shader/tiler/L2 core stacks
  (PRESENT -> PWRON -> poll READY) are brought up.
- Builds a real **AArch64 4K GPU MMU** (4 level page tables) and enables it on
  AS0. Mapping is **identity** (`GPU VA == physical`), non-cached, so bringing up
  buffers needs no cache maintenance.
- Allocates **GPU buffer objects**: physically contiguous, zeroed, non-cached,
  identity-mapped into the MMU, mapped into the calling process, and freed with
  automatic unmapping.
- **Submits jobs**: userspace (the ICD) builds the job chain in a BO and passes
  its GPU VA; the KMD programs `JS_HEAD_NEXT`, `JS_AFFINITY_NEXT`,
  `JS_CONFIG_NEXT`, rings `JS_COMMAND_NEXT`, and can wait for completion either
  through the JOB interrupt or by polling `JOB_IRQ_RAWSTAT`.
- **Handles the GPU/MMU/JOB interrupt lines** when the platform assigns them:
  level-triggered, mask-in-ISR / clear-and-restore-in-DPC, with GPU fault and
  MMU fault capture plus counters.
- Exposes raw register read/write, power status, BO info, MMU fault, job
  submit/status/stop and interrupt statistics IOCTLs.
- Refuses power transitions on SoCs whose SPM offsets are not filled in (no
  blind poking of unknown registers).

## Not in this phase

- Real job-chain construction: the kernel does not build chains. The first real
  job awaits the Vulkan ICD (Phase 3); `IOCTL_MALI_JOB_SELFTEST` only exercises
  the path with an empty chain.
- Async completion objects (timelines/fences shared with userspace). Today a
  submit is either fire-and-forget or a synchronous wait.
- EVENT and PWR interrupt lines (in the vendor tree they belong to the MediaTek
  `ged` driver, not kbase).
- System SMMU/IOMMU (bypassed for bring-up; GPU-side MMU is used instead).
- ACPI publication of the GPU node (that lives in `edk2-UEFI-mtk`, Phase 1).
- The user-mode Vulkan ICD (Phase 3).

## Build (no WDK install required)

`build.ps1` uses **clang** (LLVM shipped with VS Build Tools) with
`--target=aarch64-pc-windows-msvc`, the **kernel WDK headers/libs** extracted
under a read-only cache, and the MSVC x64 `link.exe` for the final image. No
`cl.exe` and no MSVC ARM64 CRT are needed: the driver links only against
`ntoskrnl.lib`, `HAL`, `bufferoverflowfastfailk.lib` and the KMDF 1.33 stubs.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Release
# -> build\MaliKmd.sys, then build\package\{MaliKmd.sys,MaliKmd.cat,MaliKmd.inf}
```

Paths the script resolves (override with `-WdkRoot` / `-SdkRoot` if they move):

- WDK: `D:\PORT\iOS 27\edk2-UEFI-mtk\.cache\p13001l-port\windows-wdk\extracted\c`
- SDK: `C:\Program Files (x86)\Windows Kits\10`
- clang/link: VS Build Tools `VC\Tools\Llvm\bin` and `VC\Tools\MSVC\...\bin\Hostx64\x64`

The build signs the package with the local test certificate and runs `Inf2Cat`
(signability check) automatically.

`malitest.exe` is built separately, CRT-free, so it needs no C runtime on the
tablet:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-malitest.ps1
# -> build\malitest.exe  (ARM64, no CRT imports)
```

## Install / test (installed Windows, ARM64)

The tablet is reached over the USB CDC agent, which cannot use its `!PUT`
path on installed Windows (it is gated on `X:\P13Stage` + the MiniNT key), so
files are staged to `C:\P13Stage\MTKMALI` with base64 chunks over the command
channel and verified with SHA-256:

```powershell
python tools\bridge\p13upload.py --port COM8 --output out\upload `
  --mkdir C:\P13Stage\MTKMALI `
  --put windrv\kmd\build\package\MaliKmd.sys  C:\P13Stage\MTKMALI\MaliKmd.sys `
  --put windrv\kmd\build\package\MaliKmd.cat  C:\P13Stage\MTKMALI\MaliKmd.cat `
  --put windrv\kmd\build\package\MaliKmd.inf  C:\P13Stage\MTKMALI\MaliKmd.inf `
  --put windrv\kmd\build\package\P13Test.cer  C:\P13Stage\MTKMALI\P13Test.cer `
  --put windrv\kmd\build\malitest.exe         C:\P13Stage\MTKMALI\malitest.exe `
  --put windrv\kmd\build\devcon.exe           C:\P13Stage\MTKMALI\devcon.exe `
  --put windrv\kmd\build\device\INSTALL.CMD   C:\P13Stage\MTKMALI\INSTALL.CMD `
  --put windrv\kmd\build\device\TEST.CMD      C:\P13Stage\MTKMALI\TEST.CMD
```

For anything large (GPU-Z is ~11 MB) use the agent's `!PUT` path — 1536-byte
hex frames, no per-chunk process spawn. It is gated on the `MiniNT` key plus a
real `X:\P13Stage`; `--enable` satisfies both by mapping `X:` to `C:\P13Stage`
with `subst`:

```powershell
python tools\bridge\p13put.py --port COM8 --output out\put --enable `
  --put windrv\kmd\build\package\MaliKmd.sys MALIKMD.SYS `
  --put windrv\kmd\build\package\MaliKmd.cat MALIKMD.CAT `
  --put windrv\kmd\build\package\MaliKmd.inf MALIKMD.INF `
  --put windrv\kmd\build\package\P13Test.cer P13TEST.CER `
  --put windrv\kmd\build\malitest.exe        MALITEST.EXE `
  --put windrv\kmd\build\devcon.exe          DEVCON.EXE `
  --put windrv\kmd\build\device\INSTALL.CMD  INSTALL.CMD `
  --put windrv\kmd\build\device\TEST.CMD     TEST.CMD `
  --put "D:\Downloads\GPU-Z.2.68.0.exe"      GPUZ.EXE `
  --copy-to C:\P13Stage\MTKMALI
```

Files land in `C:\P13Stage\P13Stage\NAME`; `--copy-to` mirrors them into
`C:\P13Stage\MTKMALI`, where the scripts expect them. Remote names must match
`[A-Z0-9_]{1,8}\.[A-Z0-9]{1,3}` (so `GPU-Z.EXE` must be sent as `GPUZ.EXE`).
Add `--cleanup` to delete the `MiniNT` key and the `X:` mapping afterwards.

Then run `INSTALL.CMD` (read-only checks + install + read-only report) and,
once the device is started, `TEST.CMD` (power-on + MMU/BO + job):

```cmd
C:\P13Stage\MTKMALI\INSTALL.CMD
C:\P13Stage\MTKMALI\TEST.CMD
```

Manual equivalent:

```cmd
pnputil /add-driver MaliKmd.inf /install
devcon install MaliKmd.inf ROOT\MaliMtk   :: or: devcon update ... for an existing node
sc query MaliKmd
malitest.exe --irq        :: version, caps, SPM power status (read only)
malitest.exe --on         :: power the GPU domains
malitest.exe --bo         :: enable MMU + alloc/free a 1 MB BO
malitest.exe --jstatus    :: JS_PRESENT / JS_STATUS / JS_FEATURES for slot 0
malitest.exe --job        :: submit a null job through the full path
```

`--bo`, `--job` and `--jstatus` power the domains on demand; `--irq` and a bare
`malitest.exe` never touch power.

## Bring-up checklist (first hardware run)

1. `malitest.exe` → it prints the SPM power status. On a cold boot the MFG
   domains are usually **off**, so `GPU_ID` is `0x00000000` by design (the read
   is skipped). That is not a failure.
2. `malitest.exe --on` → expect `POWER ON ok` and `powered=1`, then
   `GPU_ID`, `CORE_FEATURES`, `MMU_FEATURES` and `SHADER_PRESENT` non-zero.
   A stuck `POWER ON` means an SPM domain never acked; compare `MaliHw.c`'s
   bit order against `mtk-scpsys.c` (`PWR_ON`=bit2, `PWR_ON_2ND`=bit3).
3. If `GPU_ID` still reads `0`/`0xFFFFFFFF` after a successful power-on, the MFG
   clock tree (TOPCKGEN `CLK_CFG_4` mux + APMIXED MFGPLL + MFG_TOP gates) is
   still gated. That is **Phase 2h** and is deliberately not poked yet.
4. `malitest.exe --bo` → expect `MMU enabled`, a `BO #N` with matching
   `GpuVa`/`UserVa`, and `BO CPU readback OK`.
5. `malitest.exe --jstatus` → expect a sane `JS_PRESENT` (e.g. `0x7` for three
   slots), `JS_STATUS 0x00`, and a non-zero `JS_FEATURES`.
6. `malitest.exe --job` → the null job should complete with `JS_STATUS 0x00`.
   `0x40` CONFIG_FAULT / `0x42` READ_FAULT means the chain header or the MMU
   translation is wrong; `0x41` POWER_FAULT means the core stacks are not up.
7. `malitest.exe --irq` → with an ACPI-published node expect `lines 3` and
   `JOB` counter increments. With 0 lines, completions still work through the
   poll fallback and the counters stay at zero.
8. If a domain ack times out, dump `PWR_STATUS` (0x16C) and `PWR_STATUS_2ND`
   (0x170) before changing anything. A domain is on only when **both** show its
   status bit; one alone means it is mid-transition.
9. If `SHADER_READY` never matches `SHADER_PRESENT`, the core power-on order in
   `MaliPowerCores()` (L2 -> tiler -> shader) is the thing to compare against
   `kbase_pm_driver.c` on that hardware.
10. If the MMU faults, read `IOCTL_MALI_MMU_FAULT` (status + address). A fault at
    the BO's `GpuVa` usually means the page-table walk or the PTE flags differ
    from what the hardware expects.

> Safety: power/clock writes only run when an IOCTL asks for them (including a
> job submit, which powers up on demand). Driver load itself performs reads only.
> BO pages and page tables are allocated non-cached on purpose for the first
> bring-up.