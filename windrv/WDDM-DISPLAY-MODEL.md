# Model display Windows — yang sebenarnya dilakukan Snapdragon X Elite, dan artinya untuk MT6789

> Dokumen ini menjawab satu pertanyaan: **kenapa GPU iGPU di Windows (Adreno X1-85)
> bisa menampilkan desktop dengan baik, sementara Mali-G57 kita belum bisa?**
> Jawabannya bukan "Adreno punya scan-out", dan bukan "Windows tidak punya HWC".
> Windows **punya** padanan HWC — namanya MPO — dan punya jalur present lintas
> adapter bernama CASO. Dokumen ini memetakan keduanya ke hardware MT6789, lalu
> menetapkan arsitektur target yang mengikuti bentuk hardware kita.

Terkait: [`WDDM-PATH.md`](WDDM-PATH.md) (lapisan WDDM), [`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md)
(stack Android + tangga L1–L5), [`GRAPHICS-STACK.md`](GRAPHICS-STACK.md) (Vulkan/D3D),
[`ROADMAP.md`](ROADMAP.md) (fase).

---

## 0. Ringkasan

1. **Snapdragon tampil baik karena Qualcomm mengirim satu miniport WDDM yang
   memiliki GPU *dan* display controller sekaligus**, lalu miniport itu
   mengimplementasi **MPO** (multiplane overlay). MPO = padanan HWC di Android:
   layer disusun hardware saat scan-out, bukan diblit GPU tiap frame.
2. **MPO tidak otomatis ada.** Kalau KMD tidak mengimplementasi DDI MPO, DWM
   jatuh ke *composition mode*: satu komposisi penuh + satu tulis layar penuh
   setiap frame. Itu yang terjadi pada kita sekarang (Basic Display Adapter).
3. **Windows 11 punya CASO** (WDDM 2.9): render di adapter A, **scan-out langsung
   dari cross-adapter resource** di adapter B — satu copy, bukan dua. Ini persis
   bentuk hardware MT6789: Mali render, MMSYS scan-out. **Mali tidak perlu punya
   scan-out.**
4. **Arsitektur target: dua adapter WDDM.** `R` = Mali (render-only, VidMm/VidSch),
   `D` = MMSYS (display-only, VidPn + OVL sebagai plane MPO, klaim
   `CrossAdapterResourceScanout`). Bukan satu miniport raksasa.
5. **Ada jalur display yang jauh lebih murah dan berisiko rendah: IddCx**
   (indirect display, UMDF/user-mode). Ia memberi kita monitor Windows yang
   menerima frame desktop hasil komposisi, lalu kita sendiri yang menggerakkan
   OVL/RDMA/DSI. Biayanya satu copy per frame; risikonya jauh di bawah miniport
   kernel penuh. Cocok sebagai L5b sebelum L5a.

Status bukti: §1 dan §2 bersumber dari dokumen Microsoft + release notes driver
Qualcomm (tertaut di §9). §5–§7 adalah **keputusan desain** kita, bukan kutipan.

---

## 1. Bukti: seperti apa stack Snapdragon X di Windows

Yang bisa diverifikasi dari paket driver publik Qualcomm (v31.0.x / 2026.08.x):

| Fakta | Arti |
| --- | --- |
| GPU muncul di kelas **"Display adapters"** dengan nama `Qualcomm(R) Adreno(TM) X1-85 GPU` | Ini bukan "compute accelerator". Windows hanya menaruh sesuatu di kelas itu kalau ada **miniport display** yang mendaftar lewat `DxgkInitialize`. |
| INF utama `qcdx8380.inf` (`8380` = X1E80100) | Satu INF = satu adapter WDDM yang memegang GPU. |
| **Extension INF** `qcdxext_crd8380.inf` | Data per-board (CRD = reference design) dipisah dari driver inti. |
| Release notes memuat **"display fixes"** (HBR2, port DFP, fallback pembacaan info display) | Miniport itu memang memegang jalur display (link/EDID/mode), bukan hanya render. |
| Paket berisi UMD D3D12/D3D11, OpenCL ICD, dan **DirectX12 Advanced Shader Delivery** (PSDB) | Shader dikompilasi di muka dan di-cache — padanan "shader cache" Android. |

**Kesimpulan §1:** di Windows, "GPU bagus" = satu paket: miniport display + UMD +
data board. Tidak ada satu pun dari ketiganya yang bisa dipisahkan dari hasil
akhir. Ini juga menjelaskan kenapa tabel di `GRAPHICS-STACK.md` §1 kita panjang:
kita mengisi ketiga kolom itu dari nol.

---

## 2. Rantai present Windows — enam mode, biaya berbeda

Dari aplikasi ke panel, Windows punya beberapa mode present. Yang dipakai
ditentukan oleh **DXGI + DWM + kemampuan yang diklaim KMD**, bukan oleh aplikasi.

| Mode | Siapa menyusun gambar | Biaya per frame (1200p) | Syarat |
| --- | --- | --- | --- |
| **Composed (DWM blit)** | DWM mengomposisi semua window ke satu permukaan, lalu menulis ke scan-out | 1 komposisi penuh + 1 tulis layar penuh | selalu tersedia (fallback) |
| **DirectFlip** | Swapchain aplikasi = ukuran layar, DWM tidak ikut | ~0 | swapchain fullscreen, ukuran cocok |
| **iFlip (independent flip)** | Present dikirim langsung ke present manager kernel | ~0 | flip model, tidak ada occlusion |
| **MPO** | Plane 0 = swapchain DWM, plane 1..n = window/video; **hardware** menyusun saat scan-out | ~0 untuk layer yang di-offload | KMD implement DDI MPO |
| **Hardware flip queue** (WDDM 3.0) | Seperti iFlip, tapi beberapa flip boleh diantre | turunkan latency | `MaxQueuedMultiPlaneOverlayFlipVSync`, `DxgkDdiSetInterruptTargetPresentId` |
| **CASO** (WDDM 2.9) | Render di adapter A, scan-out dari resource cross-adapter di adapter B | **1 copy** (bukan 2) | `DXGK_VIDMMCAPS.CrossAdapterResourceScanout` + `CheckMultiplaneOverlaySupport3` |

Tiga hal yang penting untuk kita:

1. **MPO adalah HWC-nya Windows.** Syarat minimumnya (dari dokumen Microsoft):
   blending premultiplied alpha, plane 0 (swapchain DWM) harus bisa
   di-enable/disable dan di-clip, minimal satu plane bisa scale dengan filtering
   lebih baik dari bilinear, dan minimal satu plane mendukung format YUV
   (BT.601 + BT.709). Kalau tidak dipenuhi, DWM **otomatis** kembali ke composed
   mode — bukan error, hanya mahal.
2. **DWM selalu mengomposisi dengan D3D11.** Jadi "desktop dirender Mali"
   (L4) tetap butuh **D3D11 UMD**; MPO tidak mengubah itu. MPO hanya menghindari
   komposisi untuk layer yang bisa di-offload.
3. **CASO menoleransi ukuran sampai 1920×1080.** Panel kita 1920×**1200** —
   di atas batas minimum itu. Artinya driver kita **boleh** menolak scan-out
   (`DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT`) dan DXGI akan jatuh ke jalur dua
   copy. Karena OVL kita sanggup 1920×1200, kita **harus** mendeklarasikan
   dukungan untuk ukuran itu secara eksplisit; kalau tidak, desktop akan tetap
   dua copy tanpa gejala yang jelas.

---

## 3. Padanan Android ↔ Windows (tabel kerja)

| Android (MT6789) | Windows | Catatan |
| --- | --- | --- |
| SurfaceFlinger | DWM | DWM hanya bicara D3D11 |
| HWC (memilih GPU vs OVL) | MPO (`CheckMultiplaneOverlaySupport3`) | keputusan yang sama, API berbeda |
| DRM atomic commit | `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3` | satu commit = satu set plane |
| Plane OVL0 | plane MPO 0..n (didukung OVL) | OVL memang punya beberapa layer |
| dma-buf | cross-adapter resource (VidMm) / allocation KMD | tanpa fd, pakai handle |
| gralloc/ION | `D3DKMTCreateAllocation` | VidMm yang memilih fisik |
| `sync_file`/dma-fence | fence WDDM (`DxgkCbSignalFence`) | model waktu beda |
| kbase IRQ handler | interrupt DPC → `DxgkCbNotifyInterrupt` | WDDM tidak bisa polling |
| vsync dari DSI | vsync VidPn + flip queue | |
| panel init (per-panel) | `DXGKQAITYPE_QUERYINTEGRATEDDISPLAY` + extension INF | data, bukan kode |
| shader cache aplikasi | PSDB / ASD, atau cache DXVK/vkd3d | |

**Inti §3:** tidak ada konsep Android yang "hilang" di Windows. Yang berbeda
adalah **siapa yang harus menuliskannya**: di Android semua baris kanan sudah
ada di kernel/vendor; di Windows baris kanan itu adalah pekerjaan kita.

---

## 4. DDI yang membentuk MPO (checklist implementasi)

Untuk adapter display kita (`D`), MPO berarti:

- [ ] `DxgkDdiGetMultiplaneOverlayCaps` — wajib untuk KMD WDDM 2.2+ yang mau
      lebih dari satu plane.
- [ ] `DxgkDdiCheckMultiplaneOverlaySupport3` — dipanggil DWM untuk memutuskan
      scan-out vs compose. **Wajib** kalau kita mengklaim CASO.
- [ ] `DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay3` — memprogram OVL.
- [ ] `DxgkDdiPostMultiplaneOverlayPresent` — opsional, untuk optimasi state.
- [ ] `DxgkCbMultiPlaneOverlayDisabled` — memberi tahu DWM agar fallback
      (mis. karena bandwidth/thermal) tanpa artefak.
- [ ] Flags `StaticCheck` (WDDM 3.0) di `DXGK_MULTIPLANE_OVERLAY_FLAGS`: dipakai
      DXGI saat membuat swapchain untuk menanyakan "bisa scan-out atau tidak"
      sebelum commit nyata. Jangan cache hasilnya.

Konsekuensi teknis untuk MMSYS: setiap plane MPO = satu layer OVL. Jadi
"dukungan MPO" kita = "berapa layer OVL yang bisa kita program" + format apa
yang bisa dibaca OVL + apakah OVL bisa scale (kualitas filtering).

---

## 5. Dua jalur "panel digerakkan oleh kita"

### 5a. Miniport display WDDM penuh (L5a) — kualitas tertinggi, risiko tertinggi

VidPn, mode set, MPO, vsync, hotplug, `DxgkDdiCommitVidPn`, per-panel DSI init.
Ini satu-satunya jalan ke **zero-copy + MPO + vsync asli**. Risikonya juga
tertinggi: mode set yang salah bisa mematikan panel dan GOP tidak otomatis
kembali (lihat `DISPLAY-ANDROID.md` §5).

### 5b. IddCx indirect display (L5b) — murah, UMDF, satu copy

IddCx adalah framework Microsoft untuk **indirect display driver (IDD)**:
driver **UMDF (user-mode)** yang mendaftarkan monitor virtual ke Windows. OS
mengomposisi desktop dan menyerahkan hasilnya sebagai swapchain:

```
DWM (D3D11) ──► IDDCX_SWAPCHAIN ──► IDD kita
                                      │ IddCxSwapChainReleaseAndAcquireBuffer
                                      │ (permukaan hasil komposisi)
                                      ▼
                             copy ke BO KMD → program OVL/RDMA/DSI → panel
                             IddCxSwapChainFinishedProcessingFrame
```

Keunggulan untuk kasus kita:

* **tidak ada VidPn, tidak ada MPO, tidak ada mode set kernel** — IddCx yang
  mengurus model monitor;
* **user-mode** ⇒ kesalahan tidak mem-bugcheck sistem, dan tidak butuh
  miniport kernel untuk mencoba;
* monitor muncul normal di Windows (resolusi/refresh dilaporkan sendiri, cursor
  hardware didukung), jadi panel bisa lepas dari BDA tanpa menyentuh GOP.

Biayanya jujur:

* **satu copy penuh per frame.** 1920×1200×4 B = 9,2 MB; pada 60 Hz itu
  **± 550 MB/s** trafik DRAM tambahan (angka 440 MB/s di `DISPLAY-ANDROID.md`
  adalah jalur GDI 24 bpp; WDDM 32 bpp sedikit lebih besar).
* **tidak ada MPO, tidak ada iFlip** ⇒ setiap frame adalah komposisi penuh.
* tetap butuh **render adapter**: kalau Mali belum punya UMD D3D11, DWM tetap
  mengomposisi di BDA/WARP (software). Jadi L5b memperbaiki *jalur ke panel*,
  bukan *kecepatan render*.

Urutan yang masuk akal: **L5b dulu** untuk membuktikan jalur MMSYS/DSI + OVL
dari Windows tanpa risiko kernel, lalu **L5a** kalau zero-copy/MPO sudah
dibutuhkan.

---

## 6. Arsitektur target MT6789 (dua adapter + CASO)

```
   aplikasi D3D12/D3D11 ──DXVK/vkd3d──► Vulkan ICD ──► KMD Mali ──► Mali-G57
                                                            │
                                            cross-adapter resource (VidMm)
                                                            │  satu copy (CASO)
   DWM (D3D11) ────────────────────────────────────────────┤
                                                            ▼
                                        adapter D: MMSYS display-only
                                        plane MPO 0 = swapchain DWM
                                        plane 1..n   = window/video (iFlip)
                                                            │
                                                    OVL0 → RDMA0 → DSI0
                                                            │
                                                         panel
```

Pembagian tanggung jawab:

| Adapter | Hardware | Peran WDDM | Yang wajib ada |
| --- | --- | --- | --- |
| `R` (render) | Mali-G57 | VidMm + VidSch, **tanpa anak** (`NumOfChildren = 0`) | IRQ asli, page table/multi-AS, fence |
| `D` (display) | MMSYS | VidPn + MPO, klaim `CrossAdapterResourceScanout` | OVL programmable, timing DSI, vsync |

Kenapa dipisah, bukan digabung: **Mali tidak punya scan-out.** Satu miniport
gabungan akan memaksa kita memalsukan scan-out dari memori Mali — persis
pekerjaan yang tidak perlu. CASO sudah dirancang untuk kasus "render di satu
adapter, scan-out di adapter lain"; di sistem hybrid biasa urutannya terbalik
(iGPU display + dGPU render), dan itu tidak masalah: CASO tidak peduli siapa
yang mana.

Catatan penting: pada WDDM 3.0+, adapter bertipe **HybridIntegrated** *wajib*
mendeklarasikan `CrossAdapterResourceScanout`. Jadi begitu kita memakai model
hybrid, klaim CASO bukan opsional.

---

## 7. Yang ditiru dan yang tidak dari Qualcomm

Ditiru (bentuknya, bukan kodenya):

* **Satu adapter yang memiliki GPU dan display controller** — untuk kita jadi
  dua adapter, karena hardware kita memang dua blok terpisah dan Windows punya
  CASO untuk itu.
* **Extension INF untuk data board.** DSI init P13001L, PHY/PLL, dan timing
  panel masuk sebagai **data** (extension INF / registry), bukan `#define`.
  Ini sejalan dengan aturan proyek: driver portabel, SoC data-driven.
* **Shader dikompilasi di muka.** Di sisi kita padanannya cache DXVK/vkd3d;
  untuk jangka panjang, pipeline cache Vulkan milik ICD.
* **Urutan bring-up:** render dulu (Task Manager), present kedua, display
  terakhir. Qualcomm pun tidak mulai dari panel.

Tidak ditiru / tidak mungkin:

* firmware GPU dan microcode tertutup (kita tidak punya; kbase r32p1 jadi acuan);
* satu UMD proprietary per-SoC;
* driver bertanda tangan Microsoft — untuk distribusi kita juga butuh itu,
  tapi tidak untuk pengembangan (test signing).

---

## 8. Dampak ke urutan kerja

Tangga di `DISPLAY-ANDROID.md` §5 tetap berlaku, dengan L5 dipecah dan satu
tingkat baru:

| Tingkat | Isi | Hasil | Risiko |
| --- | --- | --- | --- |
| L3 | WDDM **render-only** adapter (Mali) | DXGI + Task Manager melihat GPU | sedang |
| **L3b** | IRQ asli + fence + page table/multi-AS | WDDM bisa submit; angka engine bergerak | sedang |
| **L5b** | **IddCx IDD** di atas MMSYS/OVL/DSI | panel digerakkan kode kita, user-mode | **rendah–sedang** |
| L4 | UMD D3D11 | desktop dirender Mali | besar |
| **L5a** | miniport display penuh + **MPO** | zero-copy, vsync asli, layer di-offload | besar + risiko brick |
| **L5c** | **CASO** antar adapter R→D | satu copy, bukan dua | besar |

Urutan yang direkomendasikan: **L3 → L3b → L5b → L4 → L5a → L5c**.
L5b sengaja sebelum L4/L5a: ia membuktikan MMSYS/DSI/OVL dari Windows dengan
kegagalan yang tidak mem-brick sistem, dan itu prasyarat pengetahuan untuk L5a.

Yang **tidak** berubah: L3/L3b tetap langkah berikutnya setelah PanVK tervalidasi,
dan semuanya masih menunggu akses perangkat (USB) untuk diverifikasi.

---

## 9. Prasyarat toolchain (diverifikasi di mesin ini, 2026-11-10)

Sebelum menulis satu baris WDDM pun, satu pertanyaan harus dijawab: **bisakah
driver WDDM/IddCx dikompilasi di sini, tanpa install apa pun?** Jawabannya
**ya** — tapi tidak dengan ekstraksi WDK yang dipakai saat bring-up.

Ekstraksi WDK yang lama hanya berisi header kernel (`km`) + beberapa header
user-mode, jadi ia cukup untuk driver KMDF akselerator tetapi **tidak** untuk
grafis. Yang hilang, dan dari mana asalnya:

| Kebutuhan | Berkas | Sumber |
| --- | --- | --- |
| DDI miniport WDDM | `Include\<ver>\shared\d3dkmddi.h` | paket WDK NuGet (`...\windows-wdk\*.nupkg`) |
| header miniport display | `Include\<ver>\km\dispmprt.h` | paket WDK NuGet |
| **link** `DxgkInitialize` | `Lib\<ver>\km\ARM64\displib.lib` | paket WDK NuGet |
| IddCx (L5b) | `Include\<ver>\um\iddcx\1.10\IddCx.h` + `Lib\...\um\ARM64\iddcx\1.10\iddcxstub.lib` | paket WDK NuGet |
| UMDF | `Lib\wdf\umdf\ARM64\<ver>\WdfDriverStubUm.lib` | paket WDK NuGet |
| `d3dkmdt.h`, `d3dkmthk.h`, `dxgi*.h` | — | **SDK**, `Include\<ver>\shared` |

Dua hal yang mudah salah dan sudah dipastikan di sini:

1. **Tidak ada `dxgkrnl.lib`.** `DxgkInitialize` dan
   `DxgkInitializeDisplayOnlyDriver` ternyata **didefinisikan di
   `displib.lib`** (dicek dengan `llvm-nm --defined-only` → `T DxgkInitialize`).
   Ini yang harus di-link oleh miniport, bukan `dxgkrnl.lib`.
2. **Header display dibagi dua root.** `d3dkmddi.h` dari WDK, `d3dkmdt.h` dari
   SDK. Salah root = build gagal di `cannot open d3dkmdt.h`.

Reproduksi (tanpa install, tanpa elevasi, tanpa jaringan — paket dibaca di
tempatnya):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File windrv\kmd\tools\extract-wdk.ps1
```

Script itu mengekstrak payload `c/` dari paket ke `external\wdk` (git-ignored)
lalu mencetak verdict per kelompok: `kernel`, `WDDM`, `IddCx`, `SDK`. Kondisi
sekarang: **keempatnya `ready`**. `windrv\kmd\build.ps1` tetap hijau dengan WDK
baru ini (KMDF 1.33, `MaliKmd.sys` 45.056 B).

Artinya: **L3 (miniport WDDM), L5a (miniport display), dan L5b (IddCx) bisa
ditulis dan dikompilasi hari ini.** Yang masih menunggu perangkat hanyalah
menjalankannya. Blocker yang tersisa untuk grafis bukan lagi toolchain.

---

## 10. Referensi

Microsoft Learn:

* Multiplane overlay support — `display/multiplane-overlay-support`
* Multiplane overlay hardware requirements — `display/multiplane-overlay-hardware-requirements`
* Hardware flip queue (WDDM 3.0) — `display/hardware-flip-queue`
* Supporting cross-adapter resource scan-out (CASO, WDDM 2.9) — `display/supporting-caso`
* Indirect display driver model / IddCx — `display/iddcx-objects`, sample `video/IndirectDisplay/IddSampleDriver`
* For best performance, use DXGI flip model — `windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model`
* `DXGK_QUERYADAPTERINFOTYPE` (termasuk `QUERYINTEGRATEDDISPLAY`) — `ddi/d3dkmddi/ne-d3dkmddi-_dxgk_queryadapterinfotype`

Qualcomm:

* Adreno Windows Graphics Driver release notes (v31.0.x, 2026.08.x) — nama INF
  `qcdx8380.inf`, extension `qcdxext_crd8380.inf`, kelas "Display adapters".
* DirectX12 Advanced Shader Delivery untuk Snapdragon X2 (blog Qualcomm, 2026-10).
