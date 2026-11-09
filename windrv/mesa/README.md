# Mesa / PanVK build environment (Jalur B)

This directory is the **build-toolchain side of Jalur B** (the Mesa PanVK port).
It does not contain Mesa; `tools\fetch-mesa.ps1` pins it into the git-ignored
`external\mesa`. It **does now build it**: as of 2026-11-09 this tree links a
complete ARM64 Windows `vulkan_panfrost.dll` with no MSVC ARM64 CRT installed.
See **Status** below.

Nothing here touches the tablet, the KMD, or USB. Everything it writes stays
inside the repository (`.venv\`, `build\`), and both are git-ignored.

## Status (measured on this host, 2026-11-08)

| Step | Result |
| --- | --- |
| Meson in a repo-local venv | ✅ Meson **1.12.1** (`tools\setup.ps1`) |
| Cross file accepted by Meson | ✅ `cross\aarch64-windows-msvc.ini.in` → `build\cross.ini` |
| Meson sees a cross build | ✅ host `aarch64`, build `x86_64` |
| C compiler | ✅ `clang-cl` **20.1.8** (`/clang:--target=aarch64-pc-windows-msvc`) |
| Linker | ✅ `lld-link` 20.1.8 |
| Compile an ARM64 object | ✅ machine type `0xAA64` (ARM64) |
| Archive a static library | ✅ `libmali_mesa_smoke.a` (via `llvm-ar`) |
| **Link an ARM64 `.exe`/`.dll`** | ✅ C **and** C++ link now, via the CRT-free recipe + `runtime\` shim (below) |
| Configure Mesa (panfrost) | ✅ pins `mesa-25.0.7`; configures with no CLC/LLVM |
| **Link `vulkan_panfrost.dll` (ARM64)** | ✅ **11.7 MB**, `COFF-ARM64`, exports `vk_icdGetInstanceProcAddr` — see below |

### Windows ARM64 PanVK links (2026-11-09)

`tools\build-mesa.ps1` now produces:

```
windrv\mesa\build\mesa\src\panfrost\vulkan\vulkan_panfrost.dll   11,745,792 bytes
```

That is the whole stack of this tree exercised end-to-end: the Meson cross
build, the libdrm/Win32 shim, the POSIX compat headers, the C++ runtime shim,
and the CRT-free link recipe. Three things had to be added since the first
configure:

| Gap | Fix |
| --- | --- |
| `dep_libdrm` never found ⇒ `vk_drm_syncobj.c` not built, but PanVK calls `vk_drm_syncobj_*` | patch `0009` builds it unconditionally; its `drmSyncobj*` calls resolve against the shim |
| `__chkstk`, `_tls_index` (from `vcruntime.lib`, not installed for ARM64) | `runtime/mali_aarch64.s` + `runtime/mali_win32os.cpp` |
| `sysinfo`, `sched_yield`, `mprotect`, `dup`/`strdup`/`stricmp`, Windows cache-UUID, build-id | compat headers under `cross\compat\`, `runtime/mali_win32os.cpp`, `runtime/mali_posix.cpp`, patches `0007`/`0008` |

**Caveat — v9 is not in this DLL.** The build covers Valhall **v6/v7** and
**v10** (CSF). Upstream PanVK does not support Valhall **v9** (Mali-G57, this
tablet's GPU); the JM backend is Bifrost-only. The two patches that would force
a `libpanvk_v9` are parked in `patches\experimental\` and each fails deep in
the Bifrost `RENDERER_STATE` draw path. So this DLL proves the *port* works; it
does not yet drive this GPU. See `patches\experimental\README.md`.

### What the missing CRT actually blocks (measured 2026-11-09)

The host has the ARM64 **dynamic UCRT** and the SDK, but not the MSVC C/C++
runtime libraries (`vcruntime.lib`, `libcmt.lib`, `libcpmt.lib`). What that does
and does not block was measured by linking real ARM64 objects with
`/nodefaultlib` and only what is already installed:

| Target | Libraries | Result |
| --- | --- | --- |
| **C** exe: `printf`/`malloc`/`sqrt`, a 16 KiB stack frame (forces `__chkstk`), `/GS` | `ucrt.lib` + `BufferOverflow.lib` + `kernel32.lib` + `ntdll.lib` | **LINKED OK** |
| same, minus `BufferOverflow.lib` | `ucrt.lib` + `kernel32.lib` + `ntdll.lib` | only `__security_cookie`, `__security_check_cookie` missing |
| **C++** exe: `std::string`, `std::vector`, `try/throw`, virtual dtor | `ucrt.lib` + `BufferOverflow.lib` + `kernel32.lib` + `ntdll.lib` | missing `__CxxFrameHandler3`, `_CxxThrowException`, `operator new/delete`, `__std_exception_copy/destroy`, `std::_Xlength_error`, `type_info::vftable` |
| anything linked against the **static** `libucrt.lib` | — | drags in `__GSHandlerCheck`, `__GSHandlerCheck_EH`, `__chkstk`, `__xp_*`, `_fltused`, none of which any installed lib defines |
| `msvcurt.lib` | — | unusable: `lld-link: msvcurt.lib(ti_inst.obj): 11000001 should not refer to special section 0` |

Conclusions:

- **C is not blocked at all.** The *dynamic* UCRT (`ucrt.lib` → `ucrtbase.dll`,
  which Windows 11 ARM64 ships) plus the SDK's `BufferOverflow.lib` (which
  provides the GS cookie) is enough to link a complete C ARM64 exe or DLL. Use
  the dynamic UCRT, not `libucrt.lib`.
- **C++ is blocked**, and only by the standard C++ runtime (`libcpmt.lib`).
  Replacing that is not a tiny shim; it is a libc++/MSVC-STL port.

That reframes Jalur B:

1. Mesa's `panfrost` driver, its Valhall shader compiler (`bi_*`) and **PanVK
   are C**. If a Vulkan-only, GL-disabled build stays C-only, it needs **no CRT
   install and no shim**.
2. C++ enters Mesa mainly through the **GLSL front-end** (`*-glsl-to-nir.cpp`
   and friends) and a few util files, which a PanVK-only build should not need.
3. So the next measurable step is to configure a minimal
   `-Dvulkan-drivers=panfrost` Mesa and check whether the file set is C-only.
4. Only if C++ is unavoidable do we need the ARM64 workload install or a C++
   runtime port.

Either way, executables can be *linked* here but not *run* here: the only ARM64
machine is the tablet.

## The shim (`runtime\`) — what it is and is not

`runtime\` is the ARM64 C++ runtime the CRT-free recipe needs. It is **not** a
libc++/STL port. Measured against `ucrt.lib + BufferOverflow.lib + msvcurt.lib +
kernel32.lib + ntdll.lib`, with the STL compiled `_HAS_EXCEPTIONS=0 /GR-`, the
only symbols missing were `atexit`, `std::_Raise_handler`, and
`operator new`/`delete`. Everything else (`_Mtx_lock`, `std::_Xlength_error`,
`std::_Throw_Cpp_error`, `__C_specific_handler`, ...) is already in
`msvcurt.lib`. So the shim is small and split into three archive members so the
linker pulls only what a target needs:

| Member | Provides |
| --- | --- |
| `mali_cxxrt.cpp` | `operator new`/`delete` (+ array, sized, aligned, nothrow), `atexit` → UCRT `_crt_atexit`, `std::_Raise_handler` |
| `mali_crt_exe.cpp` | `mainCRTStartup` (argv from `GetCommandLineA`, then `exit(main(...))`) |
| `mali_crt_dll.cpp` | `_DllMainCRTStartup` → the library's `DllMain` if any |
| `mali_posix.cpp` | POSIX spellings the UCRT only exports underscored (`read`, `write`, `open`… `dup`, `strdup`, `stricmp`) |
| `mali_drm.cpp` | the libdrm-compatible shim over the KMD (device enumeration, BOs, ioctls, mmap, syncobjs) |
| `mali_win32os.cpp` | Win32-backed Linux helpers: `sysinfo`, `sched_yield`, `_tls_index` |
| `mali_aarch64.s` | `__chkstk` (arm64 stack-probe helper clang emits; normally in `vcruntime.lib`) |

Two things make this work, and both are in Mesa already:

- Mesa compiles its C++ with `-D_HAS_EXCEPTIONS=0` (meson.build line 1359), so
  the STL never pulls the SEH exception machinery
  (`__CxxFrameHandler3`, `_CxxThrowException`), which we do **not** implement.
- `-Dcpp_rtti=false` keeps RTTI off, which also avoids `msvcurt.lib`'s broken
  `ti_inst.obj` (lld: *"should not refer to special section 0"*).

Prove it:

```powershell
.\tools\link-probe.ps1     # shows exactly which symbols each library adds
.\tools\build-runtime.ps1  # builds the shim and links a C exe + an STL DLL
```

The resulting DLL imports **only UCRT API sets** — no `msvcp140.dll`, no
`vcruntime140.dll` — so it stays self-contained on the target.

## First Mesa configure (2026-11-09): where it stops now

> **Resolved.** This wall was the CLC/LLVM dependency of Mesa's `libpan`
> CL kernels. It disappeared by pinning the checkout to **`mesa-25.0.7`**
> (`tools\fetch-mesa.ps1`), the newest release whose PanVK predates that work.
> The section is kept because it documents *why* the tree is pinned. Configure
> now completes (patch list below), and `vulkan_panfrost.dll` links.

`tools\setup.ps1` writes a cross file whose link recipe is the shim above
(`-fuse-ld=lld`, `/nodefaultlib`, the SDK libs, the shim). With that, Meson's
compiler sanity checks **pass** and the configure runs far:

```powershell
meson setup build\mesa ..\..\external\mesa --cross-file build\cross.ini `
  -Dvulkan-drivers=panfrost -Dpanfrost-rust=false -Dgallium-drivers= `
  -Dplatforms= -Dglx=disabled -Dgbm=disabled -Dtools= -Dbuild-tests=false `
  -Dopengl=false -Dllvm=disabled -Dcpp_rtti=false
```

It clears the toolchain, the Rust default (`panfrost-rust`, now off) and the
GCC-atomics probe, then stops at:

```
meson.build:1046: ERROR: Feature llvm cannot be enabled: CLC requires LLVM
```

This is **not** a CRT problem. Mesa's PanVK embeds CL kernels
(`src/panfrost/libpan/*.cl`: `draw_helper`, `indirect_dispatch`, `query_pool`,
`libpan_copy`) that are compiled at build time by `mesa_clc`, Mesa's own
OpenCL-C compiler, which links LLVM. This host has only `LLVM-C.lib` and no LLVM
headers, so `mesa_clc` cannot be built. `-Dllvm=disabled` is therefore rejected
whenever panfrost is enabled.

Ways forward (owner's decision):

1. **Pin an older Mesa release** where PanVK/Valhall predates the `libpan`
   CL-kernel work, so no CLC is needed. Cheapest and keeps the host untouched.
2. **Install LLVM dev** (libLLVM + clang headers) so `mesa_clc` builds natively.
   Contradicts the no-host-install rule.
3. **Vendor/spoof the generated `libpan` shaders** — i.e. patch Mesa to skip
   `src/panfrost/libpan` and CLC. Invasive: PanVK's draw/meta paths reference
   those kernels.

The CRT wall is solved; this next wall is a Mesa/LLVM packaging question, and it
is recorded in [`../GRAPHICS-STACK.md`](../GRAPHICS-STACK.md) and
[`../PANVK-SHIM.md`](../PANVK-SHIM.md).

## Files

```
mesa/
  README.md                       this file
  cross/aarch64-windows-msvc.ini.in   Meson cross-file template (tokens, no machine paths)
  cross/msvc-compat.h             force-included compat (atomics, asprintf, clock_gettime, …)
  cross/arm64-msvc-atomic.h       clang <arm64intr.h> stand-in for the MSVC-target atomics probe
  cross/compat/                   headers Windows lacks: sys/{mman,ioctl,ioccom,sysinfo,sysmacros}.h,
                                  sched.h, unistd.h, poll.h, xf86drm.h (libdrm API subset)
  runtime/mali_cxxrt.cpp          C++ runtime shim: operator new/delete, atexit, _Raise_handler
  runtime/mali_crt_exe.cpp        mainCRTStartup
  runtime/mali_crt_dll.cpp        _DllMainCRTStartup
  runtime/mali_posix.cpp          POSIX/oldnames aliases (read/write/dup/strdup/stricmp/…)
  runtime/mali_drm.cpp            libdrm + libsyncobj + POSIX shim over the Mali KMD
  runtime/mali_win32os.cpp        sysinfo / sched_yield / _tls_index
  runtime/mali_aarch64.s          __chkstk
  patches/*.patch                 portability patches, applied by tools/patch-mesa.ps1
  patches/experimental/           v9-enable patches, deliberately NOT applied (see its README)
  smoke/meson.build, smoke/hello.c    trivial target: compile + archive an ARM64 lib
  smoke/link_probe.c, link_probe.cpp  the sources link-probe.ps1 measures with
  tools/setup.ps1                 venv + Meson + materialise build\cross.ini + readiness check
  tools/fetch-mesa.ps1            shallow-clone upstream Mesa at the pinned tag into external\mesa
  tools/patch-mesa.ps1            reset the checkout and apply patches\*.patch (idempotent)
  tools/build-mesa.ps1            patch, configure, and compile Mesa → vulkan_panfrost.dll
  tools/build-runtime.ps1         build runtime\ + prove a C exe and an STL DLL link
  tools/link-probe.ps1            measure what each SDK library contributes to an ARM64 link
  tools/build-smoke.ps1           configure + build the smoke target
```

Generated, git-ignored: `.venv\`, `build\`. Fetched source goes to `external\mesa\`.

## How to run

```powershell
cd windrv\mesa
.\tools\setup.ps1          # venv + Meson + cross.ini  (prints the readiness check)
.\tools\build-runtime.ps1  # build the C++ runtime shim + prove exe/dll link
.\tools\fetch-mesa.ps1     # pin upstream Mesa at the tag into external\mesa
.\tools\patch-mesa.ps1     # reset the checkout and apply patches\*.patch
.\tools\build-mesa.ps1     # patch + configure + compile → vulkan_panfrost.dll
.\tools\link-probe.ps1     # show what each SDK library contributes to the link
.\tools\build-smoke.ps1    # configure + compile + archive the ARM64 smoke lib
```

`build-mesa.ps1` runs `patch-mesa.ps1` itself, and touches a fresh `meson setup`
only when the cross file or the build graph changed (`-Reconfigure` to force).
It also exports `LIB`/`INCLUDE` for the regeneration step: `meson setup`
activates the MSVC environment, but `ninja`'s internal regenerate does not, and
Mesa's `find_library('ws2_32')` fails without it.

`setup.ps1` also *builds the shim* on first run, because the cross file links it
by default. Pass `-SystemCrt` to fall back to the normal (installed) MSVC
libraries if that workload ever gets added.

`setup.ps1` reuses the same LLVM + Windows SDK the KMD/UMD scripts resolve
(`windrv/kmd/tools/winsdk.ps1`), so the whole project shares one toolchain. It
accepts the same overrides (`-KitsRoot`, `-WdkRoot`, `-Clang`, `-Linker`).

## Why these choices

- **clang-cl, not the GNU clang driver.** Meson probes the linker with
  `-Wl,--version`; for a `*-windows-msvc` target the GNU driver forwards it to
  `link.exe`, which rejects it, and Meson aborts. clang-cl takes Meson's
  MSVC-like path, where the linker is known to be `link`/`lld-link`. The target
  is requested with `/clang:--target=aarch64-pc-windows-msvc`.
- **`llvm-ar`, not `llvm-lib`.** Meson appends `/MACHINE:x64` to `llvm-lib`
  because the archiver runs on the x64 build machine, which then conflicts with
  the ARM64 objects. `llvm-ar` takes no machine flag.
- **A cross file template, not a fixed file.** Paths differ per machine; the
  template is committed, `setup.ps1` fills it. This keeps the repo portable, the
  same rule the KMD/UMD builds follow.

## Next steps for Jalur B

The build works. What is left is the GPU itself:

1. **v9 is the wall now.** Upstream PanVK supports Valhall v6/v7 (JM, Bifrost
   descriptors) and v10 (CSF), but not v9 (Mali-G57). `docs/drivers/panfrost.rst`
   leaves the Vulkan column blank for G57, and `jm\panvk_vX_cmd_draw.c` hard-uses
   Bifrost's `RENDERER_STATE`, which v9 genxml does not have. Options, in
   `../GRAPHICS-STACK.md`:
   - **OpenGL via Gallium Panfrost**, which *does* support v9 (`panfrost` Gallium
     is the mainline v9 driver). This is the realizable hardware path.
   - **Software Vulkan (lavapipe)** to carry VKD3D/DX12 without the GPU.
   - **Write a Valhall-JM v9 Vulkan backend** — a new backend, not a port.
2. Once a driver renders, **validate on the tablet**: install the ICD JSON, run
   `windrv\umd\vulkan\test\mali_vk_smoke` / `vulkaninfo`, then `vkd3d`/`DXVK`.
3. The `WDDM-PATH.md` work (render-only WDDM miniport, or a compute-accelerator
   class driver) is what makes the GPU appear to Task Manager/DXGI; independent
   of which userspace API the driver exposes.

The shader-compiler work (SPIR-V → Valhall) is real; the Win32/DRM shim that
used to come first is now written and exercised by the build.
