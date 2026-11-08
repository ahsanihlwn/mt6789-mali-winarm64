# MT6789 (Helio G99) — Hardware inventory for the Windows driver work

Source of truth: `android_kernel_mediatek_mt6789` @ `f0a85c3c19d808b86dbad9070a267a30709f85d0`
(Linux **5.10.101**, branch `alps-mp-s0.mp1`). Everything below is read from the
device tree and the vendor driver sources in this repo, not guessed.

---

## CPU

| Core | Nodes | MPIDR | `compatible` | Notes |
|------|-------|-------|--------------|-------|
| Cortex-A55 x6 | `cpu0..cpu5` (`cpu@000..005`) | `0x000..0x005` | `arm,cortex-a55` | little cluster |
| Cortex-A76 x2 | `cpu6..cpu7` (`cpu@100..101`) | `0x100, 0x101` | `arm,cortex-a76` | big cluster |

- `enable-method = "psci"`, PSCI 1.0 over SMC (from the firmware/ACPI audit).
- Architected timer `CNTFRQ = 13 MHz`.
- GICv3: distributor `0x0C000000`, redistributor `0x0C040000` (len `0x200000`).
- **Windows consequence:** ARM64 Windows already has a generic CPU HAL. There is
  **no custom CPU "driver" to write** — the CPU is enabled by correct
  **ACPI** (`MADT`, `GTDT`, `FADT`, plus `PPTT` for topology) and PSCI. See
  `Docs/Porting/P13001L-Windows-ACPI-audit.md` in `edk2-UEFI-mtk`.

## GPU  —  Arm Mali-G57 MC2 (Valhall)

| Item | Value |
|------|-------|
| DT node | `mali: mali@13000000` |
| `compatible` | `"mediatek,mali", "arm,mali-valhall"` |
| Register block | `0x13000000`, size `0x4000` |
| MFG / G3D config | `0x13FBF000` (`mfg_top_config`, syscon, size `0x1000`) |
| IRQs (GIC SPI, level-high) | `396` GPU, `397` MMU, `398` JOB, `399` EVENT, `400` PWR |
| Clocks | `MFG_PLL_SEL`, `MFG_REF_SEL`, `MFGPLL`, `MFG_REF`, `MFGCFG_BG3D` |
| Power domains | `MT6789_POWER_DOMAIN_MFG0..MFG3` (scpsys) |
| Regulators | `mt6358_vgpu`, `mt6358_vsram_gpu` |
| OPP (top) | 1.10 GHz @ 850 mV (DVFS down to ~0.3x GHz) |
| Vendor kernel driver | Arm **kbase DDK r32p1** — `drivers/gpu/mediatek/gpu_mali/mali_valhall/mali-r32p1/drivers/gpu/arm/midgard/` (also r25/r30/r32p0 in-tree) |
| MTK glue | `ged` (GPU energy/DVFS/vsync), `gpufreq` v2, `gpueb` |

Slower clocks in `gpufreq` map (MFG is clocked off `MFGPLL`; the SPM/`scpsys`
block gates `MFG0-3`). SMMU/IOMMU for GPU virtual memory lives in the
MediaTek SMMU (separate from the GPU register block).

### Job Manager chain rules (measured on silicon, tNAx r0p1)

| Rule | Evidence |
|------|----------|
| Job descriptor header = 32 bytes; payload follows at +32 | `WRITE_VALUE` payload at +32 honoured |
| `job_descriptor_size` bit 0 = 1 for a job that carries a payload | write-value job writes memory |
| Chain terminates with `next_job == 0` | 1-job chain completes |
| **Every descriptor in a chain must be 128-byte aligned** | stride 128/256 complete; 64/96/192/320 give `JS_STATUS 0x58` (`DATA_INVALID_FAULT`) |
| `job_index` is 1-based per chain | `pan_jc_add_job` uses `++jc->job_index` |
| `JS_CONFIG` flush bits do not affect chaining | 0x8000 / 0x8300 / 0x8800 / 0x8301 all behave the same |

The 128-byte alignment rule is the non-obvious one: a 64-byte stride places the
next header inside the previous descriptor's footprint and the JM rejects the
whole chain before running any job (`HEAD == TAIL`).

## Display

- MediaTek MDP / OVL / DSI with `clk-mt6789-mmsys`, `clk-mt6789-disp_dsc`.
- UEFI side inherits a **simple-framebuffer** (LK OVL layer) — already proven to
  produce GOP output on the tablet (see the P13001L docs).

## Peripherals that matter for bring-up

- USB: **MediaTek MUSB USB2** @ `0x11200000` (PHY `0x11F40000`) — **not xHCI**.
- Storage: UFS (UEFI BlockIO already works; Windows still needs its own ACPI +
  storage driver).

## What this means in one line

CPU = ACPI problem (no new driver). GPU = the real work, because **no Mali WDDM
driver exists for Windows anywhere** (see `ROADMAP.md`).
