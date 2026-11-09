# MT6789 on Windows 11 ARM64 — driver plan & DX12 path

> Ringkasan arsitektur: `ARCHITECTURE.md`. Detail hardware: `HARDWARE-MT6789.md`.
> **Target: Windows 11 ARM64 full install** (bukan WinPE) untuk fase render/D3D.

> Honest scope first. Read this part before the phases.

## 1. Can it be done? Realistic answer

**CPU / timer / interrupt — yes, and mostly done.**
Windows on ARM has a generic ARM64 HAL. Nothing custom is needed for the CPU.
What is needed is correct **ACPI** (`MADT`/`GTDT`/`FADT`/`PPTT`, `DSDT`) + PSCI.
The `edk2-UEFI-mtk` tree is already drafting those tables. This is a
completion/verification task, not a research project.

**Display — yes, but split carefully.** Mali has **no scan-out**: on MT6789 the
panel is driven by MediaTek MMSYS (OVL → RDMA → DSI), a different block from the
GPU. Android works the same way — Mali only writes buffers that the display
controller reads. See [`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md) for the full map
and the L1–L5 ladder. The tiers are:
1. Tier A (fast): ship the UEFI GOP framebuffer and let the **Microsoft Basic
   Display Adapter** drive scanout at native res. Gets a real desktop today.
2. Tier B (Mali renders, BDA scans out): present path copies the rendered frame
   into the BDA framebuffer. No WDDM, no UMD, no display driver.
3. Tier C (proper): a WDDM display-only miniport for MTK MDP/OVL/DSI — last,
   because it needs per-panel DSI init and a recovery path.

**GPU 3D + DirectX 12 — possible, but this is research-grade and long.**
There is **no Mali (Arm) WDDM/Windows driver in existence** — not from Arm, not
from MediaTek, not from the community (the WOA/Renegade ports only have
Snapdragon Adreno, via ported Qualcomm drivers). So there is nothing to "port
wholesale". That absence is precisely why this project exists: the Windows
graphics layer is **written, not ported**. The staged plan for making Windows
recognise Mali as a GPU is in [`WDDM-PATH.md`](WDDM-PATH.md).

DirectX 12 on Windows cannot be "translated" without a driver underneath it:
`d3d12.dll` loads a UMD from a display driver. The known-good trick is:

```
D3D12 app ──► vkd3d-proton (drop-in d3d12.dll + d3d12core.dll)  ─┐
D3D11/10/9 app ──► DXVK (d3d11.dll / dxgi.dll)                   ├─► Vulkan ICD
                                                                 ┘
                          Vulkan ICD for Mali-G57  ──►  Mali hardware
```

- `vkd3d-proton` and `DXVK` are **native Win32 DLLs** and already run on Windows
  (they are the same libs Proton uses). **We do not have to write the D3D side.**
- The missing link is a **Vulkan ICD that talks to Mali-G57 from Windows.**
  Mesa's `PanVK` already speaks Valhall (G57), but it expects the Linux
  `panfrost`/`panthor` kernel/DRM interface (`/dev/dri`, ioctls, GEM/dma-buf).
  Windows has no such thing, so we supply it.

So the whole game reduces to **two components we own**:

1. a small **kernel-mode driver (KMD)** that owns the Mali hardware
   (registers, clocks, power, IRQs, SMMU, memory), and
2. a **user-mode Vulkan ICD** that talks to that KMD (a Windows-shaped
   `panfrost`/`kbase` shim under Mesa's PanVK).

Everything above that (D3D12/D3D11/D3D9) is free, reused from upstream.

**Stopgap (ship today, no hardware acceleration):** WARP (software D3D12) is
built into Windows. With a working display driver, D3D12 apps run *correctly*
(just slow). That gives "DX12 jalan" now while the hardware path is built.

## 2. Target definition

The target is a **render + display stack**, split so that partial wins are
usable:

| Layer | Component | Owner |
|-------|-----------|-------|
| CPU/timer/IRQ | ACPI tables + PSCI | finish in `edk2-UEFI-mtk` |
| Display | Basic Display Adapter → present-by-copy → later WDDM display miniport (MMSYS/OVL/DSI) | us |
| GPU memory/regs | **Mali KMD (KMDF, ARM64)** | us |
| Vulkan | **Mesa PanVK ICD on Windows** — talks to KMD | us (port) |
| D3D12 | vkd3d-proton | reuse |
| D3D9/10/11 | DXVK | reuse |

## 3. Phases

**Phase 0 — Foundation (this folder)** — *mostly done*
- [x] Hardware inventory (`HARDWARE-MT6789.md`).
- [x] ARM64 driver build with **clang + the extracted (read-only) WDK** and the
  MSVC x64 linker — no WDK/MSVC install needed. `build.ps1` emits a signed
  `build\package\{MaliKmd.sys,MaliKmd.cat,MaliKmd.inf}` and runs `Inf2Cat`.
- [ ] Confirm board/slot + keep a WinPE boot loop for bring-up.

**Phase 1 — Display + ACPI (usable desktop)** — *done*
- [x] Windows 11 ARM64 boots; desktop up; 8 CPUs; timer ticking.
- [x] Display via Basic Display Adapter at native res (no display driver written).
- [x] ACPI (`MADT`/`GTDT`/`FADT`/`DSDT`) sufficient for boot in `edk2-UEFI-mtk`.
- [ ] Publish the Mali node in ACPI so PnP binds it without the ROOT hack (2g).

**Phase 1.5 — Stopgap DX12** — *optional, available now*
- Enable WARP; validate a D3D12 sample runs on the device.
- Result: "DX12 berjalan" (software), correctness first. Not Mali acceleration.

**Phase 2 — Mali KMD (accelerator / hardware broker, KMDF)** — *validated on silicon*
- [x] Map `0x13000000` (GPU), `0x13FBF000` (MFG_TOP), `0x1000C000` (APMIXED),
  `0x10006000` (SPM/scpsys). Identification (`GPU_ID`, `CORE_FEATURES`,
  `MMU_FEATURES`, `SHADER_PRESENT`) is read **only when the MFG domains already
  report powered**, so a cold boot cannot touch a dead block.
- [x] Data-driven SPM MTCMOS power for `MFG0..3`, matching `mtk-scpsys.c`:
  `PWR_ON`/`PWR_ON_2ND` (bits 2/3), both `PWR_STATUS` (0x16C) and
  `PWR_STATUS_2ND` (0x170) must agree, then `PWR_CLK_DIS`/`PWR_ISO` release,
  `PWR_RST_B`, and SRAM un-gate with ACK. Driven by IOCTL; load is read-only.
- [ ] **Phase 2h — MFG clock tree**: TOPCKGEN `CLK_CFG_4` mux (`mfg_pll_sel`,
  bit 18), APMIXED `MFGPLL_CON0..3` enable, and the MFG_TOP clock gates. Until
  this lands, a successful MTCMOS power-on may still leave the GPU clock gated
  and `GPU_ID` reading 0. Deliberately not poked yet.
- [x] GPU buffer objects: physically-contiguous alloc, MDL user mapping, handle
  table, free-with-unmap.
- [x] Mali **MMU** (AArch64 4K, 4-level page tables, identity VA==PA), enable on
  AS0 with `AS_COMMAND_UPDATE` (verified against `kbase_mmu_hw_configure`), PTE
  type/attr/AF/RW bits verified against `mali_kbase_mmu_mode_aarch64.c`, fault
  status readback. Page-table creation is race-safe.
  **Address mode: `ADRMODE_IDENTITY` by default** (`MALI_MMU_USE_PAGE_TABLES` in
  `include/mali_mmu.h` flips back). Rationale: BOs are identity mapped, so the
  table buys nothing today, and on silicon the AArch64 table path raises
  `AS_FAULTSTATUS=0x7C0002C0` (translation fault, read) on the first descriptor
  fetch even though `TRANSTAB`, `MEMATTR` and `TRANSCFG` read back correctly;
  `IDENTITY` completes the same job in 0 ms. Two suspects to chase before
  enabling tables: (a) page-table pages are allocated >4 GB and the walker may
  not reach them, (b) table VAs come from `MmGetVirtualForPhysical()` (cached
  direct map) while the pages are `MmNonCached`, which is architecturally
  unpredictable on ARM64. Per-page cache attributes will be needed for peak
  throughput, so this must be fixed before performance work.
- [x] **Core power**: shader/tiler/L2 `PRESENT -> PWRON -> poll READY`, after
  MTCMOS; `SHADER_PRESENT` offset corrected (`0x100`, was wrongly `0x2C0`).
- [x] **GPU soft reset on power-up**, matching `kbase_pm_do_reset`: after MTCMOS
  is on, write `GPU_COMMAND=SOFT_RESET` and poll `GPU_IRQ_RAWSTAT.RESET_COMPLETED`,
  *then* power the core stacks. Two bring-up failures came from the missing
  ordering: without any reset the JM accepted a job but never ran it
  (`JS_STATUS` stuck at `0x08` = ACTIVE, no IRQ); with the reset but cores
  powered *before* it, the first submit failed `JS_STATUS=0x41` (POWER_FAULT)
  because the reset clears `PWRON`.
- [x] Register the GPU/MMU/JOB interrupt lines (level-triggered, mask-in-ISR /
  clear-and-restore-in-DPC, GPU+MMU fault capture, counters). EVENT/PWR are
  intentionally left to the power work. **Zero lines is tolerated**: the ROOT
  device has no interrupt resources until ACPI publishes the node, in which case
  completions fall back to polling `JOB_IRQ_RAWSTAT`.
- [x] Job submission: userspace supplies the chain head GPU VA, the KMD programs
  `JS_HEAD_NEXT` / `JS_AFFINITY_NEXT` / `JS_CONFIG_NEXT` / `JS_COMMAND_NEXT` and
  offers a synchronous completion fence (IRQ generation or RAWSTAT poll).
  `JS_CONFIG` bits verified against `mali_kbase_gpu_regmap_jm.h`:
  start-flush bits 8-9, `START_MMU` bit 10, end-flush bits 12-13, pri `<<16`.
  `IOCTL_MALI_JOB_SELFTEST` exercises the path with an empty chain.
- [x] IOCTL ABI for the ICD: open, caps, power, BO alloc/map/free/info, MMU
  control + fault, job submit/status/stop/selftest, IRQ stats (ABI v2).
- [x] CRT-free ARM64 `malitest.exe` + `INSTALL.CMD` / `TEST.CMD`, uploaded over
  the USB CDC bridge with SHA-256 verification.
- [x] **Hardware validation** of every path: power on/off (`PWR_STATUS` both
  copies), `GPU_ID=0x90930010` (tNAx r0p1, Mali-G57 MC2), MMU enable, BO
  alloc/CPU readback/free, and a **null job completing**
  (`JS_STATUS=0x0`, `JOB_IRQ_RAWSTAT` slot0 done, `CMD_NEXT=0`) both from
  `--rawjob` and through `MaliJobSubmit` with `JS_CONFIG=0x3700`.
  Remaining gap: zero interrupt lines, because `ROOT\MaliMtk` has no ACPI
  resources yet, so completions are polled (2g).
- [x] **Real job chains** — validated on silicon: 2, 4, 8 and 15 WRITE_VALUE jobs
  linked through `next_job` all complete with `JS_STATUS=0` and every sentinel
  cleared. The hardware rule that cost the most time: **each descriptor in a
  chain must be 128-byte aligned** (`next_job & 0x7F == 0`). A 64-byte stride
  puts the next header inside the previous descriptor's footprint and the JM
  rejects the entire chain with `0x58` (`DATA_INVALID_FAULT`) before running any
  job (`HEAD == TAIL`). Strides 128/256 work, 64/96/192/320 fail. `JS_CONFIG`
  flush bits are irrelevant to this. Encoded in `include/mali_job.h` for the ICD.
- [ ] Shared fences / timeline semaphores for the ICD (Phase 3).
- [ ] System SMMU/IOMMU (bypassed for bring-up — GPU-side MMU is used instead).
- Result: power on GPU, allocate GPU memory, map it, run a null job, see IRQs.

**Phase 3 — Vulkan ICD** *(critical path)*
- [x] **Jalur A — ICD skeleton we own** (`windrv/umd/vulkan`, on the winsys in
  `windrv/umd`): Khronos loader interface (version 2), instance / physical device
  / device (opens the KMD, powers on, enables the MMU), `vkAllocateMemory` →
  buffer object, buffers, and `vkCmdFillBuffer` as a real GPU `WRITE_VALUE` job
  chain through `vkQueueSubmit` + fences (synchronous). No shaders yet. See
  [`GRAPHICS-STACK.md`](GRAPHICS-STACK.md) and [`umd/README.md`](umd/README.md).
- [~] **Jalur B — render**: port Mesa `PanVK` to Windows ARM64.
  - [x] Build a Windows ARM64 PanVK: `vulkan_panfrost.dll` (11.7 MB) links with
    the CRT-free recipe + libdrm/POSIX shim (`mesa/README.md`, patches
    `0001..0011`, 9 active). DRM backend replaced by our IOCTL backend; no MSVC
    ARM64 CRT needed, and no VC++ redistributable dependency
    (`VCRUNTIME140`/`MSVCP140` eliminated from all four DLLs).
  - [x] Build the Gallium panfrost OpenGL pair: `libgallium_wgl.dll` (14.7 MB) +
    `opengl32.dll` (115 KB), same recipe (`build-mesa.ps1 -Kind gl`). This is the
    v9 render path; `z-1.dll` must ship alongside it.
  - [ ] **The v9 gap.** Upstream PanVK supports Valhall v6/v7 (Bifrost JM) and
    v10 (CSF), **not v9** (G57). So the linked DLL does not drive this GPU.
    Options (see [`GRAPHICS-STACK.md`](GRAPHICS-STACK.md) §2.2/§8):
    (a) **Gallium `panfrost` OpenGL** — mainline *does* support v9; render in GL
    first; (b) software Vulkan (lavapipe) under vkd3d as a stopgap; (c) write a
    Valhall-JM v9 Vulkan backend (new backend, not a port).
  - [ ] Implement/adjust Win32 WSI (`VK_KHR_win32_surface`) so swapchain present
    works — needed by Zink too, so it follows whichever GL/Vulkan driver is real.
- Result: a clear triangle / Vulkan CTS smoke test on Mali.
- This, not the KMD, is what makes the GPU visible to applications.

**Phase 4 — D3D translation**
- Drop in vkd3d-proton (`d3d12.dll`, `d3d12core.dll`) and DXVK (`d3d11.dll`,
  `dxgi.dll`) as per-application DLL overrides on top of the ICD.
- Result: D3D11 and D3D12 apps render on Mali.
- Note: only apps using the overrides are accelerated; desktop/DWM stay on BDA.

**Phase 5 — WDDM render-only adapter (Windows sees a GPU)** — *planned, see
[`WDDM-PATH.md`](WDDM-PATH.md)*
- [ ] ACPI node for the GPU + real interrupt lines (shared work with Phase 2g).
- [ ] Page tables / multi-AS / non-contiguous allocations (Phase 2e follow-up) —
      VidMm will not hand out physically contiguous memory.
- [ ] Render-only WDDM miniport (`NumOfChildren = 0`, no display outputs) so
      DXGI enumerates Mali and Task Manager shows a GPU row. Scanout stays on
      BDA; no display/DSI/MDP work.
- [ ] Optional: route ICD submissions through `D3DKMT` so Task Manager engine
      utilisation reflects real Mali work instead of reading 0 %.
- [ ] Optional, long term: a D3D12 UMD — the only way to get native D3D on Mali.
- Needs Microsoft driver signing to ship.
- Ladder position: this is **L3**. It makes Task Manager show the adapter, but
  the desktop still renders elsewhere. **L4** (desktop rendered by Mali) needs a
  D3D11 UMD because DWM only speaks D3D. See [`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md) §5.

**Phase 5b — Display miniport for MTK MMSYS/DSI (last)** — *L5, high risk*
- [ ] Port `mtk_drm_drv`/`mtk_disp_ovl`/`mtk_disp_rdma`/`mtk_dsi` concepts to a
      WDDM display miniport (VidPn, scan-out, vsync, hotplug).
- [ ] Per-panel DSI init sequence for P13001L; DSI PHY/PLL tuning; DSC if used.
- Requires a proven recovery path (WinPE + UART) before first attempt: a bad
  mode set can leave the panel dark and GOP does not automatically return.

**Phase 6 — Distribution**
- Dev: test signing. Ship: attestation/WHQL (ARM64 kernel drivers must be
  Microsoft-signed).
- Package ICD manifest + DLL overrides so a user can install without a build
  toolchain.
- [x] `tools\stage-testkit.ps1` collects the deployable set into `out\testkit\`
      with a SHA-256 manifest, and [`DEPLOY.md`](DEPLOY.md) is the ordered
      checklist (KMD → Jalur A ICD → Gallium panfrost GL → D3D overrides).

**Signing (Windows 11 ARM64 full install)**
- Dev: Secure Boot off + `bcdedit /set testsigning on`, self-signed driver.
- Ship: attestation/WHQL (ARM64 kernel drivers must be Microsoft-signed).

## 4. What is reused vs written

External references (links, papers, notes) are collected in
[`../REFERENCES.md`](../REFERENCES.md).

Reused (external, for reference/reuse as allowed):
- `vkd3d-proton` (D3D12→Vulkan), `DXVK` (D3D9/10/11→Vulkan).
- Mesa `PanVK`/`panfrost` (Valhall userspace + register knowledge).
- Arm `kbase` r32p1 and MTK `ged`/`gpufreq` (this repo) as the register/clock/
  power reference for the KMD.

Written by us:
- Mali **KMD** for Windows (new).
- **UMD winsys** (`windrv/umd`, new) — the single caller of the IOCTL ABI.
- **Jalur A Vulkan ICD** (`windrv/umd/vulkan`, new) — memory/buffer/fill/fence
  path on the KMD; the measuring stick for Jalur B.
- **Windows backend** for PanVK (new — the port).
- WDDM display miniport (new, later).

## 5. Risks / cost

- ✅ **Porting PanVK off DRM is done** (shim `mali_drm.cpp` + `cross/compat/*`),
  and the Windows ARM64 DLL links. The remaining render risk is **v9 having no
  Vulkan backend upstream** — a real backend to write, not a build problem. The
  lower-risk render path for this GPU is Gallium `panfrost` OpenGL (v9 is its
  mainline target).
- ✅ Mesa's ARM64-Windows build is now proven here: `vulkan_panfrost.dll`,
  cross-compiled with clang-cl and linked CRT-free.
- Win32 WSI (swapchain present) is a separate integration point from rendering.
- ARM64 driver signing: test-sign for dev, attestation/WHQL to ship.
- No physical debug is cheap: keep the UART console and a WinPE boot loop.
- Timeline is months of iteration, not weeks. Each phase delivers something
  usable on its own, so progress is never all-or-nothing.
- The WDDM surface (Phase 5) is large but fully documented; the UMD D3D12 surface
  is the only genuinely unbounded piece, which is why it is last and optional.
