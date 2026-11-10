# MMSYS/DSI — inventaris register yang terukur (MT6789)

> Dokumen ini adalah **peta alamat**, bukan rencana. Semua angka di sini
> diambil dari device tree vendor `mt6789.dts` (Xiaomi emerald,
> `adomerle/android_kernel_xiaomi_emerald@6931719`), bukan dari tebakan.
> Tujuannya satu: supaya siapa pun yang menulis jalur display Windows (L5a/L5b)
> tidak perlu menebak di mana OVL, RDMA, DSI, PHY, MUTEX, SMI, dan IOMMU berada.

Terkait: [`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md) (konsep + tangga L1–L5),
[`WDDM-DISPLAY-MODEL.md`](WDDM-DISPLAY-MODEL.md) (MPO/CASO/IddCx),
[`HARDWARE-MT6789.md`](HARDWARE-MT6789.md) (GPU 0x13000000).

---

## 0. Tiga hal yang sering terlewat

Sebelum tabel, tiga fakta yang menentukan bentuk driver display kita:

1. **MMSYS membaca memori lewat IOMMU sendiri.** `disp_iommu` (M4U) adalah blok
   terpisah dari MMU Mali. Buffer yang akan di-scan-out harus dipetakan ke M4U,
   atau M4U di-bypass secara eksplisit. Tanpa ini OVL akan fault, bukan
   menampilkan gambar.
2. **Register display tidak diprogram langsung dari CPU saat commit.** Vendor
   memakai **GCE/CMDQ** (command queue hardware) + **MUTEX** supaya perubahan
   register mendarat di batas vsync (`disp_mutex0_eof`, `disp_dsi0_te`,
   `disp_rdma0_eof0`). Ini padanan `drm_atomic_commit` di Android dan padanan
   "flip di vsync" di Windows.
3. **PHY DSI adalah blok ketiga.** Host DSI di `0x14013000`, tapi PHY/PLL-nya di
   `0x11F60000`. Salah PHY = tidak ada clock lane = panel gelap.

---

## 1. Peta blok (dari `mt6789.dts`)

| Blok | Alamat | Ukuran | IRQ (GIC_SPI) | GSIV | Catatan |
| --- | --- | --- | --- | --- | --- |
| `dispsys_config` (MUTEX/clock glue) | `0x14000000` | 0x1000 | — | — | `compatible = "mediatek,mt6789-disp"`, power domain `DISP` |
| `disp_mutex0` | `0x14001000` | 0x1000 | — | — | kepemilikan jalur display |
| `smi_larb0` | `0x14003000` | 0x1000 | — | — | SMI/LARB untuk OVL0 |
| `disp_ovl0` | `0x14005000` | 0x1000 | 287 | 319 | overlay utama, LARB0 |
| `disp_ovl0_2l` | `0x14006000` | 0x1000 | 288 | 320 | overlay 2-layer, LARB1 |
| `disp_rdma0` | `0x14007000` | 0x1000 | 289 | 321 | memori → stream untuk DSI |
| `disp_rsz0` | `0x14008000` | 0x1000 | 290 | 322 | resizer |
| `disp_color0` | `0x14009000` | 0x1000 | 291 | 323 | color matrix |
| `disp_aal0` | `0x1400C000` | 0x1000 | 294 | 326 | adaptive average (gamma) LUT |
| `disp_gamma0` | `0x1400D000` | 0x1000 | 295 | 327 | gamma LUT |
| `disp_postmask0` | `0x1400E000` | 0x1000 | 296 | 328 | post mask |
| `dsi0` (host) | `0x14013000` | 0x1000 | 301 | 333 | `mediatek,mt6789-dsi` |
| `disp_wdma0` | `0x14014000` | 0x1000 | 302 | 334 | writeback (tidak dipakai scan-out) |
| `disp_iommu` | `0x14016000` | 0x1000 | — | — | M4U display, bank 0 |
| `disp_iommu_bank1..4` | `0x14017000`..`0x1401A000` | 0x1000 | 304..307 | 336..339 | satu bank per LARB |
| `mipi_tx_config0` (PHY) | `0x11F60000` | — | — | — | `clock-names = "hs"`, `phy-names = "dphy"` |

GSIV = `GIC_SPI + 32` (GICD GSI base = 0, lihat
[`ACPI-GPU-NODE.md`](ACPI-GPU-NODE.md) §8). Angka GSIV inilah yang nanti masuk
ke `Interrupt()` di SSDT — bukan nomor SPI.

Jalur DDP utama, **diukur dari DTS MT6789** (blok di tengah.path adalah wiring
tetap, bukan routing runtime):

```
OVL0 -> COLOR0 -> AAL0 -> GAMMA0 -> RDMA0 -> DSI0 -> PHY(0x11F60000) -> panel
```

Perhatikan: di MT6789 blok setelah AAL0 adalah **GAMMA0**, bukan UFOE (UFOE itu
nama blok MT6357-era). `DISPLAY-ANDROID.md` §1 masih menulis UFOE; itu yang
benar adalah GAMMA0. Semua blok di antara OVL dan RDMA harus di-clock dan
di-enable walau fiturnya tidak dipakai.

---

## 2. Yang harus hidup sebelum satu piksel keluar

Urutan ini diambil dari `dispsys_config` di DTS + perilaku `mtk_drm_ddp_comp.c`:

1. **Power domain `DISP`** (`scpsys`, `MT6789_POWER_DOMAIN_DISP`) — MTCMOS.
2. **Clock**: `CLK_MM_DISP_26M`, `CLK_MM_APB_BUS`, `CLK_MM_DISP_MUTEX0`
   (wajib untuk register access), lalu per-blok: `CLK_MM_DISP_OVL0`,
   `CLK_MM_DISP_RDMA0`, `CLK_MM_DSI0`, `CLK_MM_DSI0_DSI_CK_DOMAIN`.
3. **SMI/LARB**: `smi_larb0` (OVL0), `smi_larb1` (OVL0_2L, RDMA0, WDMA0).
   Tanpa LARB aktif, OVL tidak bisa membaca memori.
4. **M4U display**: `disp_iommu` + bank sesuai LARB, atau bypass.
5. **MUTEX**: ambil `disp_mutex0` untuk jalur ini.
6. **DSI PHY** (`0x11F60000`): PLL + lane, lalu host DSI timing.
7. **OVL**: layer enable, format, address, offset/size, blend.
8. **RDMA**: source = output OVL, ukuran, format, SOF/EOF.
9. **DSI**: video mode timing, lane count, `SOF`/`EOF` sebagai fence.

---

## 3. Padanan ke Windows

| MMSYS | Windows (L5a miniport) | Windows (L5b IddCx) |
| --- | --- | --- |
| `disp_mutex0` | `DxgkDdiCommitVidPn` / locking internal | tidak dipakai |
| OVL0 layer | plane MPO (`SetVidPnSourceAddressWithMultiPlaneOverlay3`) | satu surface hasil copy |
| RDMA0 | output scan-out VidPn source | sama |
| DSI0 + PHY | timing mode (`QueryIntegratedDisplay`, `CommitVidPn`) | mode dilaporkan lewat IddCx |
| `disp_rdma0_eof0` | vsync/flip fence | `IddCxSwapChainFinishedProcessingFrame` |
| `disp_iommu` | allocation VidMm (fisik dari OS) | BO KMD yang dipetakan ke M4U |
| GCE/CMDQ | tidak ada padanan langsung; ganti dengan commit di DPC + fence | tidak perlu (user-mode) |

Poin penting: **IddCx (L5b) menghapus kebutuhan GCE/CMDQ.** Karena kita tidak
mengomposisi di hardware, kita hanya perlu "tulis frame lalu program RDMA di
batas vsync" — dan itu bisa dilakukan dari user-mode dengan menunggu EOF
sebelum menulis register. Ini alasan lain L5b lebih murah daripada L5a.

---

## 4. Pertanyaan yang masih terbuka (dan di mana jawabannya)

| Pertanyaan | Kenapa penting | Di mana dicari |
| --- | --- | --- |
| Timing panel P13001L (h/v sync, porch, clock, lane rate) | tanpa ini DSI tidak mengunci | sudah separación di [`PANEL-P13001L.md`](PANEL-P13001L.md); sisa: porch/clock |
| Initial code DSI (per-panel) | panel tertentu butuh sequence khusus | `cust_panel.ko` → `cm_init_cmd` (1347 byte) |
| Nilai register OVL/RDMA/DSI | kita hanya punya alamat, belum field | `external/mtk-display-ref/` (MTK DRM/DSI sources) |
| Apakah M4U bisa di-bypass | menyederhanakan L5b secara drastis | `mtk_iommu` (bit bypass di bank), atau uji langsung |
| DSC dipakai atau tidak | mengubah jalur RDMA→DSI | `clk-mt6789-disp_dsc.c` + DTS board |

`mt6789.dts` generik ini **tidak** memuat node panel; ia hanya mendefinisikan
controller. Timing panel harus diambil dari DTS board P13001L — itu pekerjaan
berikutnya, dan itu pekerjaan yang menentukan apakah L5a/L5b bisa jalan.

---

## 5. Kode yang sudah ada

Peta di atas sudah dikodekan sebagai tabel per-SoC, persis pola `MaliSoc.c`:

* `windrv/display/include/mtk_disp_soc.h` — deskriptor: region (base/size/nama/
  SPI), rantai scan-out, nama clock, domain power.
* `windrv/display/src/MtkDispSoc.c` — tabel **MT6789** (satu-satunya entri saat
  ini) + `MtkDispGetSocDescByName()` + `MtkDispGsiFromSpi()` (Windows GSI =
  SPI + 32).
* `windrv/display/tools/disp_soc_test.c` — 203 pemeriksaan host: alignment,
  tidak ada region yang tumpang tindih, pemetaan GSI tertutup, rantai scan-out
  lengkap, clock ada di daftar, dan deskriptor panel konsisten. Dijalankan oleh
  `tools\run-host-tests.ps1`.
* `windrv/display/tools/dtb_read.py` — pembaca device tree tanpa dependency,
  untuk mengambil konfigurasi panel dari DTB stock.
* `windrv/display/tools/fetch-mtk-display-ref.ps1` — mengambil sumber DRM/DSI
  MediaTek ke `external/mtk-display-ref/` (dengan manifest SHA-256).

Menambah SoC lain = menambah satu `MTK_DISP_SOC_DESC`. Tidak ada kode lain yang
SoC-specific.

## 6. Sumber

* `arch/arm64/boot/dts/mediatek/mt6789.dts` (Xiaomi emerald, commit `6931719`):
  `disp_ovl0` L4694, `disp_ovl0_2l` L4716, `disp_rdma0` L4738, `disp_rsz0` L4756,
  `disp_color0` L4764, `dsi0` L4869, `disp_wdma0` L4887, `disp_iommu_bank1..4`
  L4903–L4928, `disp_iommu` L4931, `mipi_tx_config0` L4859,
  `dispsys_config` L4502, `disp_mutex0` L4686, `smi_larb0` L4344.
* `drivers/gpu/drm/mediatek/mtk_disp_ovl.c` dan `mediatek_v2/mtk_disp_ovl.c`
  (tersedia di `kernel-reference`, dipakai untuk §2).
