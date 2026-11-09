# Valhall v9 PanVK — the gate is closed (2026-11-09 research)

The single blocker for Vulkan, Zink/OpenGL, DXVK and vkd3d was "no Valhall v9
backend exists". That is no longer true: there is an actively maintained patch
stack for **exactly this GPU** (Mali-G57 MC2 / Helio G99 / MT6789) and upstream
Mesa has moved a long way since our `25.0.7` pin.

This document records the findings and what they change. Nothing here has been
built yet.

## 1. What we assumed

`windrv/GRAPHICS-STACK.md` §2.2/§8 said PanVK supports v6/v7 (Bifrost JM) and
v10 (CSF) but not v9, evidenced by:

- `external/mesa/docs/drivers/panfrost.rst` (25.0.7) — empty Vulkan cell for G57.
- `src/panfrost/vulkan/meson.build` — `jm_archs = [6, 7]`.
- `src/panfrost/vulkan/valhall/` contains only `.gitkeep` (re-verified).
- Mesa 24.3 commit `2fcf9e4` "panvk: Don't pretend we can compile for v9."

All of that is still true **for Mesa 25.0.7**. The conclusion "write a new
backend from scratch" was the part that no longer holds.

## 2. What exists now

### 2.1 FourFectVK — `Noysz/panvk-g99-jm`

> "Mesa PanVK driver work for **Mali-G57 MC2 / Helio G99**, using the Valhall v9
> Job Manager backend. This repository contains the patch stack, tests, results
> and Winlator test builds."

- Actively maintained (last roadmap update 2026-10-28, test9 release).
- Target hardware is our SoC family (G57 MC2 = MT6789/Helio G99). Reported test
  device: Infinix Note 40 4G (X6853), **kbase r54p1**, Android 16.
- Scope reached: "Native compute, offscreen graphics, X11 presentation and
  **DXVK bring-up** have passed their recorded tests."
- Validation: 5,658 tracked CTS cases — 3,754 Pass, 1,890 NotSupported,
  **14 Fail** (TES / transform feedback). "Not Vulkan-conformant", experimental.
- Delivered as a Winlator custom-driver ZIP (`libvulkan_panfrost.so` +
  `libc++_shared.so` + `libdrm.so` + `libz.so.1`), i.e. **Android/arm64 ELF**, not
  Windows.

### 2.2 Base and patch order

Per `patches-upstream/README.md` and `docs/test9/build-order.md`:

- Base: upstream Mesa **`feeadb5f` = 26.3.0-devel** (2026-10-05). Our pin is
  `mesa-25.0.7`.
- Candidate stack: `7f28e47d`, 5 patch groups applied in order with `git apply`
  (they are plain diffs, not mailbox commits):
  1. `patches-upstream/0001-base-upstream-rebase-52a7497a.patch` — the
     funnymdzz **kbase `pan_kmod` backend** (a vendor-kbase kmod for Mesa's new
     `pan_kmod` abstraction), the panvk-g99-jm `0015`–`0059` work 3-way merged
     onto upstream (Android native surface, AFBC swapchains via the vendor
     `IMapper`, one kbase `JOB_SUBMIT` per `vkQueueSubmit`, forward pixel kill on
     v9, RO input attachment tile loads, MSAA storage images, async submission
     with `sync_file` fences).
  2. `0002-fourfectvk-0060-0077-frame-prof.patch` — tessellation, geometry
     shaders, transform feedback, robustBufferAccess2, lazy device memory, shared
     scratch, fault report, `PANVK_FRAME_PROF`.
  3–5. performance knobs, UBO-push / clip-cull driconf, v9 timestamp queries.
- Build recipe (both targets): `-Dvulkan-drivers=panfrost -Dgallium-drivers=
  -Dpanfrost-kmds=kbase,panthor -Dopengl=false -Dgles2=disabled`.
  `-Dpanfrost-kmds=` is a **newer Meson option we do not have in 25.0.7**.

### 2.3 Upstream trajectory (context, not a dependency)

- Mesa merged the Valhall prep series in **24.3** (Phoronix, 2024-08-24,
  MR 30736 + MR 29639).
- PanVK is now described as covering "Valhall (v9–v13)" in later mirrors, and
  Mesa 26.1 (2026) saw heavy PanVK work; a Rust shader compiler ("Kraid") landed
  in the Mesa tree 2026-06-03, and Mesa 27.0 plans to default to Kraid for Mali.
- **Implication:** pin deliberately. `26.3.0-devel` at `feeadb5f` is the base
  the patch stack is actually validated against. A Mesa in which v9 support is
  upstream would be better long-term, but it must be verified to include the
  **v9 + Job Manager** combination (v9 is JM, not CSF) — that is precisely what
  the patch stack adds.

## 3. What this changes

`Jalur B` is no longer "write a Valhall-JM v9 backend". It becomes a **Mesa
upgrade + patch-stack port**, which is bounded, testable work:

1. **Rebase the port**: `mesa-25.0.7` → `feeadb5f` (26.3.0-devel).
2. **Re-apply our 9 Windows patches** (`windrv/mesa/patches/0001…0011`, applied
   by `tools/patch-mesa.ps1`) against the new base. Expect conflicts; the
   libdrm/POSIX surface is the risky part because 26.x introduced `pan_kmod`.
3. **Apply the FourFectVK stack** (`patches-upstream/0001…0005`).
4. **Re-link `vulkan_panfrost.dll`** for Windows ARM64 with the CRT-free recipe.
5. **Verify on the tablet** through our KMD: `vkCreateDevice` →
   `vkCmdFillBuffer` (Jalur A already proves the KMD path) → first triangle.

### 3.1 Winsys: which contract to implement

Mesa 26.x's `pan_kmod` has explicit kmod backends selected by
`-Dpanfrost-kmds=`. Two options:

- **Option A: adopt Mesa's kbase backend.**
  Build with `-Dpanfrost-kmds=kbase` and make our KMD answer the **kbase ioctl
  ABI** that backend speaks (`DRM_IOCTL_MALI_*`: `MEM_ALLOC`, `JOB_SUBMIT`,
  `VM_BIND`, `SET_FLAGS`, `GET_VERSION`, eventfd/fence handling). This is the
  path FourFectVK validates and it is maintained upstream-bound. Cost:
  `src/panfrost/lib/kmod/kbase_kmod.c` is **2,045 lines** of assumptions (VM
  zones, heap allocators, timeline fences, an exec VA zone — patch `0004` grew it
  from 4 MiB to 256 MiB) that our KMD would have to satisfy. That is a KMD
  project in its own right.
- **Option B (recommended after the dry-run, §3.2): keep our libdrm/POSIX
  shim.** `src/panfrost/lib/kmod/panfrost_kmod.c` is **830 lines** and talks
  only `DRM_IOCTL_PANFROST_*`, which `windrv/mesa/runtime/mali_drm.cpp` already
  emulates almost completely: `GET_PARAM`, `CREATE_BO`, `MMAP_BO`,
  `GET_BO_OFFSET`, `MADVISE`, `WAIT_BO`, `SUBMIT`, `PERFCNT_ENABLE`,
  `PERFCNT_DUMP`, plus the entire `DRM_IOCTL_SYNCOBJ_*` family including
  timeline. The 26.3 delta is `SYNC_BO`/`BO_SYNC`, `QUERY_BO_INFO`,
  `SET_LABEL_BO` and `drmCloseBufferHandle` — small, most of them no-ops.

The first draft of this document recommended Option A. The dry-run reversed
that: Option A is a 2,045-line KMD contract we do not need yet, while Option B's
gap is four ioctls. Revisit Option A only if we later choose to align the KMD
with the vendor kbase ABI.

Note the two are not exclusive: Option A's kbase ioctls can live beside the
existing v2 IOCTLs, and Jalur A (our own ICD) keeps working either way.

### 3.2 Dry-run results (measured 2026-11-09)

Base: Mesa `feeadb5f` (26.3.0-devel) fetched into `external/mesa-v9`. Patch
stack: `patches-upstream/0001…0005` of `Noysz/panvk-g99-jm` at `08aa2e62`
(2026-11-09), vendored into `windrv/mesa/patches-v9/fourfectvk/`.

**All five applied cleanly** with `git apply` (one whitespace warning only).

v9 support comes *entirely* from the patch stack — the base has none of it:

| | base `feeadb5f` | after the stack |
|---|---|---|
| `jm_archs` | `[6, 7]` | `[6, 7, 9]` |
| arch loop | `[6, 7, 10, 11, 12, 13, 14]` | `[6, 7, 9, 10, 11, 12, 13, 14]` |
| `src/panfrost/vulkan/valhall/` | `.gitkeep` only | `.gitkeep` only |
| `valhall_files` | `[]` | `[]` |
| `src/panfrost/lib/kmod/` | – | `pan_kmod.c`, `panfrost_kmod.c`, `panthor_kmod.c`, `kbase_kmod.c` |

There is **no v9-specific source directory**: arch 9 reuses the generic
`panvk_vX_*.c` templates plus the `jm/` submission backend. 135 files changed
across 16 directories.

**Our nine Windows patches** (`windrv/mesa/patches/`), `git apply --check` on top
of the stack:

| patch | result |
|---|---|
| `0002-windows-os-mman` | applies |
| `0005-windows-vk-image-drm-modifier` | applies |
| `0009-windows-vk-drm-syncobj` | applies |
| `0010-windows-renderonly-winsys-handle` | applies |
| `0001-portable-bi-index-key` | **path move only** — `compiler/bifrost_compile.c` → `compiler/bifrost/bifrost_compile.c`; the MSVC bit-field fix is still mandatory |
| `0003-windows-panvk-drm-ids` | **context drift** in `panvk_physical_device.c` (`get_drm_device_ids` is still there) |
| `0007-windows-panvk-build-id` | **context drift** in `panvk_instance.c`; the guard must now also cover `copy_build_id_to_sha1()` (line ~342) |
| `0008-windows-panvk-cache-uuid` | **obsolete** — `get_cache_uuid()` is gone; `device->cache_uuid` is now BLAKE3 over `instance->driver_build_sha` (`panvk_physical_device.c:436-446`), so fixing 0007 fixes it |
| `0011-windows-panfrost-winsys-handle-int` | **context drift**, and it patches Gallium panfrost, which is unreachable on Windows (`GRAPHICS-STACK.md` §3.1) |

Net: three to four patches need rebasing, all mechanical. **No architectural
conflict** between the Windows port and the v9 stack.

### 3.3 New blocker found: `libpan` needs CLC, and CLC needs LLVM

Mesa 25.1 added `src/panfrost/libpan/`: OpenCL helper kernels (`libpan_copy.cl`,
`query_pool.cl`, `draw_helper.cl`, `indirect_dispatch.cl`, plus the stack's
`tessellation.cl`, `xfb_helper.cl`, `geometry.cl`) compiled to SPIR-V at build
time by `mesa_clc`, then turned into C++ by `vtn_bindgen2`. `panvk` links
`idep_libpan_per_arch[arch]` unconditionally
(`src/panfrost/vulkan/meson.build:191`), so **v9 cannot be built without it**.

`mesa-25.0.7` has **no `libpan` at all** — which is exactly why our current build
never needed LLVM. Measured on this host:

- `meson.build:1067` — `with_llvm.enable_if(with_clc, error_message : 'CLC
  requires LLVM')`.
- `src/compiler/clc/meson.build` needs `dep_clang`, `dep_llvm`,
  `dep_llvmspirvlib`, `dep_spirv_tools`, `dep_clc` (libclc) and zstd.
- 26.3 ships **no** wrap for spirv-tools, spirv-llvm-translator or libclc; they
  are expected as system packages.
- Visual Studio's LLVM 20.1.8 here has only `bin/` and `lib/` (**no `include/`**,
  no `llvm-c`/`clang-c` headers), and `clang --print-targets` offers **no
  `spirv`/`spirv64` target** — so it can neither build CLC nor pre-compile the
  kernels itself.

Escape hatch, measured in the same tree: with `-Dmesa-clc=system` Mesa skips CLC
entirely and `with_llvm` is no longer forced (`meson.build:1056-1068`), but it
then requires **prebuilt native** `mesa_clc.exe` *and* `vtn_bindgen2.exe` on
`PATH`.

## 4. Risks / open questions

- **Windows, not Android.** Every published result is on arm64 **Android**
  (Winlator). Nothing here has been shown to run as a native Windows ARM64
  `vulkan_panfrost.dll`; our port + shim is still the untested half. This is the
  main unknown and it is ours to close.
- **Rebase conflicts.** 9 Windows patches + 5 upstream-stack patches on a
  ~9-month Mesa delta. `patch-mesa.ps1` must be re-validated, not hand-edited.
- **`pan_kmod` semantics.** Mesa 26.x expects VM zones, sparse/heap binding and
  kbase-style fences; our KMD currently has identity BOs, one AS, poll-based
  fences. Option A pulls real KMD work (multi-AS, non-contiguous BOs, VM_BIND,
  fence/eventfd) forward — the same items already listed as the WDDM
  prerequisite in `ROADMAP.md` Phase 5. The two tracks converge.
- **Toolchain — now the top blocker (§3.3).** `libpan` forces `mesa_clc`, which
  needs LLVM + Clang + SPIRV-Tools + SPIRV-LLVM-Translator + libclc. This host
  has no LLVM headers and a clang with no SPIR-V target. The Kraid Rust compiler
  becoming default in 27.0 is one more reason to pin at 26.3.0-devel.
- **kbase version.** FourFectVK validates against kbase **r54p1**; our vendor
  tree is **r32p1**. Mesa's kbase backend talks the *ioctl ABI*, which is stable
  enough, but flag this if ioctl semantics differ.
- **Licensing.** Mesa is MIT; the kbase backend is the vendor kbase ABI. No new
  licence obligation is introduced beyond what the repo already ships.

## 5. Next steps

Steps 1–3 are **done** — results in §3.2.

1. ~~Fetch Mesa `feeadb5f` into `external/mesa-v9`.~~ Done. Correction to the
   earlier wording: the base does **not** contain v9 — `valhall/` is empty and
   arch 9 is missing from the arch loop. The support is entirely in the stack.
2. ~~Dry-run `patches-upstream/0001...0005`.~~ Done — all apply cleanly;
   vendored under `windrv/mesa/patches-v9/fourfectvk/`.
3. ~~Port `windrv/mesa/patches/*`.~~ Done as a dry-run: four apply, three to
   four need mechanical rebasing, `0008` is obsolete.

Still open, in order:

4. **Decide the CLC toolchain route (see 3.3)** — the gating decision now:
   - (a) portable LLVM+Clang SDK extracted into `external/` (self-contained and
     matching how the WDK and winflexbison are already fetched, but it also needs
     SPIRV-Tools, SPIRV-LLVM-Translator and libclc built), or
   - (b) prebuilt native `mesa_clc.exe` + `vtn_bindgen2.exe` as host tools with
     `-Dmesa-clc=system -Dgallium-rusticl=false` (cheap, but the binaries arrive
     from outside and must be trusted and version-checked).
5. Add the four `DRM_IOCTL_PANFROST_*` cases to
   `windrv/mesa/runtime/mali_drm.cpp`.
6. Rebase the patches for real and update `patch-mesa.ps1` — it currently starts
   with `git checkout -- .`, which would now wipe the patch stack underneath; it
   needs an explicit "base layer" step. Then re-link `vulkan_panfrost.dll` for
   Windows ARM64.
7. Verify on the tablet: `vkCreateDevice`, then `vkCmdFillBuffer`, then a first
   triangle.
8. Only then touch the KMD (multi-AS, non-contiguous BOs, VM_BIND, fences) — the
   same work WDDM needs, so the two tracks converge.

Do not start step 8 before step 7; the KMD shape follows what the winsys and the
first triangle actually require.

## 6. References

- `Noysz/panvk-g99-jm` — FourFectVK patch stack, README + `patches-upstream/README.md`
  + `docs/test9/build-order.md` + `docs/roadmap-phase8-13.md`.
- Mesa MR 30736 "panvk: Valhall preparation" (Mesa 24.3), MR 29639.
- Mesa 24.3 commit `2fcf9e4` "panvk: Don't pretend we can compile for v9."
- Mesa 25.0 PanVK status (Collabora, "Mesa 25.0: PanVK moves towards production
  quality") — Vulkan 1.1 on v10.
- Phoronix: "Mesa 27.0 Switching To Kraid Compiler By Default For Arm Mali GPUs".