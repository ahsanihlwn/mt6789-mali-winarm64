# Display di MT6789 — cara Android melakukannya, dan artinya untuk Windows

> Dokumen ini menjawab permintaan "beri akses display ke GPU". Jawaban singkatnya:
> di MT6789 **Mali tidak pernah memegang layar** — di Android pun tidak. Yang
> memegang layar adalah *display controller* MediaTek (MMSYS). Dokumen ini
> memetakan stack Android ke stack Windows, lalu menetapkan tangga yang realistis
> menuju "GPU dipakai untuk layar".

Terkait: [`WDDM-PATH.md`](WDDM-PATH.md) (lapisan Windows), [`ROADMAP.md`](ROADMAP.md)
(urutan fase), [`HARDWARE-MT6789.md`](HARDWARE-MT6789.md) (peta register).

---

## 0. Kesimpulan lebih dulu

1. **Mali = render device.** Ia menulis ke memori. Ia tidak punya scan-out, tidak
   punya timing generator, tidak tahu apa itu panel.
2. **Layar = MMSYS.** OVL (blend layer) → RDMA (baca memori jadi stream) → DSI
   (kirim ke panel) + DSI PHY/PLL. Ini blok yang berbeda dari `0x13000000`.
3. **Android menyambung keduanya dengan dma-buf zero-copy.** GPU menulis ke
   buffer, OVL membaca buffer yang sama. Tidak ada copy.
4. **Windows tidak punya dma-buf.** Padanannya: satu *allocation* milik KMD yang
   bisa dipetakan ke GPU VA *dan* punya alamat fisik untuk display controller.
5. **"Task Manager kenal" dan "layar pakai GPU" adalah dua pekerjaan berbeda.**
   Yang pertama = adapter WDDM (render-only). Yang kedua = UMD D3D (karena DWM
   hanya bicara D3D), dan baru setelah itu display miniport MMSYS kalau ingin
   lepas dari Basic Display Adapter.

---

## 1. Stack Android di MT6789 (dari tree vendor, bukan asumsi)

Sumber: `android_kernel_mediatek_mt6789/drivers/gpu/drm/mediatek/`.

| Berkas | Peran |
| --- | --- |
| `mtk_drm_drv.c` | DRM/KMS master: membuat CRTC, plane, connector, memilih path DDP |
| `mtk_drm_crtc.c` | atomic modeset, komposisi layer, vsync |
| `mtk_drm_plane.c` | plane → komponen DDP |
| `mtk_drm_ddp.c` | pemetaan "path" (jalur kabel tetap antar komponen) |
| `mtk_drm_ddp_comp.c` | dispatch per komponen (`enable`/`disable`/`start`/`stop`) |
| `mtk_disp_ovl.c` | overlay: blending beberapa layer dari memori |
| `mtk_disp_color.c` | color matrix / koreksi |
| `mtk_disp_rdma.c` | read DMA: memori → stream untuk DSI/DPI |
| `mtk_dsi.c` | host MIPI DSI (command, timing, video mode) |
| `mtk_drm_gem.c` | GEM object + ekspor/impor dma-buf |
| `disp-mutex` | kepemilikan jalur display (siapa boleh menggerakkan path) |

Power/clock: MTCMOS domain `DISP` di `mtk-scpsys-mt6789.c`, clock
`clk-mt6789-mmsys.c` dan `clk-mt6789-disp_dsc.c`.

**GPU ada di driver yang sama sekali lain:** `mali_kbase` (`/dev/mali0`), bukan
bagian dari DRM/KMS. Di Android kedua driver itu tidak saling tahu; mereka hanya
bertemu di buffer.

Path display yang dideklarasikan `mtk_drm_drv.c` (contoh jalur utama):

```
OVL0 -> COLOR0 -> AAL0 -> RDMA0 -> UFOE -> DSI0
```

Artinya: overlay mem-blend layer yang ada di memori, RDMA membacanya ulang
menjadi stream, DSI mengirimkannya ke panel. GPU tidak muncul di rantai itu sama
sekali — hanya sebagai penulis isi buffer yang dibaca OVL.

Userspace Android:

```
App ──EGL/GLES──► Mali (kbase) ──► buffer dma-buf
                                      │  (buffer yang sama, tanpa copy)
SurfaceFlinger ──HWC──► DRM atomic commit ──► OVL/RDMA/DSI ──► panel
```

`gralloc` (vendor) mengalokasikan buffer ION/dma-buf; SurfaceFlinger memilih
komposisi GPU atau overlay HWC; HWC mengirim atomic commit yang menunjuk alamat
buffer ke plane OVL.

---

## 2. Satu frame, langkah demi langkah

1. App meminta surface; `gralloc` mengalokasikan dma-buf, mengembalikan fd.
2. App membuat EGLImage dari fd; `mali_kbase` meng-impor dma-buf dan memetakannya
   ke GPU VA.
3. Mali merender ke buffer itu (tile writeback selesai, fence disinyalir).
4. SurfaceFlinger mengomposisi (GPU atau HWC overlay).
5. HWC `atomic commit`: plane OVL menunjuk buffer final.
6. OVL + RDMA + DSI scan-out; vsync kembali menjadi fence.
7. Buffer di-recycle setelah fence selesai.

Inti desainnya: **satu alokasi, dua konsumen** (GPU dan display), dengan fence
eksplisit di setiap perpindahan tangan.

---

## 3. Yang bisa dipinjam untuk Windows

* **Model buffer bersama.** Satu alokasi KMD: punya GPU VA (untuk Mali) dan
  alamat fisik (untuk MMSYS). Ini sudah setengah ada di KMD kita (`MaliBo`,
  physically contiguous).
* **Pipeline display deterministik.** Path OVL→RDMA→DSI adalah jalur kabel
  tetap; tidak ada routing runtime. Kalau nanti menulis display miniport, ini
  yang harus dipetakan ke VidPn.
* **Fence di batas kepemilikan.** GPU selesai → fence → display boleh membaca.
  Di Windows padanannya fence WDDM (`DxgkCbSignalFence`) atau fence internal kita.
* **Komposisi terpisah dari scan-out.** Di Android: SurfaceFlinger vs HWC. Di
  Windows: DWM vs VidPn.

---

## 4. Yang **tidak** bisa dipinjam

| Android | Windows | Konsekuensi |
| --- | --- | --- |
| DRM/KMS (`drm_atomic_commit`) | WDDM (`VidPn`, `VidMm`, `VidSch`) | kontrak kernel beda total; tidak ada terjemahan langsung |
| dma-buf | WDDM allocation / handle IOCTL kita | perlu desain sharing sendiri |
| SurfaceFlinger (bisa GPU, bisa overlay) | DWM (hanya D3D11) | desktop tidak bisa "pakai Vulkan"; butuh D3D11 UMD |
| HWC + DRM plane | `DxgkDdiCommitVidPn` + `DxgkDdiSetVidPnSourceAddress` | scan-out lewat miniport, bukan ioctl |
| vsync = dma-fence | vsync = VidPn / fence VidSch | model waktu berbeda |
| gralloc/ION | `D3DKMTCreateAllocation` (WDDM) atau IOCTL kita | — |

Poin ketiga adalah yang paling sering disalahpahami: **di Windows tidak ada
"desktop berbasis Vulkan"**. DWM mengomposisi dengan D3D11. Jadi kalau tujuannya
"desktop benar-benar dirender Mali", yang dibutuhkan adalah **D3D11 UMD**, bukan
Vulkan, bukan display miniport.

---

## 5. Tangga "akses display" di Windows

| Tingkat | Yang dibutuhkan | Yang terlihat pengguna | Biaya |
| --- | --- | --- | --- |
| **L1** Mali render offscreen | KMD akselerator + Vulkan/GL ICD | tes render lolos, belum ada di layar | sedang (jalan) |
| **L2** Mali render, hasil tampil di window | Vulkan WSI + blit ke framebuffer BDA | aplikasi dipercepat; desktop tetap BDA | sedang |
| **L3** Mali muncul di Task Manager | WDDM **render-only** miniport | baris GPU + memori; engine 0% | besar |
| **L4** Desktop/DWM dirender Mali | D3D11 UMD (+ L3) | desktop mulus, game windowed penuh | sangat besar |
| **L5** Panel digerakkan MMSYS langsung | display miniport MMSYS/DSI (+ L3) | lepas dari BDA, mode asli, vsync asli | sangat besar + risiko brick |

Catatan penting per tingkat:

* **L2 tidak butuh WDDM, tidak butuh UMD, tidak butuh display driver.** Satu-
  satunya biaya: satu copy per frame ke framebuffer BDA. Di 1200p60 itu ± 440
  MB/detik — terasa, tapi bukan penghalang, dan ini jalur yang sama yang dipakai
  semua GPU non-WDDM.
* **L3 tidak membuat angka engine bergerak** kecuali submission dirutekan lewat
  WDDM (`WDDM-PATH.md` §3). Task Manager membaca perf counter yang dipublikasikan
  `dxgkrnl`; device non-WDDM tidak punya counter itu sama sekali.
* **L4 adalah tembok sebenarnya**, bukan L3. L3 hanya identitas; L4 baru "dipakai".
* **L5 tidak membuat rendering lebih cepat.** Ia hanya memindahkan scan-out dari
  BDA ke driver kita. Kalau tujuannya "GPU akselerator jalan", L5 tidak menambah
  apa pun kecuali vsync asli dan mode display.

---

## 6. Mengapa urutannya L2 → L3 → L4, dan L5 paling akhir

* L2 memberi hasil yang bisa dilihat (aplikasi memakai Mali) tanpa menyentuh
  kontrak kernel Windows sama sekali. Tidak ada risiko boot.
* L3 memberi yang diminta ("Task Manager kenal dia") dan menjadi fondasi L4/L5.
  Ia bergantung pada pekerjaan yang sudah diketahui: ACPI node + IRQ asli,
  page table/multi-AS, alokasi non-kontigu, fence WDDM.
* L4 butuh D3D11 UMD. Bisa ditunda tanpa memblokir apa pun.
* L5 berisiko tinggi dan **tidak bisa diuji dengan aman tanpa jalur recovery**:
  init sequence DSI bersifat per-panel (P13001L spesifik), ditambah PHY tuning,
  PLL, dan DSC. Kalau salah, panel mati dan GOP tidak otomatis kembali; butuh
  WinPE/UART untuk memulihkan. Karena itu L5 hanya dikerjakan setelah L2–L4
  stabil dan recovery terbukti.

---

## 7. Konsekuensi untuk target proyek

Target yang dipakai sekarang:

```
Fase 1  L1  Mali render (Vulkan/GL)                        ← sedang
Fase 2  L2  present ke window lewat BDA (copy)             ← sesudah L1
Fase 3  L3  WDDM render-only  →  Task Manager kenal        ← target baru
Fase 4  D3D  vkd3d-proton + DXVK di atas Vulkan             ← sesudah L1
Fase 5  L4  D3D11 UMD (desktop di Mali)                    ← opsional, jangka panjang
Fase 6  L5  display miniport MMSYS/DSI                     ← terakhir, butuh recovery
```

Yang **tidak** berubah: KMD akselerator tetap bukan WDDM untuk Fase 1–2 dan 4.
WDDM (L3) adalah adapter **tambahan**, bukan pengganti jalur akselerasi.

---

## 8. Referensi

Lihat [`REFERENCES.md`](REFERENCES.md) §5 untuk tautan DRM/KMS, MediaTek DRM,
dma-buf, WDDM, sample KMDOD, dan D3D12/D3D11 UMD.