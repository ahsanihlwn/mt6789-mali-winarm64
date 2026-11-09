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
| `libgallium_wgl.dll`, `opengl32.dll`, `z-1.dll` | `mesa\build\mesa-gl` | Gallium panfrost OpenGL (v9 render path) |
| `vulkan_panfrost.dll` | `mesa\build\mesa` | Mesa PanVK (links; **no v9 backend**, see caveat) |

`z-1.dll` is not optional: `libgallium_wgl.dll` imports `crc32` from it.

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

---

## 2. Jalur A Vulkan ICD (our own, no shaders)

- [ ] `mali_vk_smoke.exe` next to `mali_vk_icd.dll` (it links the import library
      directly, so no loader is needed).
- [ ] Run it → expect `RESULT: PASS` through V1–V5 (V1–V3 verified on silicon;
      V4/V5 are built but have not been run on the tablet). V5 is the Win32 WSI
      path: it creates a surface + swapchain (images are KMD BOs), acquires, and
      presents with a GDI blit — no display miniport needed.
- [ ] Optional, loader-based: copy `mali_g57_icd.json` to
      `%ProgramData%\Vulkan\icd.d\`, set `VK_DRIVER_FILES` to the manifest path,
      and point `vulkaninfo` at it (needs an ARM64 Khronos loader).

Pass = V1–V5 pass. This proves `Vulkan → winsys → IOCTL → Mali job` end to end,
plus a full Win32 surface/swapchain/present cycle, and that the ICD is a valid
loader target. It does **not** execute shaders.

---

## 3. Gallium panfrost OpenGL (the v9 render path)

This is the first component that can actually render on this GPU. It is
**untested on hardware**.

- [ ] Put `libgallium_wgl.dll`, `opengl32.dll` and `z-1.dll` together in **one
      folder next to the test program** — never replace the system
      `opengl32.dll`.
- [ ] Run an offscreen GL test first (a context and an FBO, no window). The
      winsys reaches the GPU through the libdrm shim (`mali_drm.cpp`) → KMD.
- [ ] Only then try window present — that needs Win32 WSI and a display path
      ([`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md) L2).

Pass = a GL context is created and an offscreen draw reaches the GPU (check the
KMD's job/IRQ counters). If the context fails, the likely cause is the DRM
winsys handshake, not the compiler.

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
