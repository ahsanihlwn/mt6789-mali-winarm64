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
  src/mali_umd.c            winsys implementation (CRT-free, kernel32 + setupapi)
  src/mali_umd_test.c       round-trip test: open -> BO -> job -> close
  tools/build-umd.ps1       builds build\mali_umd_test.exe
  vulkan/
    include/mali_vk.h       internal ICD types
    src/icd_entry.c         Khronos loader interface + entry-point resolution
    src/icd_device.c        instance, physical device, device, memory, buffers
    src/icd_queue.c         command buffers, fills, copies, fences, vkQueueSubmit
    src/mali_vk_icd.def     exported loader interface
    manifest/mali_g57_icd.json
    tools/fetch-deps.ps1    clones external\Vulkan-Headers (git-ignored)
    tools/build-icd.ps1     builds build\mali_vk_icd.dll
```

Everything builds to `windrv/umd/build/` (git-ignored).

## Build

```powershell
cd windrv\umd
.\tools\build-umd.ps1            # winsys + test, no external dependencies

cd vulkan
.\tools\fetch-deps.ps1           # needs git + network, once
.\tools\build-icd.ps1            # needs the fetched headers
```

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
* `vkAllocateMemory` / `vkFreeMemory` / `vkMapMemory` → KMD buffer objects.
* Buffers: `vkCreateBuffer`, `vkGetBufferMemoryRequirements`, `vkBindBufferMemory`,
  `vkDestroyBuffer`.
* Command buffers: pool/buffer create/reset/free, begin/end.
* `vkCmdFillBuffer`: a **zero fill** is issued to the GPU as a `WRITE_VALUE`
  (`MALI_WRITE_VALUE_ZERO = 3`) job chain through the KMD; the completion is
  checked via `JS_STATUS`. Non-zero fills and `vkCmdCopyBuffer` fall back to a
  host-side fill/copy, which is legal because every BO is host-visible and
  host-coherent.
* `vkCmdPipelineBarrier` (no-op), fences, `vkQueueSubmit`.

Submission is **synchronous**: `vkQueueSubmit` runs the work and signals the
fence before returning. That is legal and makes fences exact; asynchronous
submission comes with the real job scheduler.

### Not there yet (and why)

| Missing | Blocked on |
| --- | --- |
| shaders / pipelines / descriptors | the SPIR-V → Valhall compiler (Mesa) — Jalur B |
| images / samplers | needs Valhall tiling + the compiler |
| swapchain / WSI | a display path (WDDM render miniport or a BDA blit) |
| async queues, timeline semaphores | kernel job scheduler + real IRQ |

Until pipelines exist, D3D11/D3D12 (DXVK/vkd3d-proton) and OpenGL (Zink) cannot
use this ICD for real workloads — they are downstream of the same compiler.

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
