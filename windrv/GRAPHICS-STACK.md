# Graphics stack — Vulkan, OpenGL, DirectX 12

> Dokumen ini melengkapi [`WDDM-PATH.md`](WDDM-PATH.md). WDDM menjawab
> "bagaimana Windows **mengenali** Mali sebagai GPU"; dokumen ini menjawab
> "bagaimana aplikasi **memakai** Mali lewat Vulkan, OpenGL, dan DirectX 12".
>
> Aturan yang tidak berubah: KMD (`windrv/kmd`) adalah **broker hardware** untuk
> job, MMU, power, dan memori. Tidak ada scanout di sini. Semua graphapi di
> dokumen ini duduk di atas satu fondasi yang sama: **libmali_umd** (pembungkus
> IOCTL ABI v2).

---

## 0. Satu fondasi, tiga API

```
  aplikasi D3D12        aplikasi D3D11/10/9      aplikasi OpenGL        aplikasi Vulkan
        │                      │                       │                     │
        ▼                      ▼                       ▼                     │
  ┌───────────┐          ┌───────────┐          ┌─────────────┐             │
  │vkd3d-proton│          │   DXVK    │          │ Zink (GL→Vulkan)│          │
  │ (reuse)    │          │ (reuse)   │          │ (reuse)         │          │
  └─────┬─────┘          └─────┬─────┘          └──────┬──────────┘         │
        └──────────┬───────────┘                       │                    │
                   ▼                                   ▼                    ▼
            ┌──────────────────────────────────────────────────────────────────┐
            │            Vulkan ICD Mali-G57  (windrv/umd/vulkan)               │
            │   — implementasi KamiSendiri  ATAU  port Mesa PanVK               │
            └───────────────────────────────┬──────────────────────────────────┘
                                            ▼
            ┌──────────────────────────────────────────────────────────────────┐
            │            libmali_umd  (windrv/umd)  — winsys                    │
            │   open/close, caps, BO alloc/map/free, MMU, job submit/wait       │
            └───────────────────────────────┬──────────────────────────────────┘
                                            ▼  IOCTL (MaliKmd ABI v2)
            ┌──────────────────────────────────────────────────────────────────┐
            │            MaliKmd.sys (KMDF)  — hardware broker                  │
            └───────────────────────────────┬──────────────────────────────────┘
                                            ▼
                              Mali-G57 MC2  (Valhall, tNAx r0p1)
```

Tiga API, tapi **hanya satu komponen yang benar-benar menentukan**: Vulkan ICD.
Begitu ICD bekerja, OpenGL (lewat Zink) dan DirectX (lewat DXVK/vkd3d-proton)
ikut jalan tanpa menulis driver baru. Inilah alasan urutan kerjanya Vulkan
lebih dulu.

---

## 1. Yang reuse vs yang tulis (jujur di depan)

| Komponen | Sumber | Kita tulis? |
| --- | --- | --- |
| Vulkan **loader** (app → ICD) | Khronos `vulkan-1.dll` | tidak (reuse) |
| Vulkan **ICD** Mali-G57 | **kita** | **ya — inti proyek** |
| winsys (ICD → KMD) | **kita** (`libmali_umd`) | **ya** |
| Shader compiler (SPIR-V → Valhall ISA) | Mesa (`nir`, `bi_*`, `panfrost`) | tidak (port) |
| D3D12 → Vulkan | `vkd3d-proton` | tidak (reuse) |
| D3D9/10/11 → Vulkan | `DXVK` | tidak (reuse) |
| OpenGL → Vulkan | `Zink` (dalam Mesa) | tidak (reuse) |
| KMD (job/MMU/power/BO) | **kita** | **ya — sudah jalan** |

Yang **tidak ada di dunia** dan karena itu harus ditulis: **ICD Vulkan untuk
Mali di Windows.** Arm tidak punya, MediaTek tidak punya, komunitas tidak punya
(port Windows-on-ARM yang ada hanya Adreno). Ini tembok yang kita panjat.

---

## 2. Vulkan — ICD Mali-G57 di Windows

### 2.1 Bentuk ICD yang benar

ICD adalah DLL yang mengekspor kontrak loader Khronos (`vk_icd.h`):

```
vk_icdNegotiateLoaderICDInterfaceVersion()
vk_icdGetInstanceProcAddr()
vk_icdGetPhysicalDeviceProcAddr()
```

Loader (`vulkan-1.dll`) memanggil `vkCreateInstance` → `vkEnumeratePhysicalDevices`
→ `vkCreateDevice` lewat pointer yang kita kembalikan, bukan lewat nama simbol.
Manifest JSON (`mali_g57_icd.json`) memberi tahu loader DLL/registry mana yang
harus dimuat.

Yang **harus** ada supaya "Vulkan jalan" berarti sesuatu:

- instance + physical device + queue family (dari caps KMD, bukan hard-code);
- `vkAllocateMemory`/`vkFreeMemory`/`vkMapMemory` → BO KMD;
- buffer + image (image nanti; butuh tiling Valhall);
- command buffer + queue submit → job chain KMD (128-byte alignment!);
- fence/timeline semaphore → `JOB_IRQ`/polling KMD;
- **pipeline + shader** (lihat §2.4 — ini bagian yang tidak bisa diakali).

### 2.2 Strategi dua jalur

**Jalur A — ICD minimal milik sendiri (`windrv/umd/vulkan`).**
Tujuannya bukan menggantikan PanVK, tapi:

1. membuktikan jalur `Vulkan call → libmali_umd → IOCTL → job` end to end;
2. menjadi target uji yang kecil dan bisa di-debug;
3. menjawab "apakah ABI KMD cukup untuk sebuah ICD?" (ABI v2 yang ada sekarang
   sudah cukup untuk memory + submit + fence).

Slice pertama yang masuk akal dan **bisa diuji tanpa shader**:
`vkCmdFillBuffer` / `vkCmdUpdateBuffer` → rangkaian job WRITE_VALUE → fence.
Ini memvalidasi BO, mapping, submit, completion — tanpa butuh compiler ISA.

Status terukur Jalur A (per `windrv/umd/README.md`):

| Bukti | Cara uji | Hasil |
| --- | --- | --- |
| 130 entry point aplikasi resolve | `tools\build-icd-host.ps1` (x64, tanpa tablet) | **PASS 130/130** |
| loader/ICD interface v2 | idem | **PASS** |
| `Properties2` + UUID + LUID + driver props | idem | **PASS** |
| `*Features2` / `*MemoryProperties2` / `*FormatProperties2` | idem | **PASS** |
| `vkEnumeratePhysicalDevices` tanpa KMD = 0 adapter, `VK_SUCCESS` | idem | **PASS** |
| fill nol di GPU (WRITE_VALUE chain) | `mali_vk_smoke.exe` di tablet | **PASS (V3)** |
| image/BO round-trip, update, semaphore, event, pipeline stub | `mali_vk_smoke.exe` di tablet | **dibangun; menunggu tablet** |

Jadi Jalur A sudah **bukan** sekadar "bisa instance/device": permukaan Vulkan 1.0
+ 1.1-discovery sudah lengkap untuk object model, dan satu-satunya yang tersisa
sebelum render nyata adalah **eksekusi shader** (Jalur B).

**Jalur B — port Mesa PanVK (Valhall).**
Ini yang memberi render nyata (segitiga, tekstur, compute). PanVK sudah tahu
Valhall (G57), sudah punya compiler shader, dan sudah punya driver uji. Yang
harus diganti: **backend DRM** (`/dev/dri`, ioctl panfrost/panthor, GEM, dma-buf,
syncobj) → **libmali_umd kita**.

Yang konkret perlu dikerjakan di jalur B kini terperinci di
[`PANVK-SHIM.md`](PANVK-SHIM.md): setiap kebutuhan panfrost/panvk dipetakan ke
ABI kita dengan status ✅ siap / ⚠️ perlu glue / ❌ belum ada, plus usulan ABI v3.
Ringkasnya:

| PanVK/panfrost harap | Windows/MT6789 punya | Tindakan |
| --- | --- | --- |
| `open("/dev/dri/renderD128")` | device interface GUID | shim `open` → `MaliUmdOpen()` |
| `DRM_PANFROST_SUBMIT` | `IOCTL_MALI_JOB_SUBMIT` | shim + `mali_jc` |
| `DRM_PANFROST_CREATE_BO` | `IOCTL_MALI_ALLOC_BO` | shim |
| GEM mmap / dma-buf | `UserVa` dari KMD | shim (dma-buf = P2) |
| `drmSyncobj` / fences | JOB_IRQ + poll | **disintesis di shim** (submit sinkron) |
| `PANFROST_DEV_QUERY` | KMD caps + `REG_READ` | isi struct dari register |
| Linux `pthread`/`epoll` | `CreateThread`/`WaitForSingleObject` | port tipis |

Temuan penting (lihat `PANVK-SHIM.md` §0): **PanVK memakai model yang sama dengan
kita** — chain dibangun di userspace, `submit` hanya mengirim VA kepala chain —
jadi kebutuhan fitur KMD baru untuk smoke test **kecil**; yang besar justru
**build Mesa untuk ARM64 Windows** dan **shim Win32/DRM**, dua-duanya di sisi kita.

Realitas build (terukur — lihat [`mesa/README.md`](mesa/README.md)): Meson +
clang-cl cross ke `aarch64-windows-msvc` **sudah jalan** di mesin dev (objek ARM64
`0xAA64` + arsip statik terbukti, `tools\build-smoke.ps1`). Yang masih menghambat:
**CRT MSVC ARM64 belum terpasang** (`vcruntime`/`libcmt`/`libcpmt` absen), jadi
menautkan `.exe`/`.dll` belum bisa. Terukur: yang kurang **bukan** libc/header/
linker — `msvcurt.lib` + `libucrt.lib` + `BufferOverflow.lib` + `ntdll.lib`
sudah menyelesaikan libc, dan yang tersisa persis internal `vcruntime` ARM64
(`__GSHandlerCheck`, `__security_push/pop_cookie`, `_InterlockedAnd/Or`). Jadi
ada dua jalan: pasang workload ARM64 (sekali, ~1 GB), atau tambahkan shim
`vcruntime` mini di `windrv/mesa/runtime/`. Detail di `mesa/README.md`.

### 2.3 Shader — kompilasi vs cache

Dua mode yang harus dibedakan dan tidak boleh dicampur:

- **Offline / AOT**: SPIR-V dikompilasi ke ISA Valhall di mesin build → ICD
  memuat blob. Cocok untuk membuktikan pipeline tanpa runtime compiler.
- **Runtime**: SPIR-V → ISA saat aplikasi jalan. Ini yang dipakai driver normal
  dan **wajib untuk aplikasi nyata** (shader game tidak bisa di-AOT).

Keduanya memakai compiler Mesa. Jadi ICD **apapun** yang bisa render butuh Mesa
(NIR + `bi_*`/Valhall backend) di-build untuk Windows ARM64. Tidak ada jalan
memutar: menulis compiler ISA Valhall dari nol bukan pilihan yang realistis.

### 2.4 Batas jalur A tanpa Mesa

Jalur A (ICD minimal kita) bisa: memory, transfer, fill, copy, fence, query,
barrier, dan job non-shader. Ia **tidak bisa** menjalankan vertex/fragment/compute
sampai compiler ada. Itu bukan kelemahan desain — itu batas yang jujur dan
dicatat, supaya tidak ada klaim "render jalan" sebelum benar-benar jalan.

Konsekuensi untuk D3D: aplikasi D3D12/DXVK butuh shader → jalur A saja tidak
cukup untuk mempercepat game. Di situ jalur B wajib.

---

## 3. OpenGL — lewat Zink dulu

### 3.1 Kenapa Zink

Ada dua jalur OpenGL:

| Jalur | Cara kerja | Biaya |
| --- | --- | --- |
| **Zink** (rekomendasi) | OpenGL → Vulkan ICD kita | **gratis** begitu Vulkan lengkap (shader/pipeline) |
| Gallium `panfrost` GL | OpenGL → panfrost langsung | jalur lain, lebih banyak kode khusus |

Zink adalah Gallium driver di dalam Mesa yang menerjemahkan GL ke Vulkan. Karena
kita sudah harus punya Vulkan yang benar untuk DXVK/vkd3d, **Zink membuat OpenGL
datang hampir otomatis**. Ini alasan urutan: Vulkan dulu, OpenGL menyusul.

### 3.2 Yang perlu disiapkan

- `opengl32.dll` (Mesa `llvmpipe` tidak dipakai; kita mau Zink) → Zink memakai
  Vulkan ICD kita. Mesa Windows sudah memuat `opengl32.dll` + `libgallium_wgl.dll`;
  kita arahkan Zink ke ICD kita.
- WSI: Zink butuh `VK_KHR_win32_surface` untuk window OpenGL. Sampai WSI ada,
  GL tetap bisa untuk offscreen/headless (rendering ke FBO), window present
  menyusul — sama seperti Vulkan.

### 3.3 Fallback GL tanpa shader

Tanpa compiler ISA, Zink tidak bisa apa-apa (butuh pipeline Vulkan). Jadi urutan
OpenGL memang **di belakang** shader Vulkan. Jangan janjikan lebih.

---

## 4. DirectX 12 — jembatan, bukan tulis ulang

### 4.1 Jalur yang dipakai

```
D3D12 app ──► vkd3d-proton   (d3d12.dll + d3d12core.dll)  ─┐
D3D11/10/9 app ──► DXVK      (d3d11.dll/dxgi.dll)          ├─► ICD kita ──► Mali
                                                           ┘
```

- `vkd3d-proton` dan `DXVK` adalah **DLL Win32 native** — yang sama dipakai
  Proton untuk menjalankan game Windows di Linux. Kita **tidak menulis** sisi
  DirectX. Itu bagian yang mahal dan sudah selesai di tempat lain.
- Yang dibutuhkan cuma dua: (1) ICD Vulkan kita lengkap, (2) DLL ditaruh sebagai
  override per-aplikasi (tidak menyentuh desktop/DWM yang tetap di BDA).
- vkd3d-proton ekspektasi fitur Vulkan yang lumayan tinggi (descriptor indexing,
  timeline semaphore, dll). Ini artinya ICD harus matang, bukan sekedar "bisa
  segitiga". Dicatat sebagai syarat, bukan kejutan.

### 4.2 Kenapa D3D12 native bukan jalan pertama

`d3d12.dll` butuh UMD `D3D12DDI` di bawah **display driver WDDM**. Ratusan
fungsi (PSO, root signature, descriptor heap, barrier, query). Tidak ada
implementasi independen di dunia. Karena itu D3D12 asli (`WDDM-PATH.md` lapis 3)
ditempatkan paling akhir dan opsional; sebelum itu, translasi lewat Vulkan tetap
cara yang sah dan terbukti.

### 4.3 "DX12 jalan hari ini" (stopgap)

WARP (software D3D12) ada di dalam Windows. Dengan display berjalan, aplikasi
D3D12 langsung **benar** (lambat). Ini memberi "DX12 jalan" sekarang, sementara
jalur Mali dipercepat di belakangnya — tanpa saling menjatuhkan.

---

## 5. Urutan kerja & definisi selesai

Satu kolom satu tahap, satu baris satu bukti.

| Tahap | Isi | Definisi selesai (bisa diuji) |
| --- | --- | --- |
| U0 | `libmali_umd` (winsys) | test umd: open→caps→BO→submit→wait hijau di tablet |
| U0b | `mali_jc` job-chain builder | unit test byte-layout hijau **di mesin dev** (tanpa GPU) |
| V1 | ICD: instance/device/queue | `vulkaninfo` melihat "Mali-G57 MC2" + 1 queue family |
| V2 | ICD: memory/buffer | `vkAllocateMemory` menghasilkan BO, map, tulis dari CPU, terbaca GPU |
| V3 | ICD: submit + fence | `vkCmdFillBuffer` (job WRITE_VALUE) + `vkWaitForFences` hijau |
| V4a | ICD: object model tanpa shader | update/image/semaphore/event/pipeline-stub di `mali_vk_smoke.exe` hijau |
| V4b | ICD: pipeline + shader (butuh Mesa) | compute shader trivial menulis buffer; lalu segitiga offscreen |
| V5 | WSI win32 surface | swapchain present ke window (butuh WDDM/display miniport atau blit) |
| X1 | DXVK di atas ICD | aplikasi D3D11 sederhana render di Mali |
| X2 | vkd3d-proton di atas ICD | aplikasi D3D12 sederhana render di Mali |
| G1 | Zink di atas ICD | aplikasi OpenGL sederhana render di Mali |
| W3 | WDDM render-only | Mali muncul di Device Manager/Task Manager |

Urutan yang disarankan: **U0 → V1 → V2 → V3 → V4a → (V4b = titik berat) → X1 → X2 → G1 → V5/W3`.

---

## 6. Struktur repo untuk stack ini

```
windrv/
  GRAPHICS-STACK.md        dokumen ini
  WDDM-PATH.md             jalur "Windows mengenali Mali"
  PANVK-SHIM.md            jembatan Jalur B: panfrost/DRM → ABI kita + ABI v3
  umd/                     user-mode driver (kita)
    include/mali_umd.h     API winsys (publik, dipakai ICD)
    include/mali_jc.h      builder job-chain (encoding descriptor JM)
    src/mali_umd.c         implementasi di atas IOCTL ABI v2
    src/mali_jc.c          builder, murni C (host + ARM64)
    src/mali_umd_test.c    uji round-trip (CRT-free, seperti malitest)
    tools/build-umd.ps1    build test untuk ARM64
    tools/jc_test.c        unit test byte-layout builder (jalan di host)
    tools/build-jc-test.ps1 build + jalankan unit test itu di mesin ini
    vulkan/                ICD Vulkan (Jalur A)
      include/             header internal
      src/                 entry point loader + implementasi inti
      manifest/            mali_g57_icd.json (untuk loader Khronos)
      test/                smoke ARM64 (tablet) + iface test x64 (host)
      tools/build-icd.ps1      build ICD ARM64 (butuh Vulkan-Headers)
      tools/build-icd-host.ps1 build ICD x64 + self-test entry point, jalankan
  mesa/                    lingkungan build Jalur B (Meson cross-file + temuan)
    README.md              catatan kesiapan (terukur: apa yang jalan / absen)
    cross/                 template cross-file aarch64-windows-msvc (tanpa path hardcoded)
    smoke/                 target uji minimal (compile objek ARM64 + arsip)
    tools/                 setup.ps1, build-smoke.ps1, fetch-mesa.ps1
  kmd/                     kernel-mode driver (sudah ada)
```

Dependensi eksternal ditaruh di `external/` (git-ignored), di-fetch lewat
script — sama polanya dengan `external/wdk`:
`external/Vulkan-Headers` (loader/ICD interface), nanti `external/mesa` (PanVK).

---

## 7. Membangun

```powershell
# winsys + test (tanpa dependensi eksternal)
cd windrv\umd
.\tools\build-umd.ps1
# -> build\mali_umd_test.exe

# ICD Vulkan (butuh Vulkan-Headers)
.\tools\fetch-deps.ps1            # clone external\Vulkan-Headers (pinned)
.\tools\build-icd.ps1
# -> build\mali_vk_icd.dll
```

Semua path ditemukan otomatis (lihat `kmd\tools\winsdk.ps1`); tidak ada path
yang di-hard-code, jadi repo bisa di-build di mesin lain.

---

## 8. Risiko (dicatat supaya tidak jadi kejutan)

- **Compiler shader di Windows ARM64.** BLOCKER terbesar jalur render. Mesa
  belum mendukung build `aarch64-windows-msvc`; ini pekerjaan build-system +
  port yang panjang.
- **PanVK berasumsi Linux DRM.** Setiap asumsi (`/dev/dri`, GEM, syncobj,
  dma-buf) harus dipetakan ke KMD kita atau diganti. Tidak ada yang otomatis.
- **vkd3d-proton butuh fitur Vulkan modern.** ICD harus matang sebelum D3D12
  memberi hasil; jangan urutkan vkd3d terlalu dini.
- **WSI/present.** Window present butuh jalur display (WDDM miniport atau blit
  ke BDA). Offscreen/headless selalu lebih dulu bisa.
- **Hardware retail tanpa dokumentasi internal Arm.** Setiap klaim harus
  diverifikasi di silicon, bukan diasumsikan.

---

## 9. Prinsip

Tidak ada bagian di sini yang "mustahil" — yang ada **urutan** dan **biaya**.
Setiap tahap punya bukti yang bisa dilihat di tablet. ICD minimal kita (Jalur A)
bukan pengganti PanVK; ia fondasi dan alat ukur. PanVK (Jalur B) yang memberi
render nyata. DirectX dan OpenGL datang sebagai konsekuensi, bukan proyek
terpisah. Yang kita tulis hanya dua: **KMD** (sudah) dan **ICD + winsys** (mulai).
