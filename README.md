# Mali-G57 (MT6789) GPU driver for Windows on ARM64

An experimental, from-scratch **kernel-mode driver for the ARM Mali-G57 MC2**
found in MediaTek MT6789 (Helio G99 / itel VistaTab 30 Pro P13001L and similar)
running **Windows 11 on ARM64**.

The goal is to make the GPU usable as a compute/render accelerator on a device
whose firmware only ever described it to Android. Display output is explicitly
*out of scope*: scanout stays on GOP / Microsoft Basic Display Adapter, and the
driver only ever owns the GPU core itself.

> **Status: early bring-up.** The driver builds, installs, powers the GPU on and
> executes jobs on real silicon. A user-mode layer is under way on top of it: a
> **UMD winsys** (`windrv/umd`) and a **Vulkan ICD skeleton** that reaches the GPU
> for memory and job work — but there are **no shaders yet**, so Windows does not
> yet expose Mali to real applications.

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
| Job chains (multiple linked jobs) | ✅ 128-byte descriptor alignment |
| Interrupt delivery | ⚠️ polling fallback (no ACPI IRQ resource yet) |
| UMD winsys `windrv/umd` (IOCTL ABI v2 wrapper) | ✅ builds ARM64, CRT-free |
| Job-chain builder `mali_jc` + host unit test | ✅ 26/26 checks pass on x64 |
| Vulkan ICD `windrv/umd/vulkan` | ⚠️ full Vulkan 1.0 + 1.1-discovery object model, transfers, and Win32 WSI (swapchain present via GDI blit); no shader execution |
| Vulkan ICD host self-test (`tools\run-host-tests.ps1`) | ✅ entry points + Win32 WSI resolve on x64 |
| Vulkan ICD device-object smoke test (V4a) | ✅ PASS on the tablet (2026-11-09) |
| Vulkan ICD Win32 WSI smoke stage (V5) | ✅ PASS on the tablet (swapchain BOs + GDI present) |
| Vulkan ICD (Mesa PanVK port, render) | ❌ links, but no Valhall v9 backend upstream |
| D3D12 / DXVK / vkd3d-proton path | ❌ depends on a shader-capable ICD |
| OpenGL via Zink | ❌ depends on a shader-capable ICD |
| OpenGL via Gallium `panfrost` | ❌ not reachable on Windows (Mesa WGL has no panfrost path; GDI exposes 0 pixel formats) |

## Architecture

```
   D3D12 application            D3D11 / OpenGL application
        │                              │
   vkd3d-proton ──► DXVK          DXVK / Zink (GL→Vulkan)
        └──────────┬───────────────────┘
                   ▼
        Vulkan ICD  (windrv/umd/vulkan — Jalur A, ours;
                     Mesa PanVK port — Jalur B, render)
                   │
                   ▼  winsys: open / BO / job / fence (windrv/umd)
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

The three graphics APIs (DirectX 12, Vulkan, OpenGL) and how they map onto this
stack are laid out in [`windrv/GRAPHICS-STACK.md`](windrv/GRAPHICS-STACK.md).

## Repository layout

```
windrv/
  ARCHITECTURE.md        design of the KMD, ABI and job model
  GRAPHICS-STACK.md      Vulkan, OpenGL and DirectX 12 on top of the KMD
  HARDWARE-MT6789.md     register map, power/clock topology, SoC facts
  ROADMAP.md             phases and current validation status
  WDDM-PATH.md           plan for making Windows enumerate Mali as a GPU
  PANVK-SHIM.md          PanVK (Jalur B): panfrost/DRM -> our ABI, gaps, ABI v3
  REFERENCES.md          curated external references
  umd/                   user-mode driver stack (winsys + Vulkan ICD)
    include/mali_umd.h   winsys API (thin wrapper over the IOCTL ABI)
    src/mali_umd.c       winsys implementation (CRT-free)
    src/mali_umd_test.c  round-trip test (open/BO/job)
    include/mali_jc.h    job-chain builder (JM descriptor encoding)
    src/mali_jc.c        builder implementation (pure C: host + ARM64)
    tools/build-umd.ps1  builds the test for ARM64
    tools/jc_test.c      host unit test for the builder (runs on this machine)
    vulkan/              Vulkan ICD (Jalur A from GRAPHICS-STACK.md)
      src/               loader interface + core objects + queue/submit
      test/              smoke test (ARM64/tablet) + iface test (x64/host)
      manifest/          mali_g57_icd.json (Khronos loader manifest)
      tools/             fetch-deps.ps1, build-icd.ps1, build-icd-host.ps1
  mesa/                  Jalur B build environment (Meson cross-file + findings)
    cross/               aarch64-windows-msvc Meson cross file (template)
    tools/               setup.ps1, build-smoke.ps1, fetch-mesa.ps1
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
tools/run-host-tests.ps1 all off-hardware tests (builder bytes + ICD surface)
tools/bridge/            USB-CDC bridge used to drive the tablet remotely
```

Everything the build produces goes to `windrv/kmd/build/` or `windrv/umd/build/`
and is git-ignored. Fetched dependencies (e.g. `external/Vulkan-Headers`) land in
the git-ignored `external/` tree at the repository root.

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

### Build the user-mode stack (winsys + Vulkan ICD)

The UMD winsys needs nothing beyond the same toolchain:

```powershell
cd windrv\umd
.\tools\build-umd.ps1            # build\mali_umd_test.exe
.\tools\build-jc-test.ps1        # host (x64) unit test for the job-chain builder
```

The Vulkan ICD additionally needs the Khronos `Vulkan-Headers` (fetched into the
git-ignored `external/`, like the WDK):

```powershell
cd windrv\umd\vulkan
.\tools\fetch-deps.ps1           # clones external\Vulkan-Headers
.\tools\build-icd.ps1            # build\mali_vk_icd.dll + manifest + smoke test
```

Both are ARM64 and CRT-free. To run the ICD, register the manifest from
`build\mali_vk_icd.json` (copy it into `%ProgramData%\Vulkan\icd.d\`, or point
`VK_DRIVER_FILES`/`VK_ICD_FILENAMES` at it).

### Verify without hardware

Everything that does not need the tablet is behind one script:

```powershell
.\tools\run-host-tests.ps1
```

It builds and runs, as x64 binaries on the development machine:

| Test | What it proves |
| --- | --- |
| `mali_jc` unit test | job-descriptor byte layout: 128-byte stride, `job_index`, `next_job`, 64-bit addresses |
| ICD entry-point self-test | every application entry point resolves, loader interface v2, chained `*Properties2` queries fill correctly, and the Win32 surface/swapchain extension set behaves |

Both must print `PASS`; the script exits non-zero otherwise. The ICD step is
skipped with a pointer to `fetch-deps.ps1` when `external\Vulkan-Headers` is
absent. This is the gate to run before pushing, and the first thing to run on a
fresh clone to confirm the toolchain.

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
   identity-mapped, so `AS_TRANSCFG_ADRMODE_IDENTITY` is used. The table path is
   kept behind `MALI_MMU_USE_PAGE_TABLES`; two bugs on it are now fixed (the
   enable path hard-coded IDENTITY and ignored the flag, and page tables were
   allocated cached although the walker uses non-cacheable attributes), but the
   `AARCH64_4K` mode itself still needs a silicon run.
4. **No IRQ resource.** The `ROOT\MaliMtk` node has no interrupt descriptors, so
   completion currently falls back to polling `JOB_IRQ_RAWSTAT`. An ACPI node
   with a `GpioInt`/`Interrupt` resource is needed for real IRQs.
5. **Job chains need 128-byte alignment.** Linking jobs with a 64-byte stride
   makes the JM reject the whole chain with `JS_STATUS 0x58`
   (`DATA_INVALID_FAULT`) before running anything (`HEAD == TAIL`). Every
   descriptor must be 128-byte aligned; 128/256-byte strides complete, 64/96/192/
   320 do not. `JS_CONFIG` flush bits make no difference.

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
