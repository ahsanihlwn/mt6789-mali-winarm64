# Mesa / PanVK build environment (Jalur B)

This directory is the **build-toolchain side of Jalur B** (the Mesa PanVK port).
It does not contain Mesa and does not build it yet — it pins down the one
question that decides whether a PanVK port is even reachable from this machine:
*can we cross-compile an ARM64 Windows binary here?*

The answer is now measured. See **Status** below.

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
| **Link an ARM64 `.exe`/`.dll`** | ❌ **blocked** — no ARM64 MSVC CRT on this host |

### The one real prerequisite

The host has the **UCRT** for ARM64 (`...\Windows Kits\10\Lib\...\ucrt\arm64\`)
but **not** the MSVC C/C++ runtime:

```
VC\Tools\MSVC\14.50.35717\lib\arm64\   ->   35 files, all clang_rt.*   (compiler-rt only)
                                            no vcruntime.lib, no libcmt.lib, no libcpmt.lib
```

Mesa is a normal C/C++ program: it uses libc, and links shared libraries. Our own
KMD/UMD tools are deliberately **CRT-free**, which is why they build fine here —
but that style does not scale to Mesa. So:

> **Before Mesa can be built (or even sanity-linked) on this machine, the VS
> Build Tools ARM64 workload must be installed** — the component that ships
> `libcmt.lib` / `vcruntime.lib` / `libcpmt.lib` for ARM64 (e.g. *"MSVC v143/v145
> - VS C++ ARM64/ARM64EC build tools"*).

This is a one-time host setup, not a code change. `tools\setup.ps1` checks for it
and says so at the end.

### Can we avoid installing it? (measured 2026-11-09)

Partially, and the experiment is worth recording. Linking an ARM64 console exe
that calls `printf`/`malloc`/`strlen` with **only what is already installed**:

| Libraries tried | Result |
| --- | --- |
| `msvcurt.lib` + `libucrt.lib` + `clang_rt.builtins-aarch64.lib` + `kernel32.lib` | libc resolved; **missing** `__security_cookie`, `__security_check_cookie`, `__GSHandlerCheck`, `__GSHandlerCheck_EH` |
| `BufferOverflow.lib` (SDK) + `libucrt.lib` + builtins + `ntdll.lib` | adds `__security_cookie`; **missing** `__GSHandlerCheck`, `__GSHandlerCheck_EH`, `__security_push_cookie`, `__security_pop_cookie`, `_InterlockedAnd`, `_InterlockedOr` |
| `ntstc_libcmt.lib` (SDK) + `libucrt.lib` | no `malloc`/`strlen` — it is a staging CRT, not a full static one |

So the gap is exactly the **ARM64-specific `vcruntime` internals** (GS/unwind
cookie helpers and the interlocked intrinsics wrappers), not libc, not the
linker, and not the headers. Those symbols live in `libvcruntime.lib`, which the
ARM64 MSVC workload ships.

That means there are two ways forward, and the choice is the owner's:

1. **Install the ARM64 CRT workload** (one-time, ~1 GB, needs admin). Everything
   then works the way MSVC intends and Mesa links unchanged.
2. **Ship a tiny ARM64 `vcruntime` shim** (`windrv/mesa/runtime/`) defining the
   handful of symbols above — mingw-w64 does exactly this for its arm64 target.
   No host changes, but it becomes a support surface we own and must get right
   for exception handling and stack protection. It also cannot be executed here:
   the only ARM64 machine is the tablet.

Until one of those is done, ARM64 **executables and DLLs cannot be linked on
this host**; static libraries can (see `tools\build-smoke.ps1`).

## Files

```
mesa/
  README.md                       this file
  cross/aarch64-windows-msvc.ini.in   Meson cross-file template (tokens, no machine paths)
  smoke/meson.build, smoke/hello.c    trivial target: compile + archive an ARM64 lib
  tools/setup.ps1                 venv + Meson + materialise build\cross.ini + readiness check
  tools/build-smoke.ps1           configure + build the smoke target
  tools/fetch-mesa.ps1            shallow-clone upstream Mesa into external\mesa (+ pip reqs)
```

Generated, git-ignored: `.venv\`, `build\`. Fetched source goes to `external\mesa\`.

## How to run

```powershell
cd windrv\mesa
.\tools\setup.ps1          # venv + Meson + cross.ini  (prints the readiness check)
.\tools\build-smoke.ps1    # configure + compile + archive the ARM64 smoke lib
```

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

1. **Resolve the ARM64 CRT gap** — either install the ARM64 CRT workload
   (above), or add the `runtime/` shim. Re-run `build-smoke.ps1`; now also build
   a throwaway ARM64 `.dll` to prove linking.
2. `tools\fetch-mesa.ps1` → `external\mesa`.
3. `meson setup build\mesa external\mesa --cross-file build\cross.ini` and record
   where Mesa's own configure fails. Expect the **Windows/DRM** gaps from
   [`../PANVK-SHIM.md`](../PANVK-SHIM.md) (libdrm, `mmap`, `poll`, syncobj) long
   before the shader compiler is reached.
4. Build the Win32/DRM shim described in `PANVK-SHIM.md` §5.

The shader-compiler work (SPIR-V → Valhall) is real, but it is **not** the first
wall — the C runtime and the Win32/DRM shim come first. See `PANVK-SHIM.md`.
