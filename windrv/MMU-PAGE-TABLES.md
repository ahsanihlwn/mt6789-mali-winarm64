# GPU MMU page tables — bring-up record (MT6789 / Mali-G57, AArch64 4K)

Status: **works on silicon** (itel VistaTab 30 Pro, P13001L, `ROOT\SYSTEM\0002`,
driver `MaliKmdPt` built with `MALI_MMU_USE_PAGE_TABLES=1`).

This document records how the AArch64 4K page-table path was debugged, because
two independent bugs had to be fixed and one of them is easy to reintroduce.

---

## 1. Symptom

With `MALI_MMU_USE_PAGE_TABLES=1` the driver programmed
`AS_TRANSCFG = ADRMODE_AARCH64_4K` and a valid-looking 4-level tree, and the
first job submitted on slot 1 died immediately:

```
JS_STATUS        0x00000008        (ACTIVE -> faulted)
AS_FAULTSTATUS   0x7C0002C0
AS_FAULTADDR     0x000000023FD89000   (= the job's GPU VA)
chain cleared    0/4
```

`AS_FAULTSTATUS` decode (vendor regmap + `kbase_mmu_fault_code` tables):

| field | bits | value | meaning |
|-------|------|-------|---------|
| exception type | [7:0] | `0xC0` | `TRANSLATION_FAULT_0` — the **level-0** entry was rejected |
| access type | [9:8] | `2` | READ (the descriptor fetch) |
| source id | [15:10] | `0x1F` | unknown/master |

The same BO and the same job chain completed in 0 ms in `IDENTITY` mode, and
`AS_TRANSTAB`, `AS_MEMATTR` and `AS_TRANSCFG` all read back exactly what was
written. So the address-space registers were being accepted — the walker just
never accepted the tree.

## 2. Bug 1 — table descriptor type

`mali_kbase_mmu_mode_aarch64.c`:

```c
#define ENTRY_IS_ATE_L02  1ULL   /* block (huge page), levels 1..2 ONLY */
#define ENTRY_IS_INVAL    2ULL
#define ENTRY_IS_ATE_L3   3ULL   /* bottom-level 4K page             */
#define ENTRY_IS_PTE      3ULL   /* next-level table pointer, L0..L2 */
```

A pointer to the next level table and a bottom-level 4K page share the value
**3**. Value **1** is a block descriptor, which AArch64 permits only at levels
1 and 2 — never at level 0.

`MaliMmu.c` stored `1` for the L0→L1 pointer. The walker therefore saw a huge
page block descriptor where architecture requires a table pointer and faulted
on the very first fetch: `TRANSLATION_FAULT_0`.

Fix: `PTE_TABLE` is `3`.

> The vendor tree also contains `ate_is_valid()`, which checks `ENTRY_IS_ATE_L02`
> (1) for levels 0..2. That is the *validator* for a complete ATE, not the
> encoding used when writing a next-level pointer — reading that function and
> assuming `1` is the table type is what caused this bug.

## 3. Bug 2 — PTW shareability

After bug 1 the fault did **not** go away. Bisecting the walk from user mode
(`malitest --ptbo`) showed that the walk failed at level 0 *no matter what the
table contained*:

- a sparse, correct tree → `0xC0`
- a tree with **all 512 entries valid at every level** (`--spray`) → `0xC0`
- a tree with only L0 populated → **still `0xC0`** (it should have descended and
  faulted at level 1)

If every index at level 0 is a valid table pointer and the fault is still at
level 0, the walker is not reading our table at all. The remaining variable was
`AS_TRANSCFG`'s PTW attributes:

| `AS_TRANSCFG` | result |
|---|---|
| `0x62000006` — `PTW_SH_OS` (outer shareable) | fault `0xC0` |
| `0x32000006` — `PTW_SH_IS` (inner shareable) | fault `0xC0` |
| `0x42000006` — `PTW_SH = 0`, WB, read-allocate | **walk succeeds** |
| `0x02000006` — `PTW_SH = 0`, WB only | **walk succeeds** |
| `0x00000006` — bare mode 6 | walk starts, faults lower down |

The driver was setting `AS_TRANSCFG_PTW_SH_OS` unconditionally. kbase only sets
it when the GPU is coherent:

```c
/* mmu/mali_kbase_mmu_hw_direct.c, kbase_mmu_hw_configure() */
if (kbdev->system_coherency != COHERENCY_NONE) {
        transcfg = (transcfg & ~AS_TRANSCFG_PTW_SH_MASK)
                 | AS_TRANSCFG_PTW_SH_OS;
}
```

MT6789 is a non-coherent Mali (MTK powers the GPU with its own coherency
setup), so the correct value is `PTW_SH = 0`. With shareability set, the
page-table walker's read is issued into a shareability domain the interconnect
does not service for the GPU, and the descriptor never arrives.

Fix: `MALI_MMU_PTW_SH_OS` in `include/mali_mmu.h`, default `0`, with the
`AS_TRANSCFG` term compiled conditionally.

Final value used by the driver:

```
AS_TRANSCFG_ADRMODE_AARCH64_4K         0x006
AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK     0x2000000
AS_TRANSCFG_R_ALLOCATE                 0x40000000
                                       ----------
                                       0x42000006
```

## 4. Evidence on hardware

Driver built with `-DMALI_MMU_USE_PAGE_TABLES=1`, `--rawjob --chain 2 --wv`
(no tool-side override of any address-space register):

```
MMU enabled (UsePageTables=1, TranscfgLo=0x0x42000006)
rawjob BO #3 GpuVa 0x000000023FD89000
rawjob: waited 0 ms  RAWSTAT 0x00000001  JS_STATUS 0x00000000
  AS_FAULTSTATUS 0x00000000   AS_FAULTADDR 0x00000000
  TRANSTAB 0x00000002:0x3FD8A000
  chain cleared 2/2
```

Repeated three times after a full `--off`/`--on` cycle: identical.

The driver's own submit path (`IOCTL_MALI_JOB_SUBMIT`) also reports
`job done: JS_STATUS 0x00000000  IRQ 0x00000001`.

## 5. How the walk was bisected from user mode

`windrv/kmd/tools/malitest.c` grew the experiments that produced the evidence
above; they need no driver rebuild:

| command | what it does |
|---|---|
| `--ptdump [pa] [idx] [cnt]` | read raw 64-bit entries out of a tracked page-table page (default L0) |
| `--ptwalk <va>` | follow the tree from L0 and print every entry on the way to the leaf |
| `--ptbo [--spray] [--lvl0] [--tc HEX]` | build the whole 4-level tree inside a **BO** (memory the GPU demonstrably reads), program `AS_TRANSTAB` to it, `AS_COMMAND_UPDATE`, submit a one-job chain and report `JS_STATUS`, `AS_FAULTSTATUS`, `AS_FAULTADDR` and a sentinel |
| `--lockaddr/--oplock/--opflushpt/--opflushm/--opunlock/--opupdate` | raw `AS_COMMAND` / `AS_LOCKADDR` sequencing |
| `--rawjob --adrmode <n> [--ptwsh]` | override `AS_TRANSCFG` from the tool (`--ptwsh` re-adds `PTW_SH_OS`) |

`--lvl0` (only the top level valid) and `--spray` (every index valid) are the
two experiments that proved the walker was not touching the table: both must
fault one level lower than a plain tree, and both did not.

Using BO memory as the page table removed the last allocation-level difference:
`--ptbo` used the same `MmAllocateContiguousMemorySpecifyCache(..., MmNonCached)`
pages that the GPU reads successfully in `IDENTITY` mode.

### Pitfall: fault state is sticky

`AS_FAULTADDR`, `AS_FAULTSTATUS`, `MMU_IRQ_RAWSTAT` and the job slot keep their
state across runs. Consecutive experiments without a reset produce meaningless
output (`JS_STATUS 0x58`, `SH_PWRACTIVE 0`, chain sentinel untouched). Every
experiment must be preceded by a full power cycle:

```
malitest.exe --off --on --cores
```

`--ghard` (GPU soft reset) does **not** reliably leave the cores powered; use
`--off --on`. Also poll on the *sentinel*, not on `RAWSTAT`, or a stale
interrupt bit ends the wait loop at 0 ms.

## 6. What is still open

- `--rawjob --chain 4` reports `chain cleared 2/4` with `JS_STATUS 0x58`
  (`DATA_INVALID_FAULT`) while `--chain 2` is 100 % reproducible. It also
  happened once in `IDENTITY`, so it is a chain-building issue in the test
  tool, not an MMU issue (no MMU fault is reported). To be isolated separately.
- Page-table pages and BOs are all non-cached today (attribute index 5 =
  `0x4C`). Per-page cache attributes (write-back for BOs, non-cached for page
  tables + cache maintenance) are needed before any performance work.
- Only AS0 is used; multi-AS and non-contiguous allocations are Phase 2e.
- `--ptdump <nonzero-pa>` still returns `ERROR_NOT_FOUND` (`0x490`) while
  `--ptwalk`'s per-entry dump of the same page works; `MaliMmuPtDump` should
  fall back to `MaliPhysToVa()` for pages that are not in `PtPages[]`.