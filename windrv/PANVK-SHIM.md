# Jembatan PanVK — peta panfrost/DRM ke ABI kita

> Dokumen ini menjawab: **kalau kita port Mesa PanVK ke Windows (Jalur B), apa
> persisnya yang dibutuhkan PanVK, apa yang sudah bisa dipenuhi KMD kita, dan apa
> yang belum.** Tujuannya menghilangkan tebakan: setiap baris di sini dipetakan
> ke `windrv/kmd/include/mali_kmd_ioctl.h` (ABI v2) atau ditandai sebagai celah.
>
> Sumber: `GRAPHICS-STACK.md` §2.2 (Jalur B), `WDDM-PATH.md`, dan UAPI panfrost
> di Mesa (`include/uapi/drm/panfrost_drm.h`, `panfrost/*.c`, `panvk/*`).

---

## 0. Temuan utama (baca ini dulu)

Analisis ini **mengubah rencana**. Kesimpulannya:

1. **PanVK tidak butuh banyak fitur KMD baru.** PanVK/panfrost sudah memakai model
   yang sama dengan kita: *userspace membangun seluruh job chain di dalam BO, lalu
   `submit` hanya mengirim VA kepala chain*. Itu tepat `IOCTL_MALI_JOB_SUBMIT`.
2. **Kritikal path-nya bukan KMD, melainkan dua hal:** (a) **build Mesa untuk
   `aarch64-windows-msvc`**, dan (b) **shim kompatibilitas Win32/DRM** (libdrm +
   POSIX kecil: `open/ioctl/mmap/poll/eventfd`).
3. **Sebagian besar "celah" bisa ditutup di user mode**, karena submit kita
   sinkron: fence/syncobj, eventfd, poll — semua bisa disintesis di shim tanpa
   mengubah KMD sama sekali.
4. **G57 = Valhall arch 9 = front-end JM**, jadi driver DRM yang relevan adalah
   **`panfrost` (bukan `panthor`/CSF)**. Ini penting: `panthor` menargetkan CSF
   (gen ≥ 10, butuh MCU/firmware). Kita tidak butuh itu.

Artinya: pekerjaan KMD yang benar-benar wajib untuk smoke test PanVK **kecil**.
Yang besar adalah Mesa build + shim. Itu tertutup dan terukur — bukan riset
terbuka.

---

## 1. Dua strategi shim

| | Strategi | Cara | Biaya | Risiko |
| --- | --- | --- | --- | --- |
| **S1** | Patch Mesa | Ganti panggilan libdrm/POSIX di `panfrost/*.c` + `panvk/*` dengan API kita | banyak titik sentuh, tapi bersih | patch besar, susah rebase ke upstream |
| **S2** | Shim `libdrm` + POSIX (rekomendasi) | Sediakan `libdrm.dll`-compatible + `unistd`/`mmap`/`poll`/`eventfd` shim; kode panfrost nyaris tak berubah | satu lapisan tebal | harus meniru semantik Linux cukup setia |

**Rekomendasi: S2**, dengan *seam* S1 kecil hanya di tempat semantiknya beda
(submit/fence), supaya kalau perlu patch Mesa, patch-nya minimal dan terisolasi.

---

## 2. Peta kebutuhan PanVK → yang kita punya

Status: ✅ siap · ⚠️ bisa, perlu glue · ❌ belum ada

### 2.1 Penemuan & query device

| Kebutuhan panfrost/panvk | Bentuk Linux | Yang kita punya | Status |
| --- | --- | --- | --- |
| Buka device | `open("/dev/dri/renderD128")` | `MaliUmdOpen()` (device interface GUID) | ⚠️ shim `open` |
| `DRM_IOCTL_VERSION` | `drm_version` | `IOCTL_MALI_GET_VERSION` | ✅ |
| `DRM_PANFROST_GET_PARAM` (gpu_id, revision, shader_present, l2_features, mem_features, mmu_features, tiler_features, thread_features, js_present, js_features, as_present, …) | `drm_panfrost_get_param` | `IOCTL_MALI_GET_CAPS` **+** `IOCTL_MALI_REG_READ` (baca registfer persis yang Mesa mau) | ✅ (dari register) |
| `DRM_IOCTL_GET_CAP` | `drm_get_cap` (SYNCOBJ, PRIME, TIMESTAMP_MONOTONIC) | dipalsukan di shim | ⚠️ |

Catatan: `GET_PARAM` **bukan** masalah, karena `REG_READ` mengizinkan shim membaca
`GPU_ID`, `CORE_FEATURES`, `L2_FEATURES`, `MEM_FEATURES`, `MMU_FEATURES`,
`TILER_FEATURES`, `SHADER_PRESENT`, `JS_PRESENT`, `JS_FEATURES(n)` apa adanya —
persis yang `panfrost_gpu.c` baca.

### 2.2 Memori (buffer object)

| Kebutuhan | Bentuk Linux | Yang kita punya | Status |
| --- | --- | --- | --- |
| Buat BO | `DRM_IOCTL_MODE_CREATE_DUMB`/`DRM_PANFROST_CREATE_BO` (flags: is_heap, etc.) | `IOCTL_MALI_ALLOC_BO` (kontigu, max 256 MB, `MmNonCached`) | ⚠️ lihat §3 |
| Peta ke CPU | `mmap(fd, offset)` | `UserVa` dari `ALLOC_BO` | ⚠️ `mmap`→`UserVa` |
| Offset GPU BO | `DRM_IOCTL_PANFROST_GET_BO_OFFSET` | `GpuVa` (= fisik, identitas) | ⚠️ §3.1 |
| Info BO | — | `IOCTL_MALI_BO_INFO` | ✅ |
| Bebaskan | `GEM_CLOSE` | `IOCTL_MALI_FREE_BO` | ✅ |
| Tunggu BO | `DRM_IOCTL_PANFROST_WAIT_BO` | polling job/fence di shim | ⚠️ shim |
| dma-buf (PRIME `HANDLE_TO_FD`/`FD_TO_HANDLE`) | `drmPrimeHandleToFD` | ❌ | ❌ P2 (WSI/interop) |

### 2.3 Submit job

| Kebutuhan | Bentuk Linux | Yang kita punya | Status |
| --- | --- | --- | --- |
| Kirim job | `DRM_PANFROST_SUBMIT` (`drm_panfrost_job`: `jc`, `requirements[]`, `in_sync`, `out_sync`, `core_req`, `submit_flags`) | `IOCTL_MALI_JOB_SUBMIT` (`Jc`, `Affinity`, `Slot`, `TimeoutMs`) | ✅⚠️ |
| Chain = seluruh kerja | userspace bangun chain di BO | **sama** — kita kirim VA kepala chain | ✅ |
| `core_req` (mask shader core) | → `affinity` | `Affinity` | ⚠️ peta bit |
| `requirements[]` (BO yang harus tetap hidup) | dicek kernel | tidak perlu (BO tidak di-evict) | ✅ (diabaikan) |
| Fence masuk/keluar | `in_sync`/`out_sync` syncobj atau sync_file | ❌ (KMD hanya poll) | ⚠️ **disintesis di shim** (§3.2) |
| Selesai sinkron | — | `TimeoutMs > 0` → KMD menunggu & mengembalikan `JsStatus` | ✅ |

Poin penting: PanVK **membangun descriptor VERTEX/FRAGMENT/COMPUTE/TILER sendiri**.
KMD dan `mali_jc` tidak perlu mengenali tipe itu; hanya `JOB_SUBMIT(Jc)`. Ini yang
membuat jalur submit kita pada dasarnya sudah cocok.

### 2.4 Sinkronisasi & event

| Kebutuhan | Bentuk Linux | Yang kita punya | Status |
| --- | --- | --- | --- |
| `drm_syncobj` (create/query/wait/signal/transfer) | `DRM_IOCTL_SYNCOBJ_*` | ❌ di KMD | ⚠️ murni user-mode (submit sinkron) |
| `sync_file` (fd fence) | `SYNCOBJ_HANDLE_TO_FD` | ❌ | ⚠️ shim (event Win32) |
| `eventfd` | `eventfd()` | ❌ | ⚠️ shim (Event manual) |
| `poll()` | `poll()` | ❌ | ⚠️ shim (`WaitForMultipleObjects`) |
| Timeline semaphore | Vulkan → syncobj timeline | ❌ | ⚠️ P1 |

Karena `JOB_SUBMIT` dengan `TimeoutMs>0` sudah **sinkron**, semua syncobj bisa
dibuat "sudah tersinyal" saat submit kembali. Itu legal dan cukup untuk smoke test.

### 2.5 Power, reset, fault, IRQ

| Kebutuhan | Yang kita punya | Status |
| --- | --- | --- |
| Power on/off | `IOCTL_MALI_POWER`, `GET_PWR_STATUS` | ✅ |
| Soft reset | sudah ada di jalur power-on KMD | ✅ |
| Baca fault GPU/MMU | `IOCTL_MALI_MMU_FAULT`, `IOCTL_MALI_IRQ_STATS` | ✅ |
| IRQ nyata | polling (`ROOT\MaliMtk` belum punya resource) | ⚠️ P1 (butuh node ACPI) |

### 2.6 Lain-lain

| Kebutuhan | Status |
| --- | --- |
| `perfcnt` (profiling) | ❌ P2, tidak menghambat |
| `madvise`/evict | ❌ P2 (BO tidak di-evict) |
| `query cpu/gpu timestamp` | ⚠️ shim (`QueryPerformanceCounter`) |
| `get_bo_offset` → GVA | ⚠️ §3.1 |

---

## 3. Celah yang benar-benar perlu ditutup

### 3.1 Model GPU VA — selaras identitas (kritis, user-mode)

PanVK mengelola **heap GPU VA** dan memilih offset sendiri; KMD kita memakai
**identitas (VA == PA)**, jadi `GpuVa` ditentukan KMD, bukan userspace.

Yang dilakukan shim: jadikan `GET_BO_OFFSET` mengembalikan `GpuVa` KMD, dan
pastikan PanVK **tidak** mencoba me-remap offset itu. Opsi:
- paksa PanVK memakai mode identitas untuk alokasinya (patch kecil), atau
- sediakan "heap" palsu di shim yang hanya meneruskan offset asli KMD.

Ini titik paling mungkin membuat PanVK tersandung lebih dulu — dicatat sebagai
risiko #1 Jalur B.

### 3.2 Fence/syncobj (kritis, user-mode)

Sediakan di shim: objek syncobj (nilai + Event Win32), `wait` = `WaitForSingleObject`,
`signal` = `SetEvent`, `transfer` = salin nilai. Karena submit sinkron, out-fence
langsung tersinyal setelah `JOB_SUBMIT` kembali. **Tidak butuh KMD.**

### 3.3 Memori non-kontigu (P1, KMD)

`MmAllocateContiguousMemorySpecifyCache` membatasi ke **kontigu + 256 MB**. PanVK
akan minta buffer besar dan banyak. Untuk smoke test (buffer kecil) ini cukup;
untuk aplikasi nyata perlu **alokasi non-kontigu + page list + page table** (yang
memang sudah arah pengembangan MMU KMD).

### 3.4 Multi-AS (P1, KMD)

panfrost default **satu VM per fd**; AS0 kita mungkin cukup untuk satu proses.
Kalau PanVK membuat AS sendiri, perlu `VM_CREATE`/`VM_BIND`. Dicatat, belum wajib.

### 3.5 Cache maintenance eksplisit (P1, KMD/builder)

BO kita `MmNonCached`, jadi CPU↔GPU koheren otomatis. Meski begitu PanVK menyisipkan
job `CACHE_FLUSH` di chain (bukan IOCTL), yang sudah dikenali `mali_jc`
(`MaliJcCacheFlush`). IOCTL cache eksplisit (invalidate range) menjadi P1.

### 3.6 dma-buf / PRIME (P2)

Dibutuhkan untuk WSI/interop dan `VK_KHR_external_memory`. Tidak untuk headless
smoke test.

---

## 4. Usulan ABI v3 (aditif; tidak mengubah v2)

Didesain **aditif**: IOCTL lama tetap; ini menambah fungsi. Nama/struktur di bawah
adalah usulan, bukan yang sudah dikodekan.

```
IOCTL_MALI_BO_ALLOC2      -> non-contiguous: ukuran bebas, + list page/segment
IOCTL_MALI_BO_MAP         -> map BO ke VM VA dgn atribut (RW/cache) di AS tertentu
IOCTL_MALI_VM_CREATE      -> address space baru (multi-AS), kembali AS number
IOCTL_MALI_VM_DESTROY     -> hapus AS
IOCTL_MALI_VM_BIND        -> bind/unbind rentang di AS (untuk page table)
IOCTL_MALI_CACHE          -> flush/invalidate rentang (invalidate range)
IOCTL_MALI_JOB_SUBMIT2    -> atom: N chain + dependensi + out-fence eksplisit
IOCTL_MALI_FENCE_CREATE/WAIT/SIGNAL  -> fence/timeline (kalau mau keluar dari polling)
IOCTL_MALI_QUERY          -> perfcnt, timestamp, resource
```

Prioritas: `JOB_SUBMIT2` (P1, untuk pipa ganda), `VM_*` + `BO_ALLOC2` (P1),
sisanya P2. **Tidak ada yang P0** untuk smoke test pertama.

`mali_jc` (userspace) sudah menjadi tempat yang tepat untuk memperluas encoding
descriptor tanpa menyentuh KMD.

---

## 5. Asumsi Linux yang harus dipetakan di shim

| Linux | Pengganti Windows | Catatan |
| --- | --- | --- |
| `open/close/read/write` pada fd | `HANDLE` + `DeviceIoControl` | fd = indeks tabel di shim |
| `ioctl(fd, DRM_IOCTL_*)` | dispatch ke `MaliUmd*` | termasuk passthrough |
| `mmap` | `UserVa` dari BO | satu mapping permanen |
| `poll/select` | `WaitForMultipleObjects` | event di balik fence |
| `eventfd` | `CreateEvent` (manual reset) | |
| `drm_syncobj`/`sync_file` | objek fence shim + Event | §3.2 |
| `dma-buf` (PRIME) | ❌ nanti | §3.6 |
| `pthread` | `CreateThread` | port tipis |
| `clock_gettime(MONOTONIC)` | `QueryPerformanceCounter` | |
| `sysconf(_SC_PAGESIZE)` | 4096 | konstanta |

Prinsip: shim **hanya** menyediakan apa yang dipakai panfrost/panvk. Tidak perlu
meniru seluruh libdrm.

---

## 6. Checklist go/no-go (smoke test PanVK)

| # | Item | Bukti selesai |
| --- | --- | --- |
| 1 | Mesa cross-build `aarch64-windows-msvc` | `libvulkan_mali.dll` / ICD Mesa ter-link |
| 2 | Shim `open`+`GET_PARAM` | PanVK mengenali GPU (gpu_id/arch benar) |
| 3 | Shim BO create/mmap/get_offset | alokasi + tulis baca dari CPU |
| 4 | Submit satu chain kosong via PanVK | `JS_STATUS 0`, fence tersinyal |
| 5 | Fence/syncobj | submit kedua menunggu yang pertama |
| 6 | Compute shader trivial (`gl_GlobalInvocationID`) | buffer berubah sesuai shader |
| 7 | Segitiga offscreen | pixel benar (readback) |
| 8 | (baru) DXVK/Zink di atas PanVK | app sederhana render |

Item 1–2 sudah menghabiskan sebagian besar biaya; item 3–5 adalah shim; item 6–7
adalah titik di mana Mesa harus benar untuk Valhall.

---

## 7. Ringkasan perubahan rencana

- ❌ Sebelumnya dianggap: "KMD perlu banyak fitur baru sebelum PanVK". **Bukan.**
- ✅ Kenyataannya: KMD v2 **cukup** untuk smoke test; yang belum hanya P1/P2
  (non-kontigu, multi-AS, fence eksplisit, cache, dma-buf).
- 🎯 Fokus berikutnya: **build Mesa ARM64 Windows** + **shim Win32/DRM**, dua-duanya
  dikerjakan di sisi kita (dan sebagian bisa diuji tanpa tablet: unit test shim,
  dan build host untuk potongan non-GPU).
- 📌 `mali_jc` adalah tempat memperluas encoding descriptor tanpa menyentuh KMD.

Detail kesiapan toolchain Mesa ada di [`mesa/README.md`](mesa/README.md).
