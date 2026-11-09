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
| `vulkan_panfrost.dll` | `mesa\build\mesa` | Mesa PanVK (links; **no v9 backend**, see caveat) |

`z-1.dll` is not optional: `libgallium_wgl.dll` imports `crc32` from it.

The GL probe is built separately from Mesa (it needs only the runtime shim):

```powershell
cd windrv\mesa
.\tools\build-runtime.ps1        # once
.\tools\build-gl-smoke.ps1       # -> build\gl-smoke\mali_gl_smoke.exe
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

## 4. Mesa PanVK (caveat)

`vulkan_panfrost.dll` links and is a valid ICD, but upstream PanVK has **no
Valhall v9 backend**, so it will not drive this GPU (see `mesa/README.md`). Keep
it in the kit for completeness and for the v10/v6/v7 paths; do not expect a
device from it on G57.

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
