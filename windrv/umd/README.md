# `windrv/umd` — user-mode driver stack

The kernel driver (`windrv/kmd`) is the hardware broker. This directory is
everything above it: the **winsys** that talks to the driver, and a **Vulkan ICD**
built on top of it.

```
  VkAllocateMemory / VkBuffer / vkCmdFillBuffer / vkQueueSubmit
                         │
        windrv/umd/vulkan   (Vulkan ICD, Jalur A)
                         │
        windrv/umd          (winsys: mali_umd.h / mali_umd.c)
                         │  DeviceIoControl
                         ▼
        windrv/kmd          (MaliKmd.sys, IOCTL ABI v2)
```

The three-API plan (DirectX 12 via vkd3d-proton/DXVK, OpenGL via Zink, Vulkan
via this ICD / Mesas PanVK) is written out in
[`../GRAPHICS-STACK.md`](../GRAPHICS-STACK.md).

## Layout

```
umd/
  include/mali_umd.h        winsys API (one source of truth: includes the KMD ABI)
  include/mali_jc.h         job-chain builder API (JM descriptor encoding)
  src/mali_umd.c            winsys implementation (CRT-free, kernel32 + setupapi)
  src/mali_jc.c             job-chain builder (pure C: builds for host and ARM64)
  src/mali_umd_test.c       round-trip test: open -> BO -> job -> close
  tools/build-umd.ps1       builds build\mali_umd_test.exe
  tools/jc_test.c           host unit test for the builder byte layout
  tools/build-jc-test.ps1   builds + runs that test on this (x64) machine
  vulkan/
    include/mali_vk.h       internal ICD types
    src/icd_entry.c         Khronos loader interface + entry-point resolution
    src/icd_device.c        instance, physical device, device, memory, buffers
    src/icd_queue.c         command buffers, fill/copy/update, fences, semaphores,
                            events, vkQueueSubmit
    src/icd_wsi.c           VK_KHR_win32_surface + VK_KHR_swapchain (GDI present)
    src/mali_vk_icd.def     exported loader interface
    manifest/mali_g57_icd.json
    test/mali_vk_smoke.c    V1–V5 end-to-end smoke test (links the ICD, ARM64)
    test/mali_vk_iface.c    host x64 entry-point + Win32 WSI self-test
    tools/fetch-deps.ps1    clones external\Vulkan-Headers (git-ignored)
    tools/build-icd.ps1     builds build\mali_vk_icd.dll (+ ARM64 smoke test)
    tools/build-icd-host.ps1 builds an x64 DLL + test and runs it here
```

Everything builds to `windrv/umd/build/` (git-ignored).

Run every off-hardware check (the `mali_jc` unit test plus the ICD self-test)
from the repository root with `.\tools\run-host-tests.ps1`.

## Build

```powershell
cd windrv\umd
.\tools\build-umd.ps1            # winsys + test, no external dependencies
.\tools\build-jc-test.ps1        # host (x64) unit test for the chain builder

cd vulkan
.\tools\fetch-deps.ps1           # needs git + network, once
.\tools\build-icd.ps1            # ARM64 ICD + smoke test
.\tools\build-icd-host.ps1       # x64 ICD + self-test, builds AND runs here
```

`build-icd.ps1` also emits `build\mali_vk_smoke.exe`, a CRT-free ARM64 test that
links the ICD's import library and resolves every entry point through
`vkGetInstanceProcAddr`, so no Vulkan loader is needed to run it. On the target
it walks V1–V5 from `../GRAPHICS-STACK.md` (V1–V3 verified on silicon; the V4
and V5 stages are built but have not been run on the tablet yet) and prints:

```
V1 instance created
V1 physical device: Mali-G57 MC2 (MT6789) - Jalur A  api 1.0  vendor 0x13B5  device 0x9003
V1 device + queue created
V2 buffer object mapped, CPU r/w OK
V3 GPU vkCmdFillBuffer(0) cleared 1 KiB via a WRITE_VALUE chain
V3 non-zero fill OK (host fallback)
V4 vkCmdUpdateBuffer payload landed in the BO
V4 image (64x64 R8G8B8A8) buffer->image->buffer round-trip OK
V4 semaphore signal/wait accepted by the queue
V4 event set/reset/query OK
V4 shader module + descriptor + compute pipeline objects OK
V4 sampler + render pass + framebuffer OK
V5 swapchain created (images are KMD BOs)
V5 acquire + present via GDI blit to the window OK
RESULT: PASS
```

`build-icd-host.ps1` is the half that runs **without the tablet**: it builds the
same sources as an x64 DLL, then checks that every application entry point
resolves through `vkGetInstanceProcAddr`, that the loader interface negotiates
version 2, that the chained `*Properties2` / `*Features2` / `*MemoryProperties2`
queries fill their structures, and that the Win32 WSI surface/query set behaves
(against a real hidden window). It exits non-zero on any miss, so it is a cheap
regression gate for the whole dispatch table:

```
loader/ICD interface
  interface version   2
instance queries
  apiVersion          1.0
  instance ext        VK_KHR_get_physical_device_properties2
                      VK_KHR_surface
                      VK_KHR_win32_surface
physical device queries (chained, no KMD needed)
  properties2         Mali-G57 MC2 (MT6789) - Jalur A
  memory types        2 (1 heap, 256 MB)
  R8G8B8A8_UNORM      transfer + sampled + color
entry points
  resolved 142 / 142
Win32 WSI (surface + queries, no KMD needed)
  caps                304x201
  formats             B8G8R8A8_UNORM, B8G8R8A8_SRGB
  present mode        FIFO
  surface lifecycle   created and destroyed
  adapters without KMD 0 (VK_SUCCESS)
RESULT: PASS
```

(A loader-based run — `vulkaninfo` through the real `vulkan-1.dll` — is a later
step that also needs an ARM64 build of the Khronos loader.)

Toolchain discovery is shared with the KMD (`kmd\tools\winsdk.ps1`); nothing is
hard-coded. Both images are ARM64 and CRT-free (clang-cl `/Zl`,
`/nodefaultlib`), so no MSVC runtime is required on the target.

## Winsys (`mali_umd.h`)

A thin, typed wrapper over the IOCTL ABI v2. It does **not** build job chains —
the caller (the ICD) owns the GPU-visible byte layout. Calls:

| Group | Calls |
| --- | --- |
| lifecycle | `MaliUmdOpen`, `MaliUmdOpenPath`, `MaliUmdEnumerate`, `MaliUmdClose`, `MaliUmdIoctl` |
| identify | `MaliUmdGetVersion`, `MaliUmdGetCaps`, `MaliUmdGetIrqStats` |
| power | `MaliUmdPower` / `MaliUmdPowerOn` / `MaliUmdPowerOff` |
| MMIO | `MaliUmdRegRead`, `MaliUmdRegWrite` (bring-up only) |
| memory | `MaliUmdBoAlloc`, `MaliUmdBoInfo`, `MaliUmdBoFree` |
| MMU | `MaliUmdMmuEnable`, `MaliUmdMmuDisable`, `MaliUmdMmuFault` |
| jobs | `MaliUmdJobSubmit`, `MaliUmdJobStatus`, `MaliUmdJobStop`, `MaliUmdJobSelfTest` |

`MaliUmdBoAlloc` returns a `MALI_UMD_BO` with the GPU VA and a CPU mapping valid
in the calling process, so GPU-visible memory is both CPU-writable and directly
addressable by the GPU. A single contiguous BO is capped at 256 MB by the KMD.

`MaliUmdEnumerate` lists present device interface paths; `MaliUmdOpen` picks the
first. The test reports the version, caps, and a BO + null-job round trip.

## Job-chain builder (`mali_jc.h`)

The JM descriptor encoding — 32-byte header, payload at +32, **128-byte stride**,
1-based `job_index`, `next_job` linkage — lives in one place: `MaliJcInit`,
`MaliJcNull`, `MaliJcCacheFlush`, `MaliJcWriteValue` / `MaliJcWriteValueZero`,
`MaliJcHead`. It is pure C (no OS or KMD header), so it builds for ARM64 **and**
for the host.

That split is the point: `tools/jc_test.c` is a **host unit test** that checks the
byte layout — header words, the `next_job` chain, the 64-bit address split, and
capacity limits — right here on the development machine, no GPU needed. Run it
with `tools\build-jc-test.ps1`. One tested encoder is shared by the ICD and,
later, by the PanVK backend.

## Vulkan ICD (Jalur A)

A small ICD we own, whose purpose is to prove the whole path
`Vulkan call → winsys → IOCTL → Mali job` and to be a target small enough to
debug. It is **not** a shader-capable driver — that is Jalur B (a Mesa PanVK
port), which needs Mesa's NIR/Valhall compiler built for `aarch64-windows-msvc`.

### Works today

* Khronos loader interface: `vk_icdNegotiateLoaderICDInterfaceVersion` (v2),
  `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`.
* Instance, physical device (reported only when the KMD is present), one queue
  family (graphics + compute + transfer), device creation
  (opens the KMD, powers the GPU, enables the MMU, allocates a chain BO).
* Vulkan 1.1 *discovery* surface: `vkGetPhysicalDeviceProperties2` (with
  `VkPhysicalDeviceIDProperties` — stable UUID + LUID — and
  `VkPhysicalDeviceDriverProperties`), `*Features2`, `*MemoryProperties2`,
  `*QueueFamilyProperties2`, `*FormatProperties2`, `*ImageFormatProperties2`,
  the `*MemoryRequirements2` / `*BindMemory2` pair, and the
  `VK_KHR_get_physical_device_properties2` extension.
* `vkAllocateMemory` / `vkFreeMemory` / `vkMapMemory` → KMD buffer objects.
* Buffers: `vkCreateBuffer`, `vkGetBufferMemoryRequirements`, `vkBindBufferMemory`,
  `vkGetBufferMemoryRequirements2`, `vkBindBufferMemory2`, `vkDestroyBuffer`.
* Images: `vkCreateImage` (linear size model), `vkGetImageMemoryRequirements[2]`,
  `vkBindImageMemory[2]`, `vkCreateImageView`, `vkGetImageSubresourceLayout`,
  `vkCreateBufferView`. Transfers run on the host (`vkCmdCopyBufferToImage`,
  `vkCmdCopyImageToBuffer`), which is legal while every BO is CPU-mapped.
* Command buffers: pool/buffer create/reset/free, begin/end,
  `vkCmdUpdateBuffer`, `vkCmdCopyBuffer`, `vkCmdFillBuffer`.
* Synchronisation objects: fences, semaphores (submit wait/signal),
  events (`vkSetEvent` / `vkResetEvent` / `vkCmdSetEvent` / `vkCmdResetEvent`).
* Shader/pipeline/descriptor/render-pass/framebuffer/sampler objects are
  created, tracked and destroyed. They are bookkeeping only until the
  SPIR-V → Valhall compiler exists, so they accept work but do not execute it.
* `vkCmdFillBuffer`: a **zero fill** is issued to the GPU as a `WRITE_VALUE`
  (`MALI_WRITE_VALUE_ZERO = 3`) job chain through the KMD; the completion is
  checked via `JS_STATUS`. Non-zero fills, updates and copies fall back to a
  host-side implementation, which is legal because every BO is host-visible and
  host-coherent.
* `vkCmdPipelineBarrier` (no-op), fences, `vkQueueSubmit`.
* Win32 WSI: `VK_KHR_surface` + `VK_KHR_win32_surface` +
  `VK_KHR_swapchain`. `vkCreateWin32SurfaceKHR` binds a window; the surface
  queries report the client-area extent, `B8G8R8A8_UNORM`/`_SRGB` and FIFO.
  Swapchain images are KMD buffer objects, and `vkQueuePresentKHR` copies the
  image to the window with a GDI `StretchDIBits` blit — the L2 present path in
  `windrv/DISPLAY-ANDROID.md`, which needs no WDDM and works over the Microsoft
  Basic Display Adapter. There is no vblank wait (submission is synchronous),
  so `vkAcquireNextImageKHR` signals its semaphore/fence immediately.
  Caveat: each swapchain image is a single *contiguous* KMD BO, so a full
  1200p surface asks for ~9 MB of contiguous non-cached memory per image. That
  can fail on a fragmented system; non-contiguous images wait for the
  page-table path to be validated on silicon.

Submission is **synchronous**: `vkQueueSubmit` runs the work and signals the
fence before returning. That is legal and makes fences exact; asynchronous
submission comes with the real job scheduler.

### Not there yet (and why)

| Missing | Blocked on |
| --- | --- |
| shader execution (draws, dispatches) | the SPIR-V → Valhall compiler (Mesa) — Jalur B |
| real image layouts / tiling / sampling | Valhall tiling + the compiler |
| real vblank throttling, mailbox/relaxed present | a display path with vblank (WDDM miniport) |
| async queues, timeline semaphores | kernel job scheduler + real IRQ |

Until pipelines exist, D3D11/D3D12 (DXVK/vkd3d-proton) and OpenGL (Zink) cannot
use this ICD for real workloads — they are downstream of the same compiler.
The swapchain/present path, by contrast, is complete and testable now.

### Job encoding used for the GPU zero fill

From `windrv/kmd/include/mali_job.h`. Descriptors are **128-byte aligned** (a
measured hardware rule: a 64/96/192-byte stride fails with `JS_STATUS 0x58`).

```
word  0..3 : exception status / fault pointer (0)
word  4    : [0]=descriptor size(1) | [7:1]=job_type(2=WRITE_VALUE)
             | [31:16]=job_index (1-based)
word  5    : dependencies (0)
word  6..7 : next_job GPU VA (0 ends the chain)
word  8..9 : payload address (the 8-byte target)
word 10    : value_descriptor = 3 (ZERO)
word 11    : reserved
word 12..13: immediate (0)
```

One `WRITE_VALUE ZERO` job zeroes 8 bytes, so a fill becomes `size/8` linked
jobs. Fills larger than `MALI_VK_MAX_ZERO_JOBS` (256 jobs, 2 KiB) use the host
fallback rather than overflow the chain BO.

## Registering the ICD

The manifest `manifest/mali_g57_icd.json` points at `mali_vk_icd.dll` (copied next
to it by the build as `build/mali_g57_icd.json`). To let the Khronos loader find
it:

* copy the manifest into `%ProgramData%\Vulkan\icd.d\`, or
* for a per-session test, set `VK_DRIVER_FILES` (or the legacy
  `VK_ICD_FILENAMES`) to the manifest path.

## Relationship to the KMD ABI

Nothing here re-declares the IOCTL ABI: `mali_umd.h` includes
`kmd/include/mali_kmd_ioctl.h`, so a change to the ABI is a single-file change.
The winsys is the only component that calls `DeviceIoControl`; the ICD only ever
talks to the winsys.
