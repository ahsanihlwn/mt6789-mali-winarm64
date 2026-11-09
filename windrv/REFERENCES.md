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
  - Yang dipakai proyek ini: tag **`mesa-25.0.7`** (PanVK-nya belum butuh
    CLC/LLVM). Patch portabilitas ada di `mesa/patches/`.
  - **Batas penting — v9 tidak didukung:** `docs/drivers/panfrost.rst` (25.0.7)
    mengosongkan kolom **Vulkan** untuk `G57`/`Valhall (v9)`;
    `src/panfrost/vulkan/meson.build` hanya punya `jm_archs = [6, 7]`. Commit
    Mesa yang menyatakannya: *"panvk: Don't pretend we can compile for v9"*.
  - Panfrost **Gallium (OpenGL)** *memang* mendukung v9 — jalur render GPU yang
    realistis untuk G57 saat ini.

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

## 4. Vendor / resmi
- **Mali GPU kbase register map & driver** — `mali_kbase_gpu_regmap.h`,
  `mali_kbase_mmu_hw_direct.c` di Mali DDK (sumber register resmi).
- **Mali Userspace Binary Drivers** —
  <https://developer.arm.com/downloads/-/Mali%20GPUs%20User-Space%20Binary%20Drivers>
- Vendor kernel tree (lokal, read-only):
  `android_kernel_mediatek_mt6789\` — `gpufreq_mt6789.c` (clock/power),
  `clk-mt6789*.c` (clock tree), `mtk-scpsys-mt6789.c` (MTCMOS),
  `mt6789.dts` (node `mali@13000000`, `g3d_config@13fbf000`).

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
