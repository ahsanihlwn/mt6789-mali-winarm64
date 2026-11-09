# Valhall v9 PanVK — the gate is closed (2026-11-09 research)

The single blocker for Vulkan, Zink/OpenGL, DXVK and vkd3d was "no Valhall v9
backend exists". That is no longer true: there is an actively maintained patch
stack for **exactly this GPU** (Mali-G57 MC2 / Helio G99 / MT6789) and upstream
Mesa has moved a long way since our `25.0.7` pin.

This document records the findings and what they change. Nothing here has been
built yet.

> **Status update (2026-11-10): both blockers named in §3 are now closed.**
>
> - The `libpan`/CLC toolchain gate is **solved**; a v9 PanVK builds from
>   checked-in, pregenerated shader sources with `-Dllvm=disabled` (§3.4).
> - The winsys question in §3.1 was **answered by measurement, against our
>   earlier choice**: the stack's v9 submission path is kbase-only, so a
>   `panfrost`-kmod shim cannot drive it (§3.5).
>
> Remaining work is the Windows kbase-UAPI layer, not a toolchain problem.

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

1. ~~**Rebase the port** to `feeadb5f` (26.3.0-devel).~~ Done.
2. ~~**Re-apply our Windows patches** against the new base.~~ Done: the set is now
   `0001`-`0008`, contiguous; `tools/patch-mesa.ps1` applies the stack layer,
   then these, then stages `pregen/`. No conflicts remain.
3. ~~**Apply the FourFectVK stack**.~~ Done - vendored under `windrv/mesa/patches-v9/fourfectvk/`.
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

**This choice was wrong, and §3.5 says why:** the *stack* does not submit
through `pan_kmod`'s panfrost backend at all. Its `jm/panvk_vX_gpu_queue.c` was
replaced wholesale with kbase atom submission on the raw kbase fd, so with
`panfrost_kmod` the file does not even compile. Option B is not a smaller
version of Option A here; it is a different (and unusable) contract.

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

Measured on this machine (2026-11-09): **WSL2 Ubuntu 26.04.1 is installed** and
already carries clang/LLVM 21.1.8 with the `spirv`, `spirv32` and `spirv64`
targets, plus `llvm-21-dev`, `libclang-21-dev`, `meson`, `ninja`, `cmake` and
`pkg-config`. Ubuntu 26.04 also offers the three pieces this host lacks —
`libclc-21-dev`, `libllvmspirvlib-21-dev` (SPIRV-LLVM-Translator) and
`spirv-tools-dev` — and they can be pulled with `apt-get download` and unpacked
with `dpkg -x` **inside `external/`**: no install, no root, nothing changed
outside the project folder.

That opens a third route, and it is by far the cheapest: run `mesa_clc` and
`vtn_bindgen2` as **Linux host tools** under WSL and feed the generated
`libpan_v*.cpp` / `.h` into the Windows build. Both are build-time-only tools and
their output is plain C++ with no ABI dependency, which is exactly what a normal
cross build does — the host half just runs on Linux instead of Windows.

### 3.4 The CLC blocker is closed (measured 2026-11-10)

The §3.3 route works. Implementation, all inside this repo:

- **Packages without installing anything.** `apt-get download` + `dpkg -x`
  into `external/wsl-deps/` gives `libclc-21-dev` (the `.spv` builtins),
  `libllvmspirvlib-21-dev`, `spirv-tools-dev` (+ headers) and, for Meson's own
  Python needs, `python3-mako` / `python3-markupsafe`. Three `.pc` files get
  their `prefix=`/`libdir=` rewritten to point at the unpacked tree.
  `libdrm` comes from Mesa's own Meson wrap.
- **Host tools built with Meson in the Mesa tree.** `mesa_clc`,
  `vtn_bindgen2`, `panfrost_compile` and the packers all build from the pinned
  26.3 tree with stock Ubuntu clang 21 (which does have the `spirv` target).
- **Generation script.** `windrv/mesa/tools/gen-libpan-wsl.sh` produces, for
  every architecture (`4..14`): `libpan_v<ver>.spv` -> `libpan_v<ver>.{cpp,h}`
  and `libpan_shaders_v<ver>.{h,c}`, plus `libpoly.{cpp,h}`. Output: 42 files,
  7.1 MB, committed under `windrv/mesa/pregen/` with `SHA256SUMS`.
- **Mesa side** (`patches/0008-panfrost-pregen.patch`): a `panfrost-pregen`
  boolean option. When set, `src/panfrost/libpan/meson.build` and
  `src/poly/cl/meson.build` take the checked-in `files()` instead of the three
  `custom_target`s, and the panfrost drivers stop counting as CLC consumers, so
  `with_clc` stays false and `with_llvm.enable_if(with_clc)` never fires.

Three details that were not obvious and cost the most time:

1. `src/poly/cl` is a **second** CLC consumer on the panfrost path: enabling
   `vulkan-drivers=panfrost` sets `with_poly`, and `src/poly/nir/*.c` include
   `poly/cl/libpoly.h`. Both had to be pregenerated.
2. The pregenerated files must sit at the paths upstream would have generated
   them to (`src/panfrost/libpan/`, `src/poly/cl/`), not in a subdirectory:
   `libpan.h` includes `"libpan_v9.h"` and poly includes `"poly/cl/libpoly.h"`,
   both resolved through existing include dirs.
3. Meson does not quote include paths containing spaces, and this repo lives
   under `iOS 27`. `pkg-config` handles the escaping fine, Meson does not
   (`clang++ -I/mnt/d/PORT/iOS '/mnt/d/...'`), so the build runs in a
   space-free scratch tree (`$MT6789_WSL_SCRATCH`, default
   `~/.cache/mt6789-mesa`) and only the generated C comes back. Windows 8.3
   short names are disabled on this volume, so there is no space-free alias.
   Also note `mesa-clc` has no `disabled` value; `-Dmesa-clc=auto` plus the
   option above is what actually works.

**Validation.** A native build of the final tree (pinned commit + vendored v9
stack + all 8 project patches + `pregen/`) with
`-Dllvm=disabled -Dmesa-clc=auto -Dpanfrost-pregen=true`:
`mesa_clc` appears **0 times** in `build.ninja`, and the link produces
`libvulkan_panfrost.so` (24,173,040 bytes) containing the v9 backend and the
`libpan` printf blobs. Regenerating the artifacts from the same tree
reproduces the committed files byte for byte.

### 3.5 The v9 stack is kbase-only (measured 2026-11-10)

This overturns §3.1. Building the final tree with
`-Dpanfrost-kmds=panfrost` (the option §3.1 chose) **fails**:

```
panvk_vX_gpu_queue.c:1737: error: 'PANVK_KBASE_SYNC_PAYLOAD_NONE' undeclared
panvk_vX_gpu_queue.c:1876: error: implicit declaration of
                                 'panvk_kbase_sync_set_pending'
```

The cause is structural, not a missing include. In
`src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c` the only `#ifdef` is a three-line
include block (`HAVE_PAN_KMOD_KBASE`, lines 50-71). Everything after it - the
whole submission path - was replaced by the stack with kbase code that talks to
the kernel **directly**:

```c
k->fd = device->drm_fd;                                  /* line 1918 */
struct kbase_ioctl_job_submit submit = {
   .addr = (uint64_t)(uintptr_t)atoms,
   .nr_atoms = nr_atoms,
   .stride = PANVK_KBASE_ATOM_STRIDE,
};
if (ioctl(k->fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {      /* line 1182 */
```

and the same fd also carries the `DRM_IOCTL_SYNCOBJ_*` family that
`mali_drm.cpp` already emulates. `pan_kmod`'s panfrost backend is bypassed
entirely.

What the v9 path actually needs from the kernel (JM only; the `CS_*` ioctls in
the tree belong to the CSF architectures 10+):

| ioctl | used for |
|---|---|
| `VERSION_CHECK_JM` | must report `major >= 11`; also what distinguishes JM from CSF |
| `JOB_SUBMIT` | **atoms array** (`addr`, `nr_atoms`, `stride`), not a single chain |
| `MEM_ALLOC_EX` / `MEM_ALLOC`, `MEM_FREE`, `MEM_QUERY` | BOs; kbase returns a handle *and* a GPU VA |
| `MEM_IMPORT`, `MEM_SYNC`, `MEM_ALIAS` | imported buffers, cache maintenance, aliasing |
| `MEM_JIT_INIT`, `MEM_COMMIT`, `MEM_EXEC_INIT` | executable/on-demand memory |
| `SET_FLAGS` | VM map/unmap and memory attributes |
| `GET_GPUPROPS`, `GET_CPU_GPU_TIMEINFO` | device properties, timestamps |
| `KCPU_QUEUE_CREATE` / `_ENQUEUE` / `_DELETE` | `BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL`, `BASE_KCPU_SYNC` |

So the shim has to speak the **kbase UAPI**, and "keep the panfrost kmod" is
not an option at all. Three ways to close it:

- **(A1) kbase UAPI in the user-mode shim (recommended).** `mali_drm.cpp`
  already owns `drmIoctl(fd, request, arg)` and a dispatch table; add an
  `ioctl()` entry point next to it with `kbase_dispatch()`, translating each
  request onto the existing `IOCTL_MALI_*` v2 ABI. The KMD stays as validated
  and Jalur A keeps working. Cost is bounded by the table above.
- **(A2) kbase UAPI in the KMD.** Native and closest to the vendor model
  (`midgard/mali_kbase` r32p1 is in the tree, read-only). More invasive: it
  moves semantics into kernel mode, needs re-signing and re-validation, and
  duplicates work A1 can do in user space where we can iterate without
  reboots.
- **(A3) Re-point the stack's submission path at `panfrost_kmod`.** Rejected:
  it means rewriting the only validated v9 submit code, i.e. forking the one
  thing that is known to work.

Mapping is mostly favourable: `JOB_SUBMIT`'s atoms array is a list of job
chains, which is what `IOCTL_MALI_JOB_SUBMIT` already takes, and our KMD
already does page tables, per-BO GPU addresses, job slots and the JS IRQ. What
is genuinely new: kbase's non-contiguous VM model (`SET_FLAGS`), seqno-based
fences (`KCPU` + `MEM_SYNC`), atom dependencies, and cache maintenance. The
first four of those are on the WDDM prerequisite list anyway (`ROADMAP.md`
Phase 5), so the tracks still converge.

## 4. Risks / open questions

- **Windows, not Android.** Every published result is on arm64 **Android**
  (Winlator). Nothing here has been shown to run as a native Windows ARM64
  `vulkan_panfrost.dll`; our port + shim is still the untested half. This is the
  main unknown and it is ours to close.
- **Rebase conflicts — resolved.** 8 project patches + 5 stack patches now apply
  cleanly from a pristine checkout via `patch-mesa.ps1`, verified by running it
  on a reset tree. Re-run it after any pin change rather than hand-editing.
- **`pan_kmod`/kbase semantics.** The v9 path expects kbase's VM model, atoms
  and seqno fences; our KMD currently has identity-mapped BOs, one AS and
  poll-based completion. Route A1 (§3.5) absorbs the first step in user space,
  but real VM work (multi-AS, non-contiguous BOs, `SET_FLAGS`/VM_BIND,
  fence/eventfd) is still coming - the same items already listed as the WDDM
  prerequisite in `ROADMAP.md` Phase 5. The two tracks converge.
- ~~**Toolchain (§3.3).**~~ **Closed** (§3.4): the panfrost shader libraries are
  pregenerated on Linux and checked in, so the Windows build needs no LLVM, no
  `mesa_clc` and no SPIR-V target. Regenerating them is one script, and the
  result is verified byte-identical. The Kraid Rust compiler becoming default in
  Mesa 27.0 is one more reason to stay pinned at 26.3.0-devel.
- **kbase version.** FourFectVK validates against kbase **r54p1**; our vendor
  tree is **r32p1**. Mesa's kbase backend talks the *ioctl ABI*, which is stable
  enough, but flag this if ioctl semantics differ.
- **Licensing.** Mesa is MIT; the kbase backend is the vendor kbase ABI. No new
  licence obligation is introduced beyond what the repo already ships.

## 5. Next steps

Steps 1–3 were done as a dry-run (§3.2); step 4 (the toolchain gate) is now
**closed** (§3.4) and the patch stack is rebased and applied for real.

1. ~~Fetch Mesa `feeadb5f` into `external/mesa-v9`.~~ Done. Correction to the
   earlier wording: the base does **not** contain v9 — `valhall/` is empty and
   arch 9 is missing from the arch loop. The support is entirely in the stack.
2. ~~Dry-run `patches-upstream/0001...0005`.~~ Done — all apply cleanly;
   vendored under `windrv/mesa/patches-v9/fourfectvk/`.
3. ~~Port `windrv/mesa/patches/*`.~~ **Done for real.** The set is now
   contiguous (`0001`–`0008`): `0001` moved to `compiler/bifrost/`, `0003` and
   `0005` rebased, the cache-UUID patch deleted (BLAKE3 over `driver_build_sha`
   makes it dead) and the Gallium winsys patch dropped (unreachable on Windows).
   `patch-mesa.ps1` now applies base layer → project layer → stages `pregen/`,
   and is idempotent from a pristine checkout.
4. ~~Get the CLC toolchain.~~ **Done** (§3.4):
   `windrv/mesa/tools/gen-libpan-wsl.sh` regenerates everything under WSL and
   the artifacts are committed in `windrv/mesa/pregen/`.

Still open, in order:

5. **Speak kbase in the shim (§3.5, route A1).** Add `ioctl()` +
   `kbase_dispatch()` beside `drmIoctl()` in `windrv/mesa/runtime/mali_drm.cpp`
   and implement the JM subset: `VERSION_CHECK_JM` (report 11.x), `GET_GPUPROPS`,
   `GET_CPU_GPU_TIMEINFO`, `MEM_ALLOC_EX/ALLOC/FREE/QUERY/IMPORT/SYNC/ALIAS`,
   `MEM_JIT_INIT/COMMIT/EXEC_INIT`, `SET_FLAGS`, `KCPU_QUEUE_*`, `JOB_SUBMIT`.
   Most map onto the existing `IOCTL_MALI_*` v2 ABI; atoms map onto our job
   chains. This is the real remaining port.
6. **Build `vulkan_panfrost.dll` for Windows ARM64** with
   `build-mesa.ps1 -Kind vulkan` (already switched to
   `-Dpanfrost-pregen=true -Dpanfrost-kmds=kbase,panthor`). The interesting
   question is what the MSVC/ARM64 build says about the stack's 14k lines —
   that is the first true compile of the stack for Windows.
7. **Verify on the tablet**, in order: `vkCreateDevice`, `vkCmdFillBuffer`, then
   a first triangle. `mmap`/import either works or the kbase layer is wrong;
   there is no partial success worth chasing.
8. Only then touch the KMD (multi-AS, non-contiguous BOs, VM_BIND, fences) — the
   same work WDDM needs, so the two tracks converge.

Do not start step 8 before step 7; the KMD shape follows what the shim and the
first triangle actually require.

## 6. References

- `Noysz/panvk-g99-jm` — FourFectVK patch stack, README + `patches-upstream/README.md`
  + `docs/test9/build-order.md` + `docs/roadmap-phase8-13.md`.
- Mesa MR 30736 "panvk: Valhall preparation" (Mesa 24.3), MR 29639.
- Mesa 24.3 commit `2fcf9e4` "panvk: Don't pretend we can compile for v9."
- Mesa 25.0 PanVK status (Collabora, "Mesa 25.0: PanVK moves towards production
  quality") — Vulkan 1.1 on v10.
- Phoronix: "Mesa 27.0 Switching To Kraid Compiler By Default For Arm Mali GPUs".