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

- **Option A (recommended): adopt Mesa's kbase backend.**
  Build with `-Dpanfrost-kmds=kbase` and make our KMD answer the **kbase ioctl
  ABI** that backend speaks (`DRM_IOCTL_MALI_*`: `MEM_ALLOC`, `JOB_SUBMIT`,
  `VM_BIND`, `SET_FLAGS`, `GET_VERSION`, eventfd/fence handling). This is the
  path FourFectVK validates, it is maintained upstream-bound, and it retires our
  hand-written libdrm shim in favour of a smaller, upstream-supported surface.
  Cost: our IOCTL ABI (v2) must gain a kbase-compatible front end, and the KMD
  must implement the semantics the backend assumes (VM zones incl. an exec VA
  zone — patch `0004` grew it from 4 MiB to 256 MiB because DXVK 2.x with GPL
  ran out of it).
- **Option B: keep our libdrm/POSIX shim** and keep the custom IOCTL winsys.
  Less KMD work, but the shim has to survive the 25.0.7 → 26.3 rebase and we
  carry a private winsys forever. Not recommended.

Note the two are not exclusive: Option A's kbase ioctls can live beside the
existing v2 IOCTLs, and Jalur A (our own ICD) keeps working either way.

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
- **Toolchain.** `mesa_clc` / `panfrost_compile` / `vtn_bindgen2` host tools are
  needed; the Kraid Rust compiler becoming default in 27.0 argues for pinning at
  26.3.0-devel.
- **kbase version.** FourFectVK validates against kbase **r54p1**; our vendor
  tree is **r32p1**. Mesa's kbase backend talks the *ioctl ABI*, which is stable
  enough, but flag this if ioctl semantics differ.
- **Licensing.** Mesa is MIT; the kbase backend is the vendor kbase ABI. No new
  licence obligation is introduced beyond what the repo already ships.

## 5. Suggested next steps (in order)

1. Clone Mesa at `feeadb5f` into a scratch tree (**outside** this repo; the repo
   only keeps patches) and confirm `src/panfrost/vulkan/valhall/` is populated
   and that v9 JM is present.
2. Dry-run `patches-upstream/0001…0005` to confirm they apply cleanly to
   `feeadb5f` (they should — that is the published base).
3. Port `windrv/mesa/patches/*` onto that tree; write down each conflict.
4. Decide Option A vs B (§3.1) once step 3 shows how much of the libdrm shim
   survives.
5. Only then touch the KMD (kbase ioctl front end and/or multi-AS + VM_BIND).

Do not start step 5 before step 4; the KMD shape depends on the winsys contract.

## 6. References

- `Noysz/panvk-g99-jm` — FourFectVK patch stack, README + `patches-upstream/README.md`
  + `docs/test9/build-order.md` + `docs/roadmap-phase8-13.md`.
- Mesa MR 30736 "panvk: Valhall preparation" (Mesa 24.3), MR 29639.
- Mesa 24.3 commit `2fcf9e4` "panvk: Don't pretend we can compile for v9."
- Mesa 25.0 PanVK status (Collabora, "Mesa 25.0: PanVK moves towards production
  quality") — Vulkan 1.1 on v10.
- Phoronix: "Mesa 27.0 Switching To Kraid Compiler By Default For Arm Mali GPUs".