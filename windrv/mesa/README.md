# Mesa / PanVK build environment (Jalur B)

This directory is the **build-toolchain side of Jalur B** (the Mesa PanVK port).
It does not contain Mesa; `tools\fetch-mesa.ps1` pins it into the git-ignored
`external\mesa-v9`. It **does build it, and it now builds v9**: the pin is
`feeadb5f` + the FourFectVK Valhall-v9 stack, and the shader libraries are
pregenerated so no LLVM is needed (see "Current state" below). The Windows ARM64
`vulkan_panfrost.dll` also links with no MSVC ARM64 CRT and no VC++
redistributable dependency; see **Status**.

Nothing here touches the tablet, the KMD, or USB. Everything it writes stays
inside the repository (`.venv\`, `build\`), and both are git-ignored.

## Current state (2026-11-10)

The tree is pinned at **`feeadb5f` (Mesa 26.3.0-devel)**, not `mesa-25.0.7`:
Valhall **v9** (Mali-G57) only exists in the FourFectVK stack, and that stack is
measured against this commit. Three consequences:

| Change | Where |
| --- | --- |
| The v9 stack is applied as a base layer, before our patches | `patches-v9/fourfectvk/` (5 patches) |
| Project patches renumbered, rebased, pruned to a contiguous set | `patches/0001`–`0010` |
| Panfrost shader libraries pregenerated, so CLC/LLVM is not needed | `pregen/` + `-Dpanfrost-pregen=true` |

`tools\gen-libpan-wsl.sh` (Linux/WSL) regenerates `pregen/`; regenerating from
the same tree reproduces the committed files byte for byte. A native build of
the final tree with `-Dllvm=disabled -Dmesa-clc=auto -Dpanfrost-pregen=true`
produces a 24 MB `libvulkan_panfrost.so` containing the v9 backend, with
`mesa_clc` appearing **zero** times in the build graph — that is the end-to-end
proof that the toolchain gate is gone. See [`../V9-PANVK-PLAN.md`](../V9-PANVK-PLAN.md)
§3.4.

**The remaining gate was the kernel contract.** The stack's v9 submission path
calls `KBASE_IOCTL_*` directly (`JOB_SUBMIT` with an atoms array, `MEM_*`,
`SET_FLAGS`, `KCPU_QUEUE_*`, `VERSION_CHECK_JM` >= 11), so the Windows shim had
to speak the **kbase UAPI**; the panfrost-kmod shim alone does not compile
against it. That is now implemented as route A1 in `runtime/mali_drm.cpp`
(`kbase_ioctl()` next to `drm_dispatch()`, plus `ioctl()`/`poll()`/`read()` on
the device fd) and is compile-clean on Windows ARM64; it has not yet been run
on the tablet. Routes, evidence and the SAME_VA decision:
[`../V9-PANVK-PLAN.md`](../V9-PANVK-PLAN.md) §3.5-3.6.

**The Windows build is green (2026-11-10).** `-Kind vulkan` now produces
`build\mesa\src\panfrost\vulkan\vulkan_panfrost.dll`:

| Property | Value |
| --- | --- |
| Machine | `IMAGE_FILE_MACHINE_ARM64` |
| Size / SHA-256 | 22,635,520 B / `6ECBD7B1D3F26FF0FFB6C5BC02DCFE74078B06CC36031BB1988A9BB2CFA0E911` |
| Exports | `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion` |
| Imports | `KERNEL32`, `api-ms-win-crt-*`, `GDI32`, `USER32`, `SETUPAPI`, `ntdll` - no `VCRUNTIME140`/`MSVCP140` |
| Patch stack | 5 FourFectVK base + 10 project patches at `feeadb5f` |

The import list is the second proof (after `malitest.exe`) that the CRT-free
recipe holds for a Mesa-sized binary: the UCRT is reached through its API-set
DLLs, and there is no VC runtime to install. Four faults had to be fixed to get
here - the MSVC bitfield ABI, `PACKED` becoming a no-op, the missing POSIX
surface, and a panvk target that exported nothing at all - each written up
below. **The DLL has not been loaded on the tablet yet**, so this is a build
result, not a hardware result.

## Status (measured on this host, 2026-11-08, on the old `mesa-25.0.7` pin)

> **Historical.** These numbers were measured against the `mesa-25.0.7` pin.
> The pin is now `feeadb5f` + the v9 stack (see "Current state" above), which is
> why the DLL must be rebuilt and why "v9 is not in this DLL" below no longer
> describes the tree. The rows are kept because they prove the Windows ARM64
> toolchain and the CRT-free link recipe work; nothing in them is a v9 result.

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
| **Link Gallium panfrost GL (ARM64)** | ✅ `libgallium_wgl.dll` **14.7 MB** + `opengl32.dll` 115 KB, `COFF-ARM64` — links, but the Windows frontend cannot select panfrost (see “Next steps”) |
| **No VC++ runtime dependency** | ✅ all four DLLs import only in-box UCRT API sets + system DLLs (no `VCRUNTIME140`/`MSVCP140`) |

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
| `dep_libdrm` never found ⇒ `vk_drm_syncobj.c` not built, but PanVK calls `vk_drm_syncobj_*` | patch `0006` builds it unconditionally; its `drmSyncobj*` calls resolve against the shim |
| `__chkstk`, `_tls_index` (from `vcruntime.lib`, not installed for ARM64) | `runtime/mali_aarch64.s` + `runtime/mali_win32os.cpp` |
| `sysinfo`, `sched_yield`, `mprotect`, `dup`/`strdup`/`stricmp`, build-id | compat headers under `cross\compat\`, `runtime/mali_win32os.cpp`, `runtime/mali_posix.cpp`, patch `0005` |

**Caveat — that DLL has no v9 (superseded).** The 2026-11-09 artifact covers
Valhall **v6/v7** and **v10** (CSF) only, because upstream PanVK has no v9 and
the old pin predates the stack. That is no longer the tree: `feeadb5f` + the
FourFectVK layer adds the v9 JM backend, so a rebuild of this DLL *does* contain
v9. The kernel contract (kbase UAPI) is now implemented in the shim as route A1
— `kbase_ioctl()` beside `drm_dispatch()` in `runtime/mali_drm.cpp`, plus
`ioctl()`/`poll()`/`read()` on the device fd — so the v9 path has a kernel side
to submit against; see "Current state" above and
[`../V9-PANVK-PLAN.md`](../V9-PANVK-PLAN.md) §3.5-3.6. It has not been run on
the tablet yet. The two archeological v9 patches in `patches\experimental\` are
still parked there unapplied; they were written against the 25.0.7 Bifrost-based
`jm\` path and are superseded by the stack's own v9 files.

### MSVC/ARM64 portability fixes the v9 build found (2026-11-10)

The 25.0.7 tree configured cleanly; the v9 tree hits four portability details
that a GCC/Itanium build never sees. All were measured on this host.

**1. `PACKED` silently became a no-op.** Meson probes `packed` as a *function*
attribute, clang-cl rejects that spelling, so `util/macros.h` left `PACKED`
empty and every GPU-shared struct quietly drifted from the Linux layout. It
surfaced as `geometry.h`'s `sizeof(poly_geometry_params) == 79 * 4` assert
(320 != 316). `__attribute__((packed, aligned(4)))` *does* work on structs under
clang-cl (measured: 12 bytes/align 4 for `{ u64; u32; }`), so `tools\setup.ps1`
now asserts `-DHAVE_FUNC_ATTRIBUTE_PACKED` in the cross file instead of patching
Mesa.

**2. The MSVC bitfield ABI splits runs of different-sized types.** Measured
under clang-cl for `aarch64-pc-windows-msvc`: adjacent bitfields share an
allocation unit when their *size* matches, so `enum : 8` + `unsigned : 8` +
`unsigned : 3` + `enum : 2` + `int : 12` still pack into 4 bytes
(`pan_varying_slot` proves it — its `== 4` assert passes). One 1- or 2-byte
member in the middle breaks the run: `bool skip : 1` followed by
`unsigned _pad : 14` becomes a 1-byte unit plus a 4-byte unit, `sizeof` 8 where
the Itanium ABI gives 4. A plain (non-bitfield) `bool` after a bitfield run is
the same story — `panvk_tess_info`'s two `bool`s landed at bytes 2/3 on Itanium
but 4/5 on MSVC, `sizeof` 4 versus 8. There is no escape hatch: clang's cc1
exposes only `-mms-bitfields` (no negative form), the driver's
`-mno-ms-bitfields` is accepted and has no effect on this target, clang-cl
rejects `-Xclang -fno-ms-bitfields` ("unknown argument ignored ... did you mean
`-mno-ms-bitfields`"), and neither `__attribute__((packed))` nor
`#pragma pack(1)` merges the runs (measured: 5 bytes).

The fix is patch `0009`, which makes the three word-`memcpy`'d structs use one
uniform `unsigned` bitfield run: `pan_bi_tex_flags` and `pan_va_tex_flags`
(`pan_nir.h`, bit-positions identical to the Itanium build) and `panvk_tess_info`
(`panvk_shader.h`, where `points`/`ccw` keep their Itanium byte offsets 2 and 3
via explicit `_pad` fields). Scope stays tiny because only structs that are
`memcpy`'d or compared as a word need the exact layout, and every one of them
carries a `static_assert(sizeof(...) == N)` that fails the build when it drifts
— which is how all three were found rather than by inspection.

One divergence is *not* fixed, because nothing needs it: `pan_earlyzs_state`
(`unsigned : 2; unsigned : 2; bool : 1; unsigned : 3;`) is 12 bytes here and 1
byte on Linux. Every access is field-by-field, so the values are right. It only
becomes a bug the day that struct is `memcpy`'d, hashed or handed to the GPU
whole — hence the rule above rather than a blanket rewrite.

**3. Missing POSIX surface.** The v9 tree reaches for a handful of glibc/BSD
names the UCRT does not have, and the right fix differs per case:

*Declared, not defined - Mesa already provides them for `_WIN32`:*
`strcasecmp`/`strncasecmp` (called from `pan_env.h` and from `pan_trace.c`,
which includes only `<string.h>`) and `strndup` (Mesa's own
`src/util/strndup.c` is gated on `_WIN32` and is compiled into `libmesa_util`
here - defining it in the shim as well would be a duplicate symbol). Both were
implicit-declaration errors only because glibc's `<string.h>` declares them and
Mesa's does not; `cross/msvc-compat.h` supplies the declarations.

*Implemented in `runtime/mali_drm.cpp`:* `ppoll()` (forwards to `poll`),
`fcntl()`/`F_DUPFD_CLOEXEC` (`_dup`; the minimum-descriptor hint and
close-on-exec are meaningless here), `pread()` (seek, read, restore position;
synthetic fds get `ENOSYS`), `mincore()` (always `ENOSYS`) and `dladdr()`
(`GetModuleHandleExA` + `GetModuleFileNameA`, with `\` rewritten to `/` because
its only caller locates itself through `strrchr(path, '/')`). They are reachable
only from the CSF/dma-buf paths, one Linux-only debug heap dump and one
device-name lookup, so they exist to make the tree type-check and degrade
sensibly rather than to do work. `ppoll`'s timestamp and `pread`'s offset are
`int64_t` for the same reason our `mmap()` offset is: the UCRT's `off_t` is
32-bit and GPU VAs are not.

*Renamed, not implemented:* `strtok_r` is a macro over `strtok_s`, which takes
the same arguments and has the same `*saveptr` continuation contract. `alloca`
maps to `__builtin_alloca` and `PATH_MAX` to the Linux value (4096), because
UCRT's `_alloca` is a front-end intrinsic and `_MAX_PATH` (260) is too small for
Mesa's temp-path buffers.

*Constants and headers only:* `<linux/dma-heap.h>`, the `<strings.h>` spelling
and a declaration-only `<dlfcn.h>` (nothing built here calls `dlopen`/`dlsym`)
live in `cross/compat/`. `clockid_t` with `CLOCK_MONOTONIC_RAW`/`CLOCK_BOOTTIME`
and `_SC_CLK_TCK` are in `cross/msvc-compat.h`: every clock id resolves to the
same `QueryPerformanceCounter` reading, and `sysconf(_SC_CLK_TCK)` answers
`CLOCKS_PER_SEC` (1000) for a caller that reads `/proc/self/stat` and therefore
always sees 0 ticks. `F_OK`/`R_OK`/`W_OK`/`X_OK` live in
`cross/compat/unistd.h`, where `panvk_instance.c`'s optional `/dev/mali<N>`
probe finds them. `sys/mman.h`'s prototypes are backed by the shim's real
`mmap()` (GPU-VA lookup) with no-op `munmap`/`mprotect`.

**4. The DLL exported nothing at all.** The first successful link produced a
22.6 MB ARM64 `vulkan_panfrost.dll` whose only export was
`std::_Raise_handler` (from our C++ runtime shim) - the Vulkan loader would
have called `LoadLibrary`, found no `vk_icdGetInstanceProcAddr`, and declared
the ICD unloadable. Nothing in the compile phase hints at this; it is only
visible in the export table. On ELF the ICD entry points stay exported through
the version script (`vulkan.sym`, gated on `with_ld_version_script`) or default
visibility, and `symbols_check` exists to verify it - but that test is disabled
in this tree ("TODO: Re-enable the symbol check with Kraid"). On Windows
exports come from `vs_module_defs` instead: a `.def` generated from
`src/vulkan/vulkan_api.def.in` by `gen_vs_module_defs_normal_command`, which
every other ICD here passes (lavapipe, radv, dzn, nvk) and the v9
`src/panfrost/vulkan/meson.build` did not. Patch `0010` adds the one missing
line; all three names it lists already exist (`vk_icdGetInstanceProcAddr` in
`panvk_instance.c`, `vk_icdNegotiateLoaderICDInterfaceVersion` and
`vk_icdGetPhysicalDeviceProcAddr` in `src/vulkan/runtime/vk_instance.c`).

The general lesson matches items 1 and 2: on this target the link succeeding
says nothing about the ABI being right, so the artifact itself has to be
inspected (`llvm-readobj --coff-exports`) rather than trusted.

`tools\build-mesa.ps1 -Reconfigure` also regenerates `build\cross.ini` now.
Before, it only ran `setup.ps1` when the file was missing, so a `-D` change to
the cross file could silently never reach the compiler.

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

### Removing the VCRUNTIME140 / MSVCP140 dependency (2026-11-09)

`msvcurt.lib` is the OneCore **import** library. It satisfies the link, but some
of its members are stubs that forward to `VCRUNTIME140.dll` / `MSVCP140.dll` —
the VC++ redistributable, which is not guaranteed on a Windows 11 ARM64 install.
Linking against it is therefore not the same as being self-contained. Measured
with `llvm-readobj --coff-imports` on the tree's own outputs:

| Symbol reached through | Why it matters |
| --- | --- |
| `memcpy`, `memset`, `memmove`, `memcmp`, `memchr`, `strchr`, `strrchr`, `strstr` | compiler intrinsics; `ucrt.lib` covers only some of them on ARM64 |
| `_Mtx_lock`, `_Mtx_unlock` | `std::mutex` ABI (MSVCP140) |
| `_Throw_Cpp_error`, `_Xlength_error` | STL `[[noreturn]]` helpers (MSVCP140) |
| `std::_Raise_handler` | data import (MSVCP140) |
| `longjmp`, `_setjmpex` | `setjmp`/SEH pair (VCRUNTIME140) |
| `__CxxFrameHandler3` | C++ SEH frame cleanup (VCRUNTIME140) |

The fix is to **own those symbols** instead of bundling the redistributable:

1. `runtime\mali_vcrt.cpp` defines the `mem*`/`str*` set, `_Mtx_*`,
   `std::_Xlength_error`, `std::_Throw_Cpp_error` (both `abort()`), built with
   `-fno-builtin` so defining `memcpy` does not recurse into itself.
2. `runtime\mali_aarch64.s` adds `_setjmpex` and `longjmp` against the ARM64
   `_JUMP_BUFFER` layout (24 × u64: Frame, Reserved, X19–X28, Fp, Lr, Sp, Fpcr,
   Fpsr, D8–D15).
3. `runtime\mali_cxxrt.cpp` adds the data-import slot
   `__imp_?_Raise_handler@std@@...` (via `__asm__`, since the name is not a
   valid C++ identifier) holding the address of the definition.
4. `std::mutex` is not emulated: `_Mtx_*` is implemented against the layout of
   MSVC 14.50's `_Mtx_internal_imp_t` (80 B: `_Type`@0, the 64-B critical
   section union @8, `_Thread_id`@72, `_Count`@76) with a zero-initialised
   `SRWLOCK` stored inline. The constexpr constructor zero-inits, so no
   `_Mtx_init_in_situ` call is needed.
5. `/EHs-c-` is appended after Mesa's `/EHsc` in the cross file. Mesa compiles
   with `_HAS_EXCEPTIONS=0` and contains no `throw`/`catch`; the flag only stops
   clang from emitting `__CxxFrameHandler3` references.
6. `mali_cxxrt.lib` is listed **first** on the link line. Both it and
   `msvcurt.lib` define these symbols; the linker takes the first definition, so
   first place is what makes the shim win over the forwarding stubs.

Verification (`llvm-readobj --coff-imports`): `libgallium_wgl.dll`,
`opengl32.dll`, `vulkan_panfrost.dll` and the shim proof DLL import **only**
in-box UCRT API sets and system DLLs.

One packaging consequence: `libgallium_wgl.dll` imports `crc32` from Mesa's zlib
subproject, so **`z-1.dll` (106,496 B) must ship next to it**. That DLL is also
in-box-UCRT-only.

## The shim (`runtime\`) — what it is and is not

`runtime\` is the ARM64 C++ runtime the CRT-free recipe needs. It is **not** a
libc++/STL port. It started as the handful of symbols `msvcurt.lib` does not
provide at all (`atexit`, `std::_Raise_handler`, `operator new`/`delete`) and
grew to own the symbols `msvcurt.lib` merely *forwards* to the VC++ runtime (see
the section above). It is split into archive members so the linker pulls only
what a target needs:

| Member | Provides |
| --- | --- |
| `mali_cxxrt.cpp` | `operator new`/`delete` (+ array, sized, aligned, nothrow), `atexit` → UCRT `_crt_atexit`, `std::_Raise_handler` |
| `mali_crt_exe.cpp` | `mainCRTStartup` (argv from `GetCommandLineA`, then `exit(main(...))`) |
| `mali_crt_dll.cpp` | `_DllMainCRTStartup` → the library's `DllMain` if any |
| `mali_posix.cpp` | POSIX spellings the UCRT only exports underscored (`read`, `write`, `open`… `dup`, `strdup`, `stricmp`) |
| `mali_drm.cpp` | the libdrm-compatible shim over the KMD (device enumeration, BOs, ioctls, mmap, syncobjs) |
| `mali_win32os.cpp` | Win32-backed Linux helpers: `sysinfo`, `sched_yield`, `_tls_index` |
| `mali_tls.cpp` | C++ thread-safe statics (the `__cxa_guard_*` equivalent) and thread-local storage init |
| `mali_vcrt.cpp` | `mem*`/`str*` intrinsics, `_Mtx_*`, `std::_Xlength_error`, `std::_Throw_Cpp_error` |
| `mali_aarch64.s` | `__chkstk`, `_setjmpex`/`longjmp` |

Those last four are what makes the result self-contained: they take over the
symbols that `msvcurt.lib` would otherwise resolve by importing
`VCRUNTIME140.dll` / `MSVCP140.dll`.

Three things make this work, and all are in Mesa already:

- Mesa compiles its C++ with `-D_HAS_EXCEPTIONS=0` (meson.build line 1359), so
  the STL never pulls the SEH exception machinery
  (`__CxxFrameHandler3`, `_CxxThrowException`). `/EHs-c-` in the cross file
  removes the remaining frame-handler reference from Mesa's own `/EHsc`.
- `-Dcpp_rtti=false` keeps RTTI off, which also avoids `msvcurt.lib`'s broken
  `ti_inst.obj` (lld: *"should not refer to special section 0"*).
- The shim archive is listed before `msvcurt.lib` so its definitions win.

Prove it:

```powershell
.\tools\link-probe.ps1     # shows exactly which symbols each library adds
.\tools\build-runtime.ps1  # builds the shim and links a C exe + an STL DLL
```

The resulting DLL imports **only UCRT API sets** — no `msvcp140.dll`, no
`vcruntime140.dll` — so it stays self-contained on the target.

## First Mesa configure (2026-11-09): where it stops now

> **Resolved twice, and the second way is the one that survives.**
>
> Pinning `mesa-25.0.7` dodged `libpan` but also dodged v9, so it could not be
> the answer. The tree is now on **26.3.0-devel** with the v9 stack, and the
> CLC wall is gone because the generated sources are checked in
> (`pregen/`) and Mesa is asked for them with `-Dpanfrost-pregen=true`
> (`patches/0008-panfrost-pregen.patch`). `-Dllvm=disabled` is then genuinely
> satisfiable and `mesa_clc` never enters the build graph. The section below is
> kept because it explains the constraint that shaped the pin.

`tools\setup.ps1` writes a cross file whose link recipe is the shim above
(`-fuse-ld=lld`, `/nodefaultlib`, the SDK libs, the shim). With that, Meson's
compiler sanity checks **pass** and the configure runs far:

```powershell
meson setup build\mesa ..\..\external\mesa-v9 --cross-file build\cross.ini `
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
  cross/compat/                   headers Windows lacks: sys/{mman,ioctl,ioccom,sysinfo,sysmacros}.h, linux/{ioctl,types,dma-heap}.h,
                                  strings.h, dlfcn.h, sched.h, unistd.h, poll.h, xf86drm.h (libdrm API subset)
  runtime/mali_cxxrt.cpp          C++ runtime shim: operator new/delete, atexit, _Raise_handler
  runtime/mali_crt_exe.cpp        mainCRTStartup
  runtime/mali_crt_dll.cpp        _DllMainCRTStartup
  runtime/mali_posix.cpp          POSIX/oldnames aliases (write/dup/strdup/stricmp/strcasecmp/…)
  runtime/mali_drm.cpp            libdrm + libsyncobj + kbase UAPI shim over the Mali KMD
  runtime/mali_win32os.cpp        sysinfo / sched_yield / _tls_index
  runtime/mali_tls.cpp            C++ thread-safe statics + thread_local TLS guard
  runtime/mali_vcrt.cpp           mem*/str*, _Mtx_*, _Xlength_error, _Throw_Cpp_error
  runtime/mali_aarch64.s          __chkstk, _setjmpex / longjmp
  patches/*.patch                 portability patches, contiguous 0001-0010,
                                  applied by tools/patch-mesa.ps1
  patches-v9/fourfectvk/*.patch   the vendored FourFectVK v9 stack (5 patches),
                                  applied *before* patches/ by the same script
  patches/experimental/           v9-enable patches, deliberately NOT applied (see its README)
  pregen/                         pregenerated libpan/libpoly C sources + SHA256SUMS
                                  (see pregen/README.md); consumed by -Dpanfrost-pregen
  smoke/meson.build, smoke/hello.c    trivial target: compile + archive an ARM64 lib
  smoke/link_probe.c, link_probe.cpp  the sources link-probe.ps1 measures with
  tools/setup.ps1                 venv + Meson + materialise build\cross.ini + readiness check
  tools/fetch-mesa.ps1            shallow-clone upstream Mesa at the pinned commit into external\mesa-v9
  tools/fetch-flexbison.ps1       vendor win_flex/win_bison into external\winflexbison (GL build)
  tools/patch-mesa.ps1            reset the checkout, apply patches-v9 + patches, stage pregen\ (idempotent)
  tools/gen-libpan-wsl.sh         Linux/WSL: regenerate pregen\ with Mesa's own host tools
  tools/build-mesa.ps1            patch, configure, compile → Vulkan (`-Kind vulkan`) or
                                  OpenGL (`-Kind gl`) DLLs
  tools/build-runtime.ps1         build runtime\ + prove a C exe and an STL DLL link
  tools/link-probe.ps1            measure what each SDK library contributes to an ARM64 link
  tools/build-smoke.ps1           configure + build the smoke target
```

Generated, git-ignored: `.venv\`, `build\`. Fetched source goes to `external\mesa-v9\`.

## How to run

```powershell
cd windrv\mesa
.\tools\setup.ps1          # venv + Meson + cross.ini  (prints the readiness check)
.\tools\build-runtime.ps1  # build the C++ runtime shim + prove exe/dll link
.\tools\fetch-mesa.ps1     # pin upstream Mesa at the pinned commit into external\mesa-v9
.\tools\patch-mesa.ps1     # reset the checkout, apply the v9 stack + patches, stage pregen\
.\tools\build-mesa.ps1 -Kind vulkan   # patch + configure + compile → vulkan_panfrost.dll
.\tools\fetch-flexbison.ps1           # GL only: vendor win_flex/win_bison (git-ignored)
.\tools\build-mesa.ps1 -Kind gl       # → libgallium_wgl.dll + opengl32.dll
.\tools\link-probe.ps1     # show what each SDK library contributes to the link
.\tools\build-smoke.ps1    # configure + compile + archive the ARM64 smoke lib
```

`build-mesa.ps1` uses separate build directories per `-Kind`
(`build\mesa` for Vulkan, `build\mesa-gl` for OpenGL) and runs `patch-mesa.ps1`
itself. It wipes the build directory when `build\cross.ini` is newer than
`build.ninja`, because `meson --reconfigure` silently ignores changes to
`c_link_args` (which is where the shim order lives), and touches a fresh
`meson setup` only when the cross file or the build graph changed
(`-Reconfigure` to force). `-ConfigureOnly` stops after the configure, which is
the fast way to check a new option or patch.

The GL build needs `win_flex`/`win_bison` for Mesa's GLSL front-end.
`fetch-flexbison.ps1` vendors winflexbison 2.5.25 into the git-ignored
`external\winflexbison`; nothing is installed on the host. `build-mesa.ps1`
also redirects `TEMP`/`TMP` into `build\tmp` for that step, because `win_flex`
writes a scratch file into `%TEMP%` and intermittently fails to delete it.
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

The build works *and* v9 is in it. What is left is the kernel contract:

1. ~~**v9 is the wall.**~~ **Solved by the stack, not by us.** The FourFectVK
   layer (vendored in `patches-v9/fourfectvk/`, applied by `patch-mesa.ps1`)
   adds the Valhall-v9 JM backend, including the v9 genxml, the Bifrost/Valhall
   split of `cmd_draw`, transform feedback and geometry prefixes. Gallium
   panfrost GL remains **unreachable on Windows** (Mesa's WGL frontend selects
   only `d3d12`/`zink`/`llvmpipe`/`softpipe`, and the tablet's GDI exposes
   **0 pixel formats**), so GL stays Zink over this ICD. See `../DEPLOY.md` §3.
2. **The new wall: the kernel contract.** The stack submits with
   `ioctl(fd, KBASE_IOCTL_JOB_SUBMIT, ...)` and a kbase atoms array; it does not
   go through `pan_kmod`'s panfrost backend. So `mali_drm.cpp` must gain an
   `ioctl()` + kbase dispatch, translating onto our existing `IOCTL_MALI_*`
   v2 ABI. The exact request set is in `../V9-PANVK-PLAN.md` §3.5.
3. **Then build the Windows ARM64 DLL** with `build-mesa.ps1 -Kind vulkan`
   (already `-Dpanfrost-pregen=true -Dpanfrost-kmds=kbase,panthor`), and
   validate on the tablet: `vkCreateDevice` → `vkCmdFillBuffer` → first
   triangle. After that, `vkd3d`/`DXVK` become reachable, since they are just
   Vulkan clients.
4. The `WDDM-PATH.md` work (render-only WDDM miniport) is what makes the GPU
   appear to Task Manager/DXGI; independent of which userspace API the driver
   exposes, and it wants the same KMD work (multi-AS, non-contiguous BOs,
   VM_BIND, fences) that kbase's VM model does.

The shader-compiler work (SPIR-V → Valhall) is real; the Win32/DRM shim that
used to come first is now written and exercised by the build — it just needs
the kbase dialect on top.
