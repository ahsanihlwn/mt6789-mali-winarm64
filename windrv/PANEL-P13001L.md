# Panel P13001L — apa yang sudah terukur, dan apa artinya untuk driver display Windows

> Dokumen ini menjawab satu pertanyaan: **panel di tablet ini apa, dan kalau
> Windows mau menggerakkan sendiri, datanya dari mana?** Semua angka di sini
> terukur dari artefak perangkat yang ada (boot log stock + device tree stock),
> bukan dari tebakan.

Terkait: [`DISPLAY-MMSYS-REGS.md`](DISPLAY-MMSYS-REGS.md) (peta blok display),
[`WDDM-DISPLAY-MODEL.md`](WDDM-DISPLAY-MODEL.md) (MPO/CASO/IddCx),
[`ROADMAP.md`](ROADMAP.md) (fase L5).

---

## 1. Panel yang terpasang

| Properti | Nilai | Sumber |
| --- | --- | --- |
| LCM name (vendor) | `m100t_ft8205_jt130wuf001a01_pen_wuxga` | log boot LK stock: `lcm.driver.name`, `mt_disp_init ... found:1` |
| DSI bridge / IC | FocalTech **FT8205** | dari nama LCM + dependensi modul `cust_panel.ko` (`it6112`, `mtk_panel_ext`) |
| Kode panel | `JT130WUF001A01` | dari nama LCM |
| Resolusi aktif | **1200 × 1920** (portrait) | log LK: `physical_screen.width=1200 height=1920` |
| Refresh | **60.06 Hz** | log LK: `mt_disp_get_lcd_time, fps=6006` |
| Jalur | DSI 4 lane, RGB888, LPM aktif | device tree: `cust,dsi-lane-number=4`, `cust,dsi-color-format="rgb888"`, `cust,mipi_dsi_mode_lpm=1` |

Catatan penting: device tree mengiklankan **1200 × 2000** untuk panel
"wuxga2000" (kandidat `m100t_ft8201_xy2210d4001a_wuxga2000_pt2` dan
`m100t_jd9366tc_sq110aq7ez51394l501_wuxga2000_jst_864`), tetapi yang benar-benar
terscan-out adalah **1200 × 1920**. Row ekstra itu virtual — bahkan firmware
workspace mencatat bahwa rentang koordinat sentuh (0,0,1200,2000) tidak cocok
dengan scan-out 1200×1920. **Bootloader adalah acuan** untuk apa yang ada di
kaca.

## 2. Konfigurasi panel = data, bukan kode

Perangkat ini tidak punya driver panel khusus; konfigurasi panelnya datang
dari device tree sebagai properti. Skemanya (dari node `/lcds/<panel>`):

| Properti | Isi | Dipakai untuk |
| --- | --- | --- |
| `display-timings/timing0` | `clock-frequency`, `hactive`, `vactive`, `hback-porch`, `hfront-porch`, `vback-porch`, `vfront-porch`, `hsync-len`, `vsync-len` | mode video DSI |
| `cust,dsi-lane-number` | 4 | jumlah lane |
| `cust,dsi-color-format` | `rgb888` | format piksel |
| `cust,dsi-work-mode` | 4 (video mode) | mode DSI |
| `cust,mipi_dsi_mode_lpm` | 1 | low-power mode |
| `cust,dsi-non-continuous-clock` | — | DSI clock kontinu/non-kontinu |
| `cust,pll_clk`, `cust,data_rate` | nilai PHY | laju data DSI |
| `cust,lcm_phy_size` | 1036 | ukuran PHY bridge |
| `cust,esd-check-*` | enable/mode/register/value | deteksi panel |
| `cust,power-on-sequence` | 96 byte | urutan power-on panel |
| `cust,power-off-sequence` | 60 byte | urutan power-off |
| `cust,initial-command` | 1347 byte | DCS init sequence |
| `cust,sleep-in-command`, `cust,sleep-out-command` | 6 byte | sleep |
| `cust,ssc_enable`, `cust,rotate`, `cust,fps` | — | SSC, rotasi, fps |

**Inilah bentuk yang harus kita pakai di Windows.** Driver display Windows kita
harus membaca deskriptor panel dari data (INF/registry), persis seperti:

* MediaTek di Android: properti device tree;
* Qualcomm di Windows: `qcdx8380.inf` (base) + `qcdxext_crd8380.inf` (data
  board/OEM).

Desainnya sudah dibuat di `windrv/display/`:
`include/mtk_disp_panel.h` + `src/MtkDispPanel.c` (tabel P13001L), dengan flag
yang jujur menandai apa yang sudah terukur dan apa yang belum.

## 3. Yang belum terukur (dan cara mengukurnya)

| Yang hilang | Kenapa penting | Cara dapat |
| --- | --- | --- |
| porch/sync aktual (hbp, hfp, hsync, vbp, vfp, vsync) | DSI host tidak bisa menyetel mode tanpa ini | tabel LCM di dalam LK, atau baca register DSI yang sedang dipakai saat boot |
| pixel clock | menentukan laju DSI | sama seperti di atas |
| `data_rate` / `pll_clk` | configuring PHY DSI | tabel LCM / register PHY (`0x11F60000`) |
| DCS init sequence (1347 byte) | tanpa init, panel bisa tetap gelap | `cm_init_cmd` di `cust_panel.ko` (sudah terpetakan di artefak firmware workspace) |
| power-on / power-off sequence | urutan rail panel | device tree / modul kernel |

Semua ini **bisa** diambil tanpa USB (dari LK binary, dari `cust_panel.ko`,
dari device tree board), tapi butuh membongkar struktur data vendor — pekerjaan
berikutnya, bukan pekerjaan yang perlu perangkat.

## 4. Konsekuensi besar: panel di-hand-over dari firmware

Firmware (LK → GOP → Windows) **sudah** menyalakan panel sebelum Windows jalan.
Artinya:

1. Driver Windows kita **tidak boleh** mengulang inisialisasi panel dengan
   urutan berbeda — kalau DCS init-nya beda, panel bisa gelap dan tidak
   kembali sendiri.
2. Sebaliknya, kalau driver kita me-*re-init* dengan urutan identik, itu aman.
   Karena itu DCS init sequence harus **sama persis** dengan yang dipakai LK.
3. Ada mekanisme hand-over di firmware (`LkHandoffDxe`, `NativeScanout.c`),
   dan GOP sudah menggambar di framebuffer native 1200×1920.

Konsekuensi untuk urutan kerja: **ambil DCS init sequence dari artefak vendor
sebelum menulis satu baris display miniport**, karena itu yang menentukan
apakah panel mau hidup atau mati.

## 5. Sumber

* Log boot LK stock: `firmware workspace .cache/p13001l-port/evidence/hardware-stock-r4/pl_lk.log`
  (baris `lcm.driver.name`, `mt_disp_init`, `mt_disp_get_lcd_time`, `physical_screen`).
* Device tree stock: `.../evidence/stock-contract/lk-handoff-fixture.dtb`
  (dibaca dengan `windrv/display/tools/dtb_read.py`, node `/lcds`).
* Modul kernel stock `cust_panel.ko`: `.../P13001L/vendor_boot/ramdisk/lib/modules/cust_panel.ko`
  (modinfo: `depends=csci_driver,it6112,cust_gpios,middle_misc,mtk_panel_ext`).
* Device tree vendor (schema DSI/OVL): `windrv/display/tools/fetch-mtk-display-ref.ps1`
  → `external/mtk-display-ref/` (MTK DRM/DSI sources, dengan manifest SHA-256).
