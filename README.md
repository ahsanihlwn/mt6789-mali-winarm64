# Mali-G57 (MT6789) GPU driver for Windows on ARM64

An experimental, from-scratch **kernel-mode driver for the ARM Mali-G57 MC2**
found in MediaTek MT6789 (Helio G99 / itel VistaTab 30 Pro P13001L and similar)
running **Windows 11 on ARM64**.

The goal is to make the GPU usable as a compute/render accelerator on a device
whose firmware only ever described it to Android. Display output is explicitly
*out of scope*: scanout stays on GOP / Microsoft Basic Display Adapter, and the
driver only ever owns the GPU core itself.

> **Status: early bring-up.** The driver builds, installs, powers the GPU on and
> executes jobs on real silicon. There is **no Vulkan ICD or D3D client yet**, so
> Windows does not yet expose the GPU to applications.

---

## What works today (validated on hardware)

| Capability | State |
| --- | --- |
| Build `MaliKmd.sys` (ARM64, KMDF) with clang | ✅ |
| Test-sign, install as `ROOT\SYSTEM\0001`, start | ✅ |
| MMIO mapping of `mali@13000000` | ✅ |
| GPU identification (`GPU_ID 0x90930010`, tNAx r0p1, Valhall) | ✅ |
| MTCMOS power-on, GPU soft reset, core power stacks | ✅ |
| MMU enable (identity address mode) | ✅ |
| Buffer object allocation + CPU readback | ✅ |
| NULL job submission, completion | ✅ |
| WRITE_VALUE job (GPU writes to memory) | ✅ |
| Job chains (multiple linked jobs) | ❌ `DATA_INVALID_FAULT` |
| Interrupt delivery | ⚠️ polling fallback (no ACPI IRQ resource yet) |
| Vulkan ICD (Mesa PanVK port) | ❌ not started |
| D3D12 / DXVK / vkd3d-proton path | ❌ depends on the ICD |

## Architecture

```
   D3D12 application
        │
        ▼
   vkd3d-proton  ──►  DXVK  ──►  Vulkan ICD  (Mesa PanVK port, user mode)
                                     │
                                     ▼  IOCTL (MaliKmd ABI v2)
                          ┌──────────────────────────┐
                          │  MaliKmd.sys (KMDF)      │
                          │  power / MMU / job / IRQ │
                          └──────────────────────────┘
                                     │  MMIO + BO memory
                                     ▼
                             Mali-G57 MC2 hardware
```

The KMD is an **accelerator / hardware broker**, not a display or WDDM driver.
It is data-driven per SoC: register bases, power domains and clock gates live in
one table (`src/MaliSoc.c`, `include/mtk_gpu_soc.h`), so the same binary can be
retargeted to other MediaTek Mali parts.

Design rule: the ICD builds the job chain in a buffer object and the KMD only
programs the job-slot registers and rings the doorbell.

## Repository layout

```
windrv/
  ARCHITECTURE.md        design of the KMD, ABI and job model
  HARDWARE-MT6789.md     register map, power/clock topology, SoC facts
  ROADMAP.md             phases and current validation status
  WDDM-PATH.md           plan for making Windows enumerate Mali as a GPU
  REFERENCES.md          curated external references
  kmd/
    build.ps1            build + package (+ optional test signing)
    MaliKmd.inf          driver package description
    include/             public ABI + internal headers
    src/                 driver sources
    tools/
      winsdk.ps1         portable toolchain discovery
      build-malitest.ps1 builds the user-mode bring-up tool
      malitest.c         bring-up / probe tool (job, MMU, power, regs)
      device/            INSTALL.CMD + TEST.CMD for the target machine
tools/bridge/            USB-CDC bridge used to drive the tablet remotely
```

Everything the build produces goes to `windrv/kmd/build/` and is git-ignored.

## Building

### Prerequisites

* Windows 10/11 x64 build host
* **LLVM / clang** (`clang.exe` + `clang-cl.exe`) — either standalone LLVM or the
  Visual Studio workload *C++ Clang tools for Windows*
* **Windows 11 SDK** (ARM64 headers/libs)
* **Windows Driver Kit (WDK)** for Windows 11 — for `km/`, `wdf/kmdf/` and
  `Inf2Cat.exe`
* A linker: MSVC `link.exe` (Visual Studio Build Tools) **or** `lld-link.exe`
* Optional, only for signing: a code-signing certificate

Nothing is hard-coded: `tools/winsdk.ps1` discovers the SDK from
`$env:WINDOWSSDKDIR` / the `KitsRoot10` registry value, the WDK from an installed
WDK / `$env:WDK_ROOT` / a repo-local `external/wdk`, clang from
`$env:MALI_CLANG` / the VS LLVM directory / `PATH`, and the linker from MSVC or
LLVM.

### Build the driver

```powershell
cd windrv\kmd

.\build.ps1              # build, then sign if a test cert is found
.\build.ps1 -SkipSign    # build only - no certificate required
.\build.ps1 -Configuration Debug
```

Output:

```
windrv\kmd\build\MaliKmd.sys
windrv\kmd\build\package\   MaliKmd.inf, MaliKmd.sys, MaliKmd.cat, P13Test.cer
windrv\kmd\build\device\    INSTALL.CMD, TEST.CMD
```

### Build the probe tool

```powershell
cd windrv\kmd
.\tools\build-malitest.ps1
```

`malitest.exe` is CRT-free (clang-cl `/Zl`, own entry point, `/nodefaultlib`), so
no MSVC runtime is needed on the target.

### Overrides

| Parameter | Environment variable | Meaning |
| --- | --- | --- |
| `-WdkRoot` | `WDK_ROOT` | WDK root (contains `Include\`, `Lib\`) |
| `-KitsRoot` | `WINDOWSSDKDIR` | Windows SDK root |
| `-Clang` | `MALI_CLANG` | path to `clang.exe` |
| `-Linker` | `MALI_LINKER` | path to `link.exe` / `lld-link.exe` |
| `-TestPfx` | `MALI_TEST_PFX` | signing certificate |
| `-PfxPassword` | `MALI_TEST_PFX_PASSWORD` | certificate password |
| — | `MALI_TEST_CER` | public cert shipped with the package |

## Installing on the target

Test signing must be enabled (`bcdedit /set testsigning on`, reboot) and the
public certificate must be trusted in both `Root` and `TrustedPublisher` before
the driver will load. `tools/device/INSTALL.CMD` does exactly that:

```cmd
certutil -addstore -f Root             P13Test.cer
certutil -addstore -f TrustedPublisher P13Test.cer
pnputil /add-driver MaliKmd.inf /install
pnputil /restart-device "ROOT\SYSTEM\0001"
```

`TEST.CMD` then walks the bring-up steps (`--irq`, `--on`, `--jstatus`, `--bo`,
`--job`) without any further tooling.

## Bring-up notes

The interesting parts are all in [`windrv/ROADMAP.md`](windrv/ROADMAP.md) and
[`windrv/ARCHITECTURE.md`](windrv/ARCHITECTURE.md). Short version:

1. **No soft reset → the job never runs.** The JM accepts a job, `JS_STATUS`
   stays `ACTIVE` forever and no interrupt arrives.
2. **Power-on order matters.** The soft reset clears `PWRON`; powering the core
   stacks before the reset yields `JS_STATUS 0x41` (`POWER_FAULT`). Correct order
   is MTCMOS domains → GPU soft reset → power core stacks.
3. **Identity address mode.** With AArch64 4 KiB page tables the GPU raised
   `TRANSLATION_FAULT` on the very first descriptor fetch even though
   `TRANSTAB`/`MEMATTR`/`TRANSCFG` all read back correctly. Buffer objects are
   identity-mapped, so `AS_TRANSCFG_ADRMODE_IDENTITY` is used; page tables are
   kept behind `MALI_MMU_USE_PAGE_TABLES` for when that path is fixed.
4. **No IRQ resource.** The `ROOT\MaliMtk` node has no interrupt descriptors, so
   completion currently falls back to polling `JOB_IRQ_RAWSTAT`. An ACPI node
   with a `GpioInt`/`Interrupt` resource is needed for real IRQs.

## Reference material

Upstream sources and documentation used while writing this driver are listed in
[`windrv/REFERENCES.md`](windrv/REFERENCES.md), including:

* <https://github.com/fxlin/mali>
* <https://github.com/ARM-software/libGPULayers>
* <https://github.com/llvm/llvm-project>
* <https://gitlab.freedesktop.org/mesa/mesa>
* <https://github.com/whaltermc/panvk>

The MediaTek kernel tree (`android_kernel_mediatek_mt6789/`, ~1 GB) and the
captured bridge logs (`out/`, `bridge/`) are **not** part of this repository;
see `README.txt` for where the vendor tree came from.

## License

MIT — see [`LICENSE`](LICENSE). Driver sources carry
`SPDX-License-Identifier: MIT`.

Nothing here is affiliated with or endorsed by Arm, MediaTek or Microsoft.
Bringing up a GPU from scratch on retail firmware can brick a device; proceed at
your own risk.
