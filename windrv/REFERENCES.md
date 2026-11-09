# Referensi eksternal — Mali / MT6789 / Vulkan di Windows ARM64

Kumpulan sumber yang dipakai (atau akan dipakai) untuk proyek driver Mali-G57
MT6789 di Windows 11 ARM64. Semua materi di bawah ini dibaca/di-referensi saja;
kode proyek tetap ditulis sendiri di dalam `windrv\`.

> Catatan: banyak dari sumber ini menargetkan Mali generasi Midgard/Bifrost atau
> Linux/Android. GPU kita **Valhall (G57 MC2)**, jadi nomor/istilah register bisa
> berbeda — selalu divalidasi ke vendor kernel tree MTK dan ke silikon.

---

## 1. Catatan membedah Mali (utama)

- **fxlin/mali** — <https://github.com/fxlin/mali>
  Kumpulan catatan "everything we learnt hacking Arm Mali GPUs" (lisensi BSD-3,
  dokumentasi saja, bukan driver). Relevan:
  - **Register interface** dibagi 3 jenis: `GPU_CTRL`, `JOB_CTRL`, `MMU_MGMT`;
    tiap jenis bisa punya banyak instance (mis. 3 job slot, sampai 16 AS).
  - **Job slot ringbuffer**: tiap slot hanya menampung **2 job** — "current"
    (read-only bagi driver) dan "next" (boleh ditulis). Menguatkan desain
    `JS_CONFIG_NEXT` / `JS_COMMAND_NEXT` + doorbell di `MaliJob.c`.
  - **MMU / GPU VA**: `struct kbase_mmu_setup { transtab, memattr, transcfg }`,
    pgtable multi-level, `AS_TRANSTAB_BASE_MASK`. Cocok dengan `MaliMmu.c`.
  - Reset hardware: register map di `mali_kbase_gpu_regmap.h` (Mali DDK).

- **Heejin Park — mobileGPU** — <https://bakhi.github.io/mobileGPU/>
  Halaman riset yang lebih kaya (dasar dari fxlin/mali). Rujukan mendalam soal
  stack GPU Mali, TrustZone, dan sistem ringan.

## 2. Vulkan layer & tooling (jalur DX12 → Vulkan kita)

- **ARM-software/libGPULayers** — <https://github.com/ARM-software/libGPULayers>
  Framework untuk membuat **Vulkan layer driver** (intercept call aplikasi →
  driver). Berisi layer siap pakai: `GPU Support` (triage), `GPU Profile`,
  `GPU Timeline`; plus generator layer. Lisensi MIT. Relevan untuk:
  - Menyisipkan diagnostik/perf di antara app dan Vulkan ICD kita.
  - Debug jalur `DXVK/vkd3d-proton → Vulkan ICD (PanVK) → MaliKmd` tanpa ubah
    aplikasi.
  - Menjadi contoh struktur ICD/layer saat membangun ICD sendiri.
  Uji resmi di Linux/Android + Mali/Immortalis; Windows ARM64 = wilayah kita.

- **ARM-software/patrace** — <https://github.com/ARM-software/patrace>
  Record & replay GLES (dan Vulkan) untuk Mali.

- **Mesa Panfrost / PanVK** — <https://gitlab.freedesktop.org/mesa/mesa>
  Driver open-source Mali (kernel Panfrost + Vulkan PanVK). Kandidat utama yang
  akan di-port sebagai **Vulkan ICD** kita.
  - Yang dipakai proyek ini: **`feeadb5f` (26.3.0-devel)** + patch stack
    FourFectVK (`mesa/patches-v9/fourfectvk/`), karena v9 hanya ada di stack itu.
    Patch portabilitas Windows: `mesa/patches/0001..0008`.
  - **v9 tidak didukung oleh Mesa upstream** (`docs/drivers/panfrost.rst`
    mengosongkan kolom Vulkan untuk `G57`, `jm_archs = [6, 7]`, commit
    *"panvk: Don't pretend we can compile for v9"*); dukungannya datang dari
    stack FourFectVK, bukan dari Mesa itu sendiri.
  - CLC/LLVM tidak diperlukan karena `src/panfrost/libpan` + `src/poly/cl`
    di-pregenerate (`mesa/pregen/`, `-Dpanfrost-pregen=true`).
  - Panfrost **Gallium (OpenGL)** mendukung v9, tetapi **tidak terjangkau di
    Windows** (frontend WGL hanya `d3d12`/`zink`/`llvmpipe`/`softpipe`), jadi
    jalur GL tetap Zink di atas ICD Vulkan.

- **Mesa — daftar referensi driver** —
  <https://docs.mesa3d.org/drivers/panfrost.html> (versi terbit dari
  `docs/drivers/panfrost.rst` yang memuat tabel dukungan per-arch).

## 2.5. Runtime C++ / CRT-free di Windows ARM64

- **LLVM compiler-rt `chkstk.S` (aarch64)** —
  <https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/builtins/aarch64/chkstk.S>
  Implementasi kanonik `__chkstk` yang dipakai `mesa/runtime/mali_aarch64.s`
  (clang meng-emit-nya untuk frame/alloca > 4 KiB; MSVC menaruhnya di
  `vcruntime.lib`, yang tidak terpasang untuk ARM64 di mesin ini).
- **Microsoft — Windows ARM64 ABI (`__chkstk`)** —
  <https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions>
  Kontrak register `x15` (jumlah dalam satuan 16 byte), SP tidak berubah.
- **Microsoft — TLS di PE (`_tls_index`, `IMAGE_TLS_DIRECTORY`)** —
  <https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#tls-directory>
  Dipakai untuk menjelaskan mengapa mendefinisikan `_tls_index` cukup
  (`mali_win32os.cpp`).
- **microsoft/STL** — <https://github.com/microsoft/STL>
  Definisi resmi `_Mtx_internal_imp_t` (`stl/inc/mutex`) dan `_Raise_handler`
  (`stl/inc/exception`). Dipakai untuk menulis `_Mtx_*` dan slot
  `__imp_?_Raise_handler@std@@...` di `runtime/mali_vcrt.cpp` /
  `runtime/mali_cxxrt.cpp` terhadap ABI MSVC 14.50 (VS 18) yang terpasang.
- **ARM64 `setjmp`/`_JUMP_BUFFER` (UCRT `setjmp.h`)** — layout 24 × u64
  (Frame, Reserved, X19–X28, Fp, Lr, Sp, Fpcr, Fpsr, D8–D15) yang dipakai
  `_setjmpex`/`longjmp` di `runtime/mali_aarch64.s`.

## 3. Paper & arsitektur

- **GPUReplay: A 50-KB GPU Stack for Client ML** — Park & Lin, ASPLOS 2022.
  Stack GPU super tipis (50 KB) yang mengeksekusi GPU dari CPU. Sejalan dengan
  tujuan proyek: stack sekecil mungkin, tanpa WDDM.
- **Safe and Practical GPU Computation in TrustZone** — Park & Lin, EuroSys 2023.
- NoMali (model Mali ARM) — `jobslot.hh` berguna untuk semantik state job slot.
- Lima / T6xx ISA wiki — <https://limadriver.org/Hardware/> (Mali lama, masih
  berguna untuk konsep hw/sw).
- Hot Chips slides: T880 (MediaTek X20), G71/Bifrost — latar arsitektur.

## 3.5. Toolchain & build

- **LLVM / clang / lld** — <https://github.com/llvm/llvm-project>
  Toolchain yang sudah dipakai proyek ini: `clang --target=aarch64-pc-windows-msvc`,
  `clang-cl`, `lld-link` (tanpa perlu install MSVC). Juga relevan nanti untuk
  build Mesa/NIR/SPIR-V dan backend shader.
- **winflexbison (win_flex / win_bison)** — <https://github.com/lexxmark/winflexbison>
  Flex/Bison untuk Windows. Dipakai build GL Mesa (GLSL front-end) karena
  `-Dplatforms=windows` memilihnya; di-vendor ke `external/winflexbison`
  (git-ignored) oleh `mesa/tools/fetch-flexbison.ps1` (2.5.25), bukan instalasi
  host.

## 4. Vendor / resmi
- **Mali GPU kbase register map & driver** — `mali_kbase_gpu_regmap.h`,
  `mali_kbase_mmu_hw_direct.c` di Mali DDK (sumber register resmi).
- **Mali Userspace Binary Drivers** —
  <https://developer.arm.com/downloads/-/Mali%20GPUs%20User-Space%20Binary%20Drivers>
- Vendor kernel tree (lokal, read-only):
  `android_kernel_mediatek_mt6789\` — `gpufreq_mt6789.c` (clock/power),
  `clk-mt6789*.c` (clock tree), `mtk-scpsys-mt6789.c` (MTCMOS),
  `mt6789.dts` (node `mali@13000000`, `g3d_config@13fbf000`).

## 5. Display: DRM/KMS, MMSYS, dan jalur WDDM

Dipakai untuk [`DISPLAY-ANDROID.md`](DISPLAY-ANDROID.md) dan
[`WDDM-PATH.md`](WDDM-PATH.md).

- **Linux DRM/KMS — overview** — <https://docs.kernel.org/gpu/drm-kms.html>
  Model CRTC/plane/connector/encoder + atomic commit. Ini kontrak yang dipakai
  Android, dan yang harus "diterjemahkan" ke VidPn WDDM.
- **MediaTek DRM driver (upstream)** —
  <https://elixir.bootlin.com/linux/latest/source/drivers/gpu/drm/mediatek>
  Padanan upstream dari tree vendor lokal. Berguna untuk melihat komponen MMSYS
  yang lebih baru (`mtk_disp_ccorr`, `mtk_disp_gamma`, `mtk_disp_dither`,
  `mtk_disp_merge`, `mtk_mutex`, `mtk_drm_ddp_comp`) yang tidak ada di tree
  vendor MT6789.
- **dma-buf — sharing buffer antar device** — <https://docs.kernel.org/driver-api/dma-buf.html>
  Mekanisme zero-copy GPU↔display di Android. Tidak ada padanan langsung di
  Windows; konsepnya dipetakan ke allocation KMD di `DISPLAY-ANDROID.md` §3.
- **AOSP graphics stack (SurfaceFlinger / HWC / gralloc)** —
  <https://source.android.com/docs/core/graphics>
  Komposisi terpisah dari scan-out: SurfaceFlinger memilih GPU atau overlay HWC.
- **WDDM — display driver model** —
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/>
  VidPn/VidMm/VidSch. Kontrak pengganti DRM/KMS di Windows.
- **Kernel Mode Display-Only Driver (KMDOD) sample** —
  <https://github.com/microsoft/Windows-driver-samples/tree/main/video/KernelModeDisplayOnly>
  Kerangka WDDM render-only/display-only (tanpa 3D engine). Basis yang paling
  masuk akal untuk L3 (Mali muncul di Task Manager).
- **D3D12 UMD (D3D12DDI)** —
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/d3d12-umd>
  Tabel fungsi yang harus diisi kalau suatu hari menulis UMD D3D12 native.
- **D3D11 UMD** —
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/d3d11-umd>
  Yang dibutuhkan DWM untuk mengomposisi desktop di Mali (L4).
- **Indirect Display Driver (IddCx)** —
  <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview>
  Bukan solusi kita (IddCx tidak menggerakkan hardware), tapi berguna sebagai
  pembanding saat memilih antara "display miniport asli" dan jalur lain.

---

## Ringkasan "mengapa ini penting"

| Sumber | Dipakai untuk |
| --- | --- |
| fxlin/mali | Semantik job slot / register / MMU saat menulis `MaliJob.c`, `MaliMmu.c` |
| libGPULayers | Tooling & contoh struktur Vulkan layer/ICD di jalur DX12 kita |
| bakhi.github.io/mobileGPU | Detail stack Mali & pendekatan minimalis |
| PanVK / Panfrost | Basis ICD Vulkan yang akan di-port |
| GPUReplay | Bukti bahwa stack GPU tipis itu layak |
| Mali DDK regmap | Definisi register resmi (validasi offset) |
| Vendor kernel tree | Angka power/clock/MTCMOS/register yang pasti untuk MT6789 |
| DRM/KMS + dma-buf + MediaTek DRM | Model buffer bersama dan pipeline display yang mau dipetakan ke WDDM |
| KMDOD / D3D11 UMD / D3D12 UMD | Kerangka kerja saat menulis driver Windows (L3/L4) |
| microsoft/STL + UCRT `setjmp.h` | Menulis `_Mtx_*` dan `_setjmpex`/`longjmp` sesuai ABI ARM64/MSVC |
| winflexbison | Prasyarat build GL Mesa, di-vendor bukan di-install |
