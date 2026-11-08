/*
 * mali_job.h  -  Job Manager (JM) submission + interrupt register map.
 *
 * Mali-G57 on MT6789 uses the classic job-manager front end (JM), not the
 * CSF/MCU firmware path, so job submission is "write a pointer, ring the
 * doorbell, read JS_STATUS".
 *
 * All offsets are byte offsets inside the GPU register block (MTK_REGION_GPU,
 * 0x13000000). Values are taken verbatim from the vendor kbase r32p1 tree:
 *   include/uapi/.../gpu/mali_kbase_gpu_regmap.h
 *   include/uapi/.../gpu/backend/mali_kbase_gpu_regmap_jm.h
 *   backend/gpu/mali_kbase_jm_hw.c      (kbase_job_hw_submit)
 *   mali_kbase_dummy_job_wa.c           (minimal submit sequence)
 *
 * NOTE on naming: the hardware MASK registers use 1 = *enabled*, the opposite
 * of the usual Linux convention. Confirmed against kbase code that writes 0 to
 * "mask off" and GPU_IRQ_REG_ALL to "unmask".
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <ntddk.h>

/* ---------------- GPU control (base 0x0000) ---------------- */
#define MALI_GPU_ID                  0x000
#define MALI_L2_FEATURES             0x004
#define MALI_CORE_FEATURES           0x008
#define MALI_TILER_FEATURES          0x00C
#define MALI_MEM_FEATURES            0x010
#define MALI_MMU_FEATURES            0x014
#define MALI_JS_PRESENT              0x01C
#define MALI_GPU_IRQ_RAWSTAT         0x020
#define MALI_GPU_IRQ_CLEAR           0x024
#define MALI_GPU_IRQ_MASK            0x028
#define MALI_GPU_IRQ_STATUS          0x02C
#define MALI_GPU_COMMAND             0x030
#define MALI_GPU_FAULTSTATUS         0x03C
#define MALI_GPU_FAULTADDRESS_LO     0x040
#define MALI_GPU_FAULTADDRESS_HI     0x044
#define MALI_JS0_FEATURES            0x0C0
#define MALI_JS_FEATURES(n)          (MALI_JS0_FEATURES + ((n) << 2))

/* GPU_IRQ bits. */
#define MALI_GPU_FAULT               (1u << 0)
#define MALI_MULTIPLE_GPU_FAULTS     (1u << 7)
#define MALI_RESET_COMPLETED         (1u << 8)
#define MALI_POWER_CHANGED_SINGLE    (1u << 9)
#define MALI_POWER_CHANGED_ALL       (1u << 10)
#define MALI_PRFCNT_SAMPLE_COMPLETED (1u << 16)
#define MALI_CLEAN_CACHES_COMPLETED  (1u << 17)

/* GPU_COMMAND values. */
#define MALI_GPU_COMMAND_NOP               0x00
#define MALI_GPU_COMMAND_SOFT_RESET        0x01
#define MALI_GPU_COMMAND_HARD_RESET        0x02
#define MALI_GPU_COMMAND_PRFCNT_CLEAR      0x03
#define MALI_GPU_COMMAND_PRFCNT_SAMPLE     0x04
#define MALI_GPU_COMMAND_CYCLE_COUNT_START 0x05
#define MALI_GPU_COMMAND_CYCLE_COUNT_STOP  0x06
#define MALI_GPU_COMMAND_CLEAN_CACHES      0x07
#define MALI_GPU_COMMAND_CLEAN_INV_CACHES  0x08

/* ---------------- Job control (base 0x1000) ---------------- */
#define MALI_JOB_IRQ_RAWSTAT         0x1000
#define MALI_JOB_IRQ_CLEAR           0x1004
#define MALI_JOB_IRQ_MASK            0x1008
#define MALI_JOB_IRQ_STATUS          0x100C
#define MALI_JOB_IRQ_JS_STATE        0x1010
#define MALI_JOB_IRQ_THROTTLE        0x1014

#define MALI_JOB_SLOT_BASE           0x1800   /* JOB_CONTROL_BASE + JOB_SLOT0 */
#define MALI_JOB_SLOT_STRIDE         0x80
#define MALI_JOB_SLOT_MAX            16

/* Per-slot registers, relative to the slot base. */
#define MALI_JS_HEAD_LO              0x00
#define MALI_JS_HEAD_HI              0x04
#define MALI_JS_TAIL_LO              0x08
#define MALI_JS_TAIL_HI              0x0C
#define MALI_JS_AFFINITY_LO          0x10
#define MALI_JS_AFFINITY_HI          0x14
#define MALI_JS_CONFIG               0x18
#define MALI_JS_XAFFINITY            0x1C
#define MALI_JS_COMMAND              0x20
#define MALI_JS_STATUS               0x24
#define MALI_JS_HEAD_NEXT_LO         0x40
#define MALI_JS_HEAD_NEXT_HI         0x44
#define MALI_JS_AFFINITY_NEXT_LO     0x50
#define MALI_JS_AFFINITY_NEXT_HI     0x54
#define MALI_JS_CONFIG_NEXT          0x58
#define MALI_JS_XAFFINITY_NEXT       0x5C
#define MALI_JS_COMMAND_NEXT         0x60
#define MALI_JS_FLUSH_ID_NEXT        0x70

/* JOB_IRQ bits: one "done" and one "error" bit per slot. */
#define MALI_JOB_IRQ_SLOT_DONE(n)    (1u << (n))
#define MALI_JOB_IRQ_SLOT_ERROR(n)   (1u << (16 + (n)))

/* JS_COMMAND values. */
#define MALI_JS_COMMAND_NOP          0x00
#define MALI_JS_COMMAND_START        0x01
#define MALI_JS_COMMAND_SOFT_STOP    0x02
#define MALI_JS_COMMAND_HARD_STOP    0x03

/* JS_CONFIG / JS_CONFIG_NEXT bits. */
#define MALI_JS_CONFIG_START_FLUSH_NO_ACTION        (0u << 0)
#define MALI_JS_CONFIG_START_FLUSH_CLEAN            (1u << 8)
#define MALI_JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define MALI_JS_CONFIG_START_MMU                    (1u << 10)
#define MALI_JS_CONFIG_JOB_CHAIN_FLAG               (1u << 11)
#define MALI_JS_CONFIG_END_FLUSH_NO_ACTION          (0u << 12)
#define MALI_JS_CONFIG_END_FLUSH_CLEAN              (1u << 12)
#define MALI_JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE   (3u << 12)
#define MALI_JS_CONFIG_ENABLE_FLUSH_REDUCTION       (1u << 14)
#define MALI_JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK     (1u << 15)
#define MALI_JS_CONFIG_THREAD_PRI(n)                ((n) << 16)

/* JS_STATUS values (mirrors base_jd_event_code). */
#define MALI_JS_STATUS_NO_EXCEPTION        0x00
#define MALI_JS_STATUS_INTERRUPTED         0x02
#define MALI_JS_STATUS_STOPPED             0x03
#define MALI_JS_STATUS_TERMINATED          0x04
#define MALI_JS_STATUS_CONFIG_FAULT        0x40
#define MALI_JS_STATUS_POWER_FAULT         0x41
#define MALI_JS_STATUS_READ_FAULT          0x42
#define MALI_JS_STATUS_WRITE_FAULT         0x43
#define MALI_JS_STATUS_AFFINITY_FAULT      0x44
#define MALI_JS_STATUS_BUS_FAULT           0x48
#define MALI_JS_STATUS_INSTR_INVALID_PC    0x50
#define MALI_JS_STATUS_INSTR_INVALID_ENC   0x51
#define MALI_JS_STATUS_INSTR_TYPE_MISMATCH 0x52
#define MALI_JS_STATUS_INSTR_OPERAND_FAULT 0x53
#define MALI_JS_STATUS_INSTR_TLS_FAULT     0x54
#define MALI_JS_STATUS_INSTR_BARRIER_FAULT 0x55
#define MALI_JS_STATUS_INSTR_ALIGN_FAULT   0x56
#define MALI_JS_STATUS_DATA_INVALID_FAULT  0x58
#define MALI_JS_STATUS_TILE_RANGE_FAULT    0x59
#define MALI_JS_STATUS_ADDRESS_RANGE_FAULT 0x5A
#define MALI_JS_STATUS_OUT_OF_MEMORY       0x60
#define MALI_JS_STATUS_UNKNOWN             0x7F

/* ---------------- MMU interrupts (base 0x2000) ---------------- */
#define MALI_MMU_IRQ_RAWSTAT         0x2000
#define MALI_MMU_IRQ_CLEAR           0x2004
#define MALI_MMU_IRQ_MASK            0x2008
#define MALI_MMU_IRQ_STATUS          0x200C
#define MALI_MMU_AS_BASE             0x2400
#define MALI_MMU_AS_REG(n, r)        (MALI_MMU_AS_BASE + ((n) << 6) + (r))

#define MALI_AS_FAULTSTATUS          0x1C
#define MALI_AS_FAULTADDRESS_LO      0x20
#define MALI_AS_FAULTADDRESS_HI      0x24

#define MALI_MMU_PAGE_FAULT(n)       (1u << (n))
#define MALI_MMU_BUS_ERROR(n)        (1u << ((n) + 16))

/* ---------------- Job context + API ---------------- */
struct _MALI_KMD_DEVICE;

typedef struct _MALI_JOB {
    ULONG          SlotCount;
    volatile ULONG SlotGen[MALI_JOB_SLOT_MAX];       /* bumped by the IRQ DPC */
    ULONG          SlotIrqStatus[MALI_JOB_SLOT_MAX]; /* JS_STATUS at completion */
    volatile ULONG LastIrqStat;
} MALI_JOB, *PMALI_JOB;

NTSTATUS MaliJobInitialize(struct _MALI_KMD_DEVICE *Ctx);
VOID     MaliJobCleanup(struct _MALI_KMD_DEVICE *Ctx);
ULONG    MaliJobSlotCount(struct _MALI_KMD_DEVICE *Ctx);

/*
 * Submits one job chain to a slot.
 *   TimeoutMs == 0  -> fire and forget, return current JS_STATUS.
 *   TimeoutMs  > 0  -> wait for completion (IRQ generation or JOB_IRQ_RAWSTAT).
 * Requires the GPU powered; the caller does that (MaliJobSubmit powers it on
 * itself if needed) and the GPU MMU enabled.
 */
/*
 * Job-chain layout the ICD must produce (validated on Mali-G57 tNAx r0p1).
 *
 * Each descriptor starts with the 32-byte "job descriptor header":
 *   +0  u32 exception_status          (must be 0)
 *   +4  u32 first_incomplete_task
 *   +8  u64 fault_pointer
 *   +16 u32 [0]=job_descriptor_size, [7:1]=job_type, [8]=job_barrier,
 *           [31:16]=job_index          (set size=1 for a job with a payload)
 *   +20 u16 dependency_index_1, u16 dependency_index_2
 *   +24 u64 next_job                   (0 terminates the chain)
 * Payloads follow at +32 (e.g. mali_payload_write_value for job_type 2).
 *
 * ALIGNMENT RULE (hardware, measured on silicon):
 *   every descriptor in a chain must be 128-byte aligned, and next_job must
 *   therefore also be 128-byte aligned. A 64-byte stride puts the next header
 *   inside the previous descriptor's 128-byte footprint and the JM rejects the
 *   chain with JS_STATUS 0x58 (DATA_INVALID_FAULT). 128/256 work; 96/192/320
 *   fail. Use a 128-byte stride.
 */
NTSTATUS MaliJobSubmit(struct _MALI_KMD_DEVICE *Ctx, ULONG Slot, UINT64 Jc,
                       UINT64 Affinity, ULONG AsNumber, ULONG Flags,
                       ULONG TimeoutMs, PULONG OutStatus, PULONG OutIrq);

NTSTATUS MaliJobStatus(struct _MALI_KMD_DEVICE *Ctx, ULONG Slot, PULONG Status,
                       PULONG Config, PULONG Present, PULONG64 Head,
                       PULONG64 Tail, PULONG Features);

NTSTATUS MaliJobStop(struct _MALI_KMD_DEVICE *Ctx, ULONG Slot, BOOLEAN Hard);

/*
 * Path-exercise self test: allocate a zeroed 4 KB BO and submit it as a job
 * chain head. An all-zero header terminates immediately, so this is expected
 * to complete as a null job. It exercises BO -> MMU translation -> job slot ->
 * IRQ/status end to end, and a fault code in JS_STATUS is itself diagnostic.
 * MUST be validated on silicon (see ROADMAP Phase 2f).
 */
NTSTATUS MaliJobNullTest(struct _MALI_KMD_DEVICE *Ctx, ULONG Slot,
                         PULONG JsStatus, PULONG IrqStat);
