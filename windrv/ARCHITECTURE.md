# Arsitektur & Rencana — MT6789 (Mali-G57) di Windows 11 ARM64

Dokumen ini merangkum **arsitektur** dan **rencana** proyek. Detail hardware ada di
`HARDWARE-MT6789.md`; checklist fase ada di `ROADMAP.md`.

> Ringkasnya: display **sudah beres** (GOP / Microsoft Basic Display Adapter).
> Yang kita bangun hanya **driver akselerator** (broker hardware Mali) + **Vulkan
> ICD**. Tidak ada driver display yang ditulis.

---

## 1. Yang sudah jalan vs yang belum

| Bagian | Status | Keterangan |
|--------|--------|------------|
| Boot Windows 11 ARM64 | ✅ | Sudah boot, gambar tampil |
| Display | ✅ | GOP / Basic Display Adapter, native res |
| CPU / timer / IRQ GIC | ✅ | HAL generic + ACPI (di `edk2-UEFI-mtk`) |
| **Akselerasi 3D (Mali)** | ❌ | **Ini yang dikerjakan** |

Jadi tidak ada pekerjaan display/CPU yang tersisa untuk target ini.

---

## 2. Layer dan siapa pemiliknya

```
  USER MODE
  ┌───────────────────────────────────────────────────────────────┐
  │  App D3D12 ──► d3d12.dll  (vkd3d-proton)        [reuse]       │
  │  App D3D11/10/9 ──► d3d11.dll / dxgi.dll (DXVK) [reuse]       │
  │                     │                                          │
  │                     ▼                                          │
  │            Vulkan loader (vulkan-1.dll)          [OS]         │
  │                     │                                          │
  │            ★ ICD Vulkan Mali                     [KITA]       │
  │              · shim panfrost/panthor                         │
  │              · WSI Win32 (swapchain/present)                  │
  └─────────────────────┼─────────────────────────────────────────┘
                        │ DeviceIoControl (ABI di mali_kmd_ioctl.h)
  KERNEL MODE           │
  ┌─────────────────────▼─────────────────────────────────────────┐
  │            ★ MaliKmd.sys  (KMDF, ARM64)          [KITA]       │
  │              · MMIO Mali 0x13000000                           │
  │              · power MFG0..3 + clock mux (SPM/APMIXED)        │
  │              · GPU MMU (AArch64 4K, page table)               │
  │              · buffer object (alokasi + map ke user)          │
  │              · (berikutnya) IRQ + job submission              │
  └─────────────────────┬─────────────────────────────────────────┘
                        ▼
                  Mali-G57 (hardware)
```

Yang **bertanda ★** hanya dua, dan keduanya kita tulis. Sisanya dipakai ulang.

---

## 3. Kenapa harus ada KMD (kernel-mode)

ICD Vulkan jalan di user mode, dan user mode di Windows **tidak bisa**:
- baca/tulis MMIO register Mali,
- program GPU MMU (`AS_TRANSTAB`, page table),
- nyalakan domain power / ganti clock mux,
- pasang IRQ GPU.

Tidak ada `/dev/mem` di Windows. Jadi KMD ini adalah **broker hardware** — perannya
sama seperti `panfrost`/`panthor` di Linux, hanya antarmukanya `DeviceIoControl`
bukan DRM ioctl.

**Penting:** KMD ini **bukan** driver display dan **bukan** WDDM. Dia tidak
menyentuh scanout, resolusi, atau komposisi desktop.

---

## 4. Jalur DirectX 12 tanpa WDDM

Kunci yang membuat rencana ini masuk akal: **vkd3d-proton dan DXVK adalah DLL
Win32 yang bicara langsung ke Vulkan**, bukan ke WDDM.

```
d3d12.dll (vkd3d-proton) ─► Vulkan ─► ICD Mali ─► KMD ─► GPU
```

WDDM tidak ada di jalur itu. WDDM hanya perlu kalau **DWM/desktop** mau ikut pakai
Mali (Phase 5, opsional).

**Konsekuensi yang harus jujur diterima:**
1. Yang dapat akselerasi = aplikasi yang memakai DLL override
   (`d3d12.dll`/`d3d11.dll`/`dxgi.dll` disimpan di folder app). Desktop/DWM tetap
   di BDA.
2. **ICD Vulkan adalah jalur kritis.** KMD sendirian tidak membuat apa pun
   terlihat di aplikasi.
3. Presentasi lewat `VK_KHR_win32_surface`; WSI Mesa di Windows perlu diuji, bukan
   otomatis jalan.

---

## 5. Desain internal KMD

### 5.1 Data-driven per-SoC
Seluruh offset register, clock, dan power ada di tabel deskriptor
(`src/MaliSoc.c`). Logika (`MaliHw.c`) tidak menyebut nama SoC sama sekali.
Menambah SoC = menambah satu entri:

```
MT6789  ← lengkap, terverifikasi dari vendor tree
MT6833 / MT6853 / MT6879 / MT6895 ← stub; power ditolak sampai SPM-nya diisi
```

### 5.2 Power aman
- Saat driver load: **hanya baca** register (tidak ada tulis).
- Tulis power/clock hanya saat IOCTL eksplisit.
- Kalau `MfgDomainCount == 0`, driver **menolak** power-on (tidak menembak register
  tak dikenal).

### 5.3 Memori GPU (buffer object)
- Alokasi **fisik contiguous**, non-cached, zeroed.
- Di-identity-map ke page table GPU saat dibuat.
- Dipetakan ke proses pemanggil lewat MDL; ada guard `OwnerPid`.
- Handle table + free otomatis (unmap MMU + user + MDL + memori).

### 5.4 MMU GPU
- **AArch64 4K**, 4 level tabel (top 0 → bottom 3), shift 39/30/21/12.
- **Identity**: GPU VA == PA (bring-up). Non-cached, jadi tanpa cache maintenance.
- AS0 base `0x2400`; enable via `AS_TRANSCFG` + `AS_COMMAND_UPDATE`.
- `AS_TRANSCFG` untuk AArch64 4K = `0x42000006`:
  `ADRMODE_AARCH64_4K(6) | PTW_MEMATTR_WRITE_BACK | R_ALLOCATE`.
  **`PTW_SH` (bit 29:28) harus 0** — GPU ini non-coherent; kalau di-set,
  page-table walker gagal di level 0 dan fault `0x7C0002C0`. Lihat
  `MMU-PAGE-TABLES.md`.
- PTE next-level table pakai tipe **3** (`ENTRY_IS_PTE`), bukan 1 (block).
- Path tabel sudah terbukti di silicon (`-UsePageTables`, `chain cleared 2/2`).
- Pembuatan tabel race-safe (double-check + spinlock; alokasi tetap di PASSIVE).
- Fault status + address bisa dibaca lewat IOCTL.

### 5.5 ABI KMD ↔ ICD
`include/mali_kmd_ioctl.h` (dipakai kernel & user mode). Sudah ada:

| IOCTL | Fungsi |
|-------|--------|
| `GET_VERSION` / `GET_CAPS` | identifikasi, GPU_ID, fitur, flags |
| `POWER` / `GET_PWR_STATUS` | nyala/mati domain + status |
| `REG_READ` / `REG_WRITE` | diagnostik bring-up |
| `ALLOC_BO` / `FREE_BO` / `BO_INFO` | memori GPU + map ke user |
| `MMU_ENABLE` / `MMU_DISABLE` / `MMU_FAULT` | kontrol MMU + fault |
| `JOB_SUBMIT` | kirim job chain (GPU VA dari ICD), opsional tunggu selesai |
| `JOB_STATUS` / `JOB_STOP` | baca JS_STATUS/HEAD/TAIL, soft/hard stop |
| `JOB_SELFTEST` | uji jalur penuh dengan chain kosong (null job) |
| `IRQ_STATS` | hitungan per-line + fault GPU/MMU terakhir |

ABI sekarang **v2**. Belum ada (dibutuhkan ICD): fence/timeline yang
di-share, tiler heap, sync object.

Catatan penting: KMD **tidak** membangun job chain. ICD (PanVK) yang menulis
chain ke BO dan memberi KMD GPU VA-nya; KMD hanya memprogram `JS_HEAD_NEXT`,
`JS_AFFINITY_NEXT`, `JS_CONFIG_NEXT`, lalu menulis `JS_COMMAND_NEXT = START`.

---

## 6. Rencana fase

| Fase | Isi | Status |
|------|-----|--------|
| **0** | Inventaris hardware, build ARM64, loop uji | ✅ sebagian (build WDK belum terpasang) |
| **1** | Boot Windows 11 ARM64 + display BDA | ✅ |
| **2** | **Mali KMD** (akselerator/broker) | 🔄 berjalan |
| 2a | Map MMIO + baca ID | ✅ |
| 2b | Power MFG0..3 + clock mux | ✅ (validasi HW) |
| 2c | BO alloc + map user | ✅ |
| 2d | GPU MMU (AArch64 4K identity) | ✅ |
| 2e | IRQ GPU/MMU/JOB (+EVENT/PWR nanti) | ✅ kode; HW belum divalidasi |
| 2f | Job submission + fence (null job dulu) | ✅ kode; HW belum divalidasi |
| 2g | Publikasi node Mali di ACPI (biar PnP bind tanpa ROOT) | ⬜ (`edk2-UEFI-mtk`) |
| **3** | **ICD Vulkan** (shim PanVK di atas IOCTL kita) | ⬜ |
| **4** | D3D translation (vkd3d-proton + DXVK) | ⬜ |
| **5** | (opsional) WDDM render driver untuk desktop | ⬜ |

**Fase 2 adalah prasyarat teknis; Fase 3 adalah jalur kritis.** Kalau mau hasil
terlihat cepat, Fase 3 boleh dimulai paralel begitu 2e/2f punya kerangka.

**Stopgap:** WARP (D3D12 software) sudah ada di Windows. Dengan display yang jalan,
app D3D12 bisa jalan sekarang juga (lambat, tapi benar). Ini bukan akselerasi Mali.

---

## 7. Target OS: Windows 11 ARM64 full (bukan WinPE)

Konsekuensi yang harus dipenuhi:

1. **Signing.** Untuk dev: matikan Secure Boot + `bcdedit /set testsigning on`,
   lalu pasang driver self-signed. Untuk distribusi: attestation/WHQL signing
   (ARM64 wajib driver bertanda tangan Microsoft).
2. **Install.** `pnputil /add-driver MaliKmd.inf /install` (bukan `devcon`, walau
   `devcon` masih bisa untuk bring-up).
3. **Driver store.** KMD dipasang permanen, tidak perlu load manual tiap boot.
4. **DXVK/vkd3d butuh lingkungan proses normal** (bukan PE): file system lengkap,
   Vulkan loader, `dxgi.dll` override per-folder.

---

## 8. Risiko utama

| Risiko | Dampak | Mitigasi |
|--------|--------|----------|
| Port PanVK lepas dari DRM | Fase 3 paling besar | Kerjakan bertahap; mulai dari device init + buffer |
| MMU identity belum terbukti di hardware | GPU fault | Sudah ada `MMU_FAULT`; fallback page-table mapping VA→PA eksplisit |
| SPM bit MTCMOS bisa beda di silicon | power gagal | `GET_PWR_STATUS` + tolak kalau mask tidak cocok |
| IRQ belum tersedia sebelum node ACPI | job cuma bisa polling | `MaliIrqInitialize` toleran 0 line; `JOB_SUBMIT` fallback poll `JOB_IRQ_RAWSTAT` |
| Format job chain (header) tidak ada di kbase | `JOB_SELFTEST` bisa fault | Chain dibangun ICD; selftest pakai header nol = chain kosong, `JS_STATUS` jadi diagnosa |
| Core power (PRESENT→PWRON→READY) belum diuji | `JS_STATUS 0x41` POWER_FAULT | `MaliPowerCores` data-driven, urutan L2→tiler→shader; gagal = `STATUS_IO_TIMEOUT` |
| WSI Win32 di Mesa | present tidak jalan | Uji terpisah dari render |
| Biaya debug fisik | iterasi lambat | UART console + WinPE boot loop tetap dipakai |

---

## 9. Yang dipakai ulang vs ditulis

**Reuse:** vkd3d-proton, DXVK, Mesa PanVK/panfrost (pengetahuan register + user
space), Arm kbase r32p1 & MTK ged/gpufreq (referensi clock/power KMD).

**Ditulis:** Mali KMD Windows (baru), backend Windows untuk PanVK (baru), WDDM
display miniport (baru, opsional, Fase 5).
