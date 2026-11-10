# Test kit & deploy checklist

> What has to reach the tablet, in what order, and what "pass" looks like.
> Staging is automated; the tablet steps are **not** — run them by hand, one at a
> time, and stop at the first failure.

Related: [`umd/README.md`](umd/README.md) (ICD), [`kmd/README.md`](kmd/README.md)
(driver package), [`mesa/README.md`](mesa/README.md) (Mesa build),
[`GRAPHICS-STACK.md`](GRAPHICS-STACK.md) (V1–V5 stages).

---

## 0. Stage the kit (on the dev machine)

```powershell
cd windrv\..              # repository root
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
.\tools\stage-testkit.ps1
```

Output: `out\testkit\` (git-ignored) with the binaries and `MANIFEST.txt`
(SHA-256 + size per file). If the script prints `MISSING`, build that component
first — do not copy a partial kit.

| File | From | Needed for |
| --- | --- | --- |
| `MaliKmd.sys` / `.inf` / `.cat` | `kmd\build\package` | the driver |
| `malitest.exe` | `kmd\build` | KMD bring-up (power, MMU, job) |
| `mali_vk_icd.dll`, `mali_g57_icd.json` | `umd\build` | Jalur A Vulkan ICD |
| `mali_vk_smoke.exe` | `umd\build` | Jalur A V1-V5 smoke |
| `libgallium_wgl.dll`, `opengl32.dll`, `z-1.dll` | `mesa\build\mesa-gl` | Mesa GL artefacts — see §3 (not reachable on Windows) |
| `mali_gl_smoke.exe` | `mesa\build\gl-smoke` | offscreen GL probe (§3) |
| `vulkan_panfrost.dll` | `mesa\build\mesa` | PanVK v9 ICD (§4) |
| `panvk_smoke.exe` | `mesa\build\vk-smoke` | PanVK first-run validation (§4) |

`z-1.dll` is not optional: `libgallium_wgl.dll` imports `crc32` from it.

The two probes are built separately from Mesa (they need only the runtime shim):

```powershell
cd windrv\mesa
.\tools\build-runtime.ps1        # once, after any runtime\ change
.\tools\build-gl-smoke.ps1       # -> build\gl-smoke\mali_gl_smoke.exe
.\tools\build-vk-smoke.ps1       # -> build\vk-smoke\panvk_smoke.exe
```

---

## 1. Driver (KMD)

Prerequisites on the tablet: Secure Boot off and test signing on
(`bcdedit /set testsigning on`), then one reboot.

- [ ] Package copied to the tablet (USB / bridge — **ask the owner first**).
- [ ] `pnputil /add-driver MaliKmd.inf /install` (or `devcon`).
- [ ] Device appears; `ROOT\SYSTEM\0001` (or the ACPI node if published) bound to
      `MaliKmd`.
- [ ] `malitest.exe --power` → `GPU_ID 0x90930010`, cores powered.
- [ ] `malitest.exe --bo` → BO alloc / CPU readback / free.
- [ ] `malitest.exe --rawjob` and `--job` → `JS_STATUS 0`, sentinel cleared.
- [ ] `malitest.exe --irqstats` → **0 lines** is expected today (no ACPI
      interrupt resources); completions are polled.

Pass = job chains complete. This is already validated on silicon; it only
confirms the freshly staged build.

### 1b. Page-table variant (AArch64 4K) — validated 2026-11-09

The table path is now proven on silicon. It is a **separate package** so the
validated driver above is never replaced:

- [ ] `windrv\kmd\build.ps1 -PackageName MaliKmdPt -UsePageTables`
- [ ] `pnputil /add-driver ...\build\MaliKmdPt\package\MaliKmdPt.inf /install`
      then `devcon install <inf> ROOT\MaliMtkPt`
      (installs as a second device, `ROOT\SYSTEM\0002`, service `MaliKmdPt`).
- [ ] Disable the original device (`ROOT\MaliMtkPt` variant only) so there is no
      device-interface ambiguity.
- [ ] `malitest.exe --off --on --cores` before **every** experiment — fault
      registers and the job slot are sticky.
- [ ] `malitest.exe --rawjob --chain 2 --wv` must print
      `MMU enabled (UsePageTables=1, TranscfgLo=0x0x42000006)`,
      `JS_STATUS 0x00000000`, `AS_FAULTSTATUS 0x00000000`, `chain cleared 2/2`.
- [ ] `malitest.exe --bo --job --jstatus` → `job done: JS_STATUS 0x00000000`.

Any `AS_FAULTSTATUS 0x7C0002C0` means `AS_TRANSCFG` has a non-zero `PTW_SH`
field — see [`MMU-PAGE-TABLES.md`](MMU-PAGE-TABLES.md). Uninstall with
`pnputil /delete-driver oem<N>.inf /uninstall` after
`devcon remove ROOT\MaliMtkPt`.

---

## 2. Jalur A Vulkan ICD (our own, no shaders)

**Validated on silicon (2026-11-09): `RESULT: PASS` through V1–V5.**

- [x] `mali_vk_smoke.exe` next to `mali_vk_icd.dll` (it links the import library
      directly, so no loader is needed).
- [x] Run it → expect `RESULT: PASS` through V1–V5 (V1–V3 verified on silicon;
      V4/V5 also pass). V5 is the Win32 WSI
      path: it creates a surface + swapchain (images are KMD BOs), acquires, and
      presents with a GDI blit — no display miniport needed.
- [ ] Optional, loader-based: copy `mali_g57_icd.json` to
      `%ProgramData%\Vulkan\icd.d\`, set `VK_DRIVER_FILES` to the manifest path,
      and point `vulkaninfo` at it (needs an ARM64 Khronos loader).

Pass = V1–V5 pass. This proves `Vulkan → winsys → IOCTL → Mali job` end to end,
plus a full Win32 surface/swapchain/present cycle, and that the ICD is a valid
loader target. It does **not** execute shaders.

---

## 3. Gallium panfrost OpenGL — **not reachable on Windows** (2026-11-09)

Tested on the tablet. **This rung is blocked for two independent reasons**, both
outside our code; nothing here is a bug to fix.

- [x] Staged `libgallium_wgl.dll`, `opengl32.dll`, `z-1.dll` and
      `mali_gl_smoke.exe` together in one folder
      (`C:\P13Stage\MTKMALI\gl`). Never replace the system `opengl32.dll`.
- [x] Ran the offscreen test (`windrv\mesa\tools\gl-smoke\mali_gl_smoke.c`,
      built by `tools\build-gl-smoke.ps1`). Result:

      G1 opengl32.dll loaded: C:\P13Stage\MTKMALI\gl\opengl32.dll
      G2 window DC: 0 pixel format(s) available
      G2 screen DC: 0 pixel format(s) available
      G2 FAILED: no GDI pixel format (display driver has no OpenGL)

**Blocker 1 — GDI has no pixel formats.** `DescribePixelFormat()` returns 0 for
every index on both the window DC and the screen DC, and `ChoosePixelFormat()`
returns 0 for all seven descriptors tried (window/single, double-buffered,
24/32-bit, bitmap). The display driver registers no OpenGL ICD (rung L5 in
[`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md) is not done), so `wglCreateContext()`
is unreachable for *any* Mesa driver, llvmpipe included. Registering an ICD
through the display-class registry (`OpenGLDriverName`) would need admin and a
system change, so it is deliberately not used.

**Blocker 2 — no panfrost in Mesa's Windows frontend.** Even with a pixel format,
`libgallium_wgl.dll` cannot produce a panfrost screen: the Windows WGL frontend
only knows `d3d12`, `zink`, `llvmpipe`, `softpipe`
(`external/mesa/src/gallium/targets/wgl/wgl.c:123-137`), and the DRI/EGL-drm
platforms are not built when `with_platform_windows` (`meson.build:575`). EGL on
Windows is `egl_wgl.c`, which links the same `libgallium_wgl`
(`src/egl/meson.build:138-144`).

Consequence: the only OpenGL that can ever reach this GPU is **Zink over a
v9-capable Vulkan ICD**, i.e. it depends on the same Valhall v9 backend that
Vulkan needs. See [`GRAPHICS-STACK.md`](GRAPHICS-STACK.md) §3.1 and §8.

The GL artefacts stay in the kit (they are valid ARM64 builds and become useful
the moment Zink has a Vulkan driver underneath it), but do not expect them to
render on this device.

---

## 4. Mesa PanVK - v9 backend, not yet run on the tablet

`vulkan_panfrost.dll` is the Valhall v9 PanVK stack (FourFectVK base, Mesa
`feeadb5f` pinned) built for Windows ARM64. Unlike the upstream driver this one
**does** claim the G57: `panvk_physical_device.c` groups arch 9 with v6/v7/v14
and drives it through kbase.

What has been proven: it builds, exports the three ICD entry points, imports
only `KERNEL32`/`api-ms-win-crt-*`/`GDI32`/`USER32`/`SETUPAPI`/`ntdll`, and
`panvk_smoke.exe` links against it. What has **not** been proven: that the
kbase uAPI emulation in `mesa\runtime\mali_drm.cpp` is complete enough for a
real submission. That is what §4.1 answers.

### 4.1 First run (`panvk_smoke.exe`)

Copy `vulkan_panfrost.dll` and `panvk_smoke.exe` into the same folder on the
tablet and run the exe. It loads the DLL by name from its own directory, so no
loader, no registry entry and no `VK_ICD_FILENAMES` are involved.

Prerequisites:

- the KMD must be installed and running, and **nothing else may hold it** - the
  device is created exclusive (`WdfDeviceInitSetExclusive`), so close
  `malitest.exe` and any other Mali client first;
- `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` must be set in the process environment.
  `panvk_physical_device.c` gates arch 9 on it. The probe sets it itself with
  `_putenv_s()` before loading the DLL; any other application (DXVK, vkd3d)
  needs it set in its environment.

The probe runs in stages, so the first failing line names the layer:

| Stage | What it proves |
| --- | --- |
| V0 | the DLL is next to the exe, and how many KMD device interfaces exist |
| V1 | the DLL loads and exports `vk_icdNegotiateLoaderICDInterfaceVersion`, `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr` |
| V2 | loader/ICD interface negotiation (expect 7) |
| V3 | `vkCreateInstance` |
| V4 | `vkEnumeratePhysicalDevices` - the ICD opened `/dev/mali0` and kbase `VERSION_CHECK` passed |
| V5/V6 | device name, memory heaps and queue families (informational) |
| V7 | `vkCreateDevice` |
| V8 | buffer, memory, bind, map |
| V9 | `vkCmdFillBuffer` submitted and completed |
| V10 | the fill pattern read back correctly |

`RESULT: PASS` requires V1-V4 and V7-V10. V9 is the real test: on PanVK
`vkCmdFillBuffer` is a compute dispatch through the meta shaders, so a correct
pattern means shaders compiled, BOs were mapped, the job chain was built and
the KMD accepted the submit. Cross-check with `malitest.exe --irq` afterwards -
the job counter must have advanced. If it did not, the CPU wrote the buffer and
the GPU never ran.

Two things this build depends on, both fixed on 2026-11-10:

- the ICD enumerates through **`/dev/mali0`** (the kbase node). The DRM node
  `/dev/dri/renderD128` is still advertised for the Gallium path, but this
  build is configured with `-Dpanfrost-kmds=kbase,panthor`, so `drmGetVersion()`
  reports `"kbase"` for the mali node and `"panfrost"` only for `/dev/dri/`;
- patch `0011` makes a node whose kmod backend is not compiled in report
  `VK_ERROR_INCOMPATIBLE_DRIVER` instead of `VK_ERROR_OUT_OF_HOST_MEMORY`, so
  enumeration continues to the kbase node instead of aborting.

If V4 reports 0 adapters, read the V0 line first: `kmd interfaces 0` means the
driver is not installed or not started, and the ICD never had a chance. If V0
says 1, the next suspect is exclusivity - enumeration opens and closes the DRM
node before it opens `/dev/mali0`, and a second open while the first handle is
still alive would fail.

---

## 5. D3D translation (later)

vkd3d-proton (`d3d12.dll`, `d3d12core.dll`) and DXVK (`d3d11.dll`, `dxgi.dll`) go
in as **per-application DLL overrides** on top of whatever Vulkan ICD works.
They are not part of this kit until a v9-capable Vulkan path exists.

---

## 6. Rules that do not change

- **Do not reboot the tablet, and do not take the USB connection** without asking
  the owner first. Everything in §0 runs without the tablet.
- One failure = stop and report the exact output. Do not chain steps.
- Keep a recovery path: WinPE + UART. A bad driver load can leave the device
  unbootable until it is removed from WinPE.
