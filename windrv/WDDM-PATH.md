# Jalur WDDM — membuat Windows mengenali Mali sebagai GPU

> Dokumen ini menjawab satu pertanyaan: **apa yang harus dibangun supaya Mali-G57
> muncul sebagai GPU di Windows (Task Manager, DXGI), dan bagaimana kita sampai
> ke sana tanpa menunggu izin siapa pun.**

## 0. Fakta dasar

Tidak ada driver Mali untuk Windows. Arm tidak pernah membuatnya, MediaTek tidak
pernah membuatnya, dan tidak ada port komunitas. Artinya:

* tidak ada yang bisa di-"port" untuk sisi Windows graphics stack;
* setiap baris yang kita tulis adalah yang pertama di dunia untuk hardware ini;
* tidak ada jalan pintas administratif — yang ada hanya urutan pengerjaan.

KMD akselerator kita (`windrv/kmd`) sudah membuktikan bagian tersulit dari
*hardware*: power, reset, MMU, BO, job submission di silicon nyata. Yang belum
ada adalah **lapisan Windows graphics**.

## 1. Temboknya ada tiga lapis, bukan satu

```
   aplikasi D3D12 / D3D11
            │
   ┌────────▼─────────┐
   │  UMD (user mode) │  lapis 3 — paling tebal
   │  D3D12DDI/D3D11DDI│
   └────────┬─────────┘
   ┌────────▼─────────┐
   │  WDDM KMD        │  lapis 2 — bisa dipanjat, terukur
   │  (dxgkrnl)       │
   └────────┬─────────┘
   ┌────────▼─────────┐
   │  Mali hardware   │  lapis 1 — SUDAH jalan
   └──────────────────┘
```

| Lapis | Yang memberi | Status |
| --- | --- | --- |
| 1. Hardware broker (KMD akselerator) | job, MMU, power, BO di Mali | **jalan di silicon** |
| 2. WDDM display miniport (render-only) | Windows melihat adapter GPU; DXGI bisa enumerate; Task Manager punya baris GPU | belum |
| 3. UMD D3D12 | aplikasi memakai adapter lewat `d3d12.dll` | belum (dan ini yang paling mahal) |

Kesalahan umum: menganggap lapis 2 dan 3 satu paket. **Tidak.** Lapis 2 bisa
berdiri sendiri dan itu yang membuat Mali "dikenali".

## 2. Lapis 2 — WDDM render-only adapter

### 2.1 Kenapa render-only

Adapter render-only = adapter WDDM tanpa output monitor (`NumOfChildren = 0`).
Windows mendukung ini penuh (dipakai kartu compute). Scanout tetap di Basic
Display Adapter, jadi **tidak ada pekerjaan display/DSI/MDP** — persis sesuai
batasan proyek.

### 2.2 Yang harus diimplementasi

Bagian KMD (semua didokumentasi WDK, sample `KernelModeDisplayOnly` sebagai
kerangka):

* `DriverEntry` → `DxgkInitialize()` dengan `DRIVER_INITIALIZATION_DATA`
* Device: `DxgkDdiAddDevice`, `StartDevice`, `StopDevice`, `RemoveDevice`,
  `DxgkDdiDispatchIoRequest` (minimal), `DxgkDdiResetDevice`
* Adapter: `DxgkDdiQueryAdapterInfo` (`DXGK_QUERYADAPTERINFOTYPE`:
  `DRIVERCAPS`, `SEGMENTDESCRIPTOR`, `VIDMMCAPS`, `VIDSCHCAPS`,
  `QUERYSEGMENT`/`QUERYSEGMENT3`), `DxgkDdiQueryChildRelations` (0 anak),
  `DxgkDdiSetPowerState`, `DxgkDdiNotifyAcpiEvent`
* Memory (VidMm): `CreateAllocation`, `DestroyAllocation`,
  `DescribeAllocation`, `GetStandardAllocationDriverData`, `OpenAllocation`,
  `CloseAllocation`, `BuildPagingBuffer`, `SetRootPageTable`
* Execution (VidSch): `CreateContext`/`DestroyContext`, `CreateDevice`,
  `CreateEngine`/`CreateHwQueue` (WDDM 2.x), `Render`/`RenderKm`, `Patch`,
  `SubmitCommand`/`SubmitCommandVirtual`, `PreemptCommand`, `CancelCommand`,
  `QueryEngineStatus`, `ResetEngine`, `CalibrateGpuClock`,
  `FormatHistoryBuffer`, `ControlInterrupt`, `MapCpuHostAperture`
* Sinkronisasi: fence WDDM (`DxgkCbSignalFence`/`DxgkCbWaitForFence`), interrupt
  DPC → `DxgkCbNotifyInterrupt` + `DxgkCbQueueDpc`

Ini bukan pekerjaan kecil, tapi **tertutup**: seluruh kontraknya publik, dan kita
sudah punya sisi hardware-nya (job slot, MMU, IRQ).

### 2.3 Yang dibutuhkan dari sisi lain

* **Node ACPI** untuk GPU (sama dengan yang dibutuhkan item 0.2/IRQ). Satu
  pekerjaan, dua manfaat: IRQ asli + enumerasi PnP.
* **IRQ asli** (bukan polling) — WDDM tidak bisa hidup dengan polling.
* **Page table + multi-AS** (item 0.3/0.4) — VidMm memberi kita allocation
  fisik yang tidak selalu kontigu; identity mode tidak cukup.
* **SMMU** (opsional tapi disarankan) supaya allocation VidMm aman.

### 2.4 Hasil

Setelah lapis 2: Device Manager menampilkan adapter display, DXGI
mengenumerasinya, Task Manager punya baris GPU dengan memori. **Engine 0%**,
karena belum ada yang mensubmit lewat WDDM.

## 3. Lapis 2.5 — supaya angka di Task Manager benar-benar bergerak

Ini ide yang membuat lapis 2 berguna tanpa harus menyentuh lapis 3.

WDDM punya jalur submission publik dari user mode: `D3DKMT*` (dipakai
`dxgi`/`d3d12`, dan bisa dipanggil API lain). Kalau ICD Vulkan kita mengirim
pekerjaan lewat **transport WDDM** dengan format paket milik kita sendiri:

```
ICD Vulkan ──D3DKMTSubmitCommand──► WDDM (VidSch) ──DxgkDdiSubmitCommand──►
            (paket kita)                                    KMD kita ──► Mali
```

maka:

* submission tercatat di scheduler WDDM → **utilisasi engine muncul di Task
  Manager** (bukan 0%);
* kita dapat fence/sinkronisasi WDDM gratis (tidak perlu bikin sendiri);
* VidMm mengelola residency/allocation.

Biaya: `D3DKMT*` sebagian tidak terdokumentasi penuh dan berubah antar versi
WDDM — perlu pin ke WDDM 2.x/3.x yang dipakai build 26100, dan siap menghadapi
perubahan. Ini **riset**, bukan pekerjaan rutin. Tapi jalurnya nyata.

Alternatif kalau `D3DKMT` terlalu rapuh: tetap pakai IOCTL kita sendiri, terima
Task Manager 0%, dan tunjukkan bukti akselerasi lewat aplikasi + counter internal
kita sendiri.

## 4. Lapis 3 — UMD D3D12 (tembok sebenarnya)

Ini yang tidak boleh dibohongi: `d3d12.dll` memanggil `OpenAdapter12` di UMD, dan
UMD harus mengimplementasi tabel `D3D12DDI` (ratusan fungsi: PSO, root signature,
command list, descriptor heap, resource barrier, query, video, dsb.) serta
menghasilkan command buffer yang dimengerti KMD.

Tidak ada implementasi independen di dunia. Yang realistis:

1. **Jangan mulai dari sini.** Akselerasi nyata datang lebih dulu lewat Vulkan.
2. Kalau suatu hari dikerjakan, urutannya: D3D11 UMD minimal (lebih kecil dari
   D3D12) → D3D12 UMD bertahap (feature level 11_0 dulu).
3. Sampai itu ada, `vkd3d-proton` + `DXVK` tetap menjadi jembatan D3D → Vulkan,
   dan itu **bukan** jalan pintas murahan: itu cara Proton membuat game Windows
   jalan di Linux.

## 5. Urutan pengerjaan yang disarankan

| Fase | Isi | Hasil yang bisa dilihat |
| --- | --- | --- |
| W0 | ACPI node GPU + IRQ asli | `!irq` melaporkan 3 line, bukan 0 |
| W1 | Page table + multi-AS + non-kontigu | BO 256 MB, 2 context paralel |
| W2 | Job chain + fence/timeline | submit async, wait tanpa polling |
| W3 | **WDDM render-only adapter** | Mali muncul di Device Manager + Task Manager |
| W4 | (opsional) submission lewat D3DKMT | angka engine bergerak di Task Manager |
| V1..V4 | Vulkan ICD → render → DXVK → vkd3d | aplikasi benar-benar dipercepat |
| W5 | (opsional, jangka panjang) UMD D3D12 | D3D12 native tanpa translasi |

W3 sengaja **setelah** W0–W2 karena ketiganya dipakai bersama: WDDM tanpa IRQ asli
dan tanpa page table akan langsung mentok di `DxgkDdiSubmitCommand`.

## 6. Batasan yang tetap berlaku (bukan alasan berhenti)

* Scanout tetap di Basic Display Adapter sampai ada display miniport MDP/DSI.
  WDDM render-only tidak mengubah itu.
* Driver ARM64 kernel untuk distribusi wajib Microsoft-signed. Dev: test signing.
* `D3DKMT` dan sebagian `DxgkDdi*` untuk jalur non-D3D tidak didokumentasi penuh;
  ekspektasi iterasi.
* Semua ini berjalan di hardware retail tanpa dokumentasi internal Arm. Setiap
  kemajuan harus diverifikasi di silicon, bukan diasumsikan.

## 7. Prinsip kerja

Tidak ada yang "tidak mungkin" di sini — yang ada adalah **urutan** dan
**biaya**. Setiap fase di atas punya definisi selesai yang bisa diuji di tablet.
Kalau satu jalur buntu (misalnya `D3DKMT` ditolak OS), kita turun satu tingkat
dan tetap punya hasil: adapter tetap muncul, akselerasi tetap lewat Vulkan.
