/*
 * mali_jc.h  -  Mali job-chain builder (user mode).
 *
 * Encodes job-manager (JM) job descriptors into a GPU-visible buffer object and
 * links them through next_job. It is deliberately free of any OS or KMD header
 * (the kernel's mali_job.h is kernel-only), so it builds for ARM64 *and* for the
 * host, which is what lets tools/jc_test.c unit-test the byte layout on the
 * development machine.
 *
 * Hardware rule (measured on Mali-G57 tNAx r0p1): every descriptor in a chain
 * must be 128-byte aligned. A 64/96/192-byte stride makes the JM reject the
 * whole chain with JS_STATUS 0x58 (DATA_INVALID_FAULT). MALI_JC_STRIDE encodes
 * that rule in one place.
 *
 * Descriptor layout (32-byte header, payload at +32):
 *   word 0..3 : exception status / fault pointer           (0)
 *   word 4    : [0]=descriptor size(1) | [7:1]=job_type | [31:16]=job_index
 *   word 5    : dependencies (dep1 lo16, dep2 hi16)
 *   word 6..7 : next_job GPU VA (0 ends the chain)
 *   payload   : type-specific, starts at byte 32
 *
 * WRITE_VALUE payload: u64 address; u32 value_descriptor; u32 reserved;
 *                      u64 immediate. Only ZERO (3) is validated on silicon.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#define MALI_JC_STRIDE  128u   /* bytes between descriptors (hardware rule) */
#define MALI_JC_HEADER  32u    /* descriptor header size in bytes           */

/* JM job types (mirrors the kernel's mali_job.h). */
enum {
    MALI_JC_JOB_NULL        = 1,
    MALI_JC_JOB_WRITE_VALUE = 2,
    MALI_JC_JOB_CACHE_FLUSH = 3,
    MALI_JC_JOB_COMPUTE     = 4,
    MALI_JC_JOB_VERTEX      = 5,
    MALI_JC_JOB_GEOMETRY    = 6,
    MALI_JC_JOB_TILER       = 7,
    MALI_JC_JOB_FUSED       = 8,
    MALI_JC_JOB_FRAGMENT    = 9
};

/* WRITE_VALUE data descriptors. ZERO is the one verified on silicon. */
#define MALI_JC_WV_ZERO  3u

typedef struct MaliJcBuilder {
    uint8_t *Base;       /* CPU pointer to the chain buffer (128-byte aligned) */
    uint64_t GpuVa;      /* GPU VA of Base                                     */
    uint32_t Capacity;   /* bytes available at Base                            */
    uint32_t Count;      /* descriptors emitted                                */
    uint32_t NextIndex;  /* next 1-based job_index                             */
    uint32_t FirstIndex; /* job_index of descriptor 0                          */
} MaliJcBuilder;

/* Start a chain. base should be 128-byte aligned and, if the GPU maps it,
 * baseGpuVa should be too. firstIndex is normally 1. */
void MaliJcInit(MaliJcBuilder *b, void *base, uint64_t baseGpuVa, uint32_t capacity);

uint32_t MaliJcCount(const MaliJcBuilder *b);

/* 1 if n more descriptors fit. */
int MaliJcHasRoom(const MaliJcBuilder *b, uint32_t n);

/* Emit a buffered NULL job. Returns the job_index (>0) or 0 if full. */
uint32_t MaliJcNull(MaliJcBuilder *b);

/* Emit a cache-flush job (empty payload). Returns job_index or 0. */
uint32_t MaliJcCacheFlush(MaliJcBuilder *b);

/* Emit a WRITE_VALUE job. valueDescriptor selects how immediate is used;
 * use MALI_JC_WV_ZERO for a zeroing write. Returns job_index or 0. */
uint32_t MaliJcWriteValue(MaliJcBuilder *b, uint64_t address,
                          uint32_t valueDescriptor, uint64_t immediate);

/* Convenience: WRITE_VALUE ZERO at address. Returns job_index or 0. */
uint32_t MaliJcWriteValueZero(MaliJcBuilder *b, uint64_t address);

/* GPU VA of the chain head, or 0 if the chain is empty. */
uint64_t MaliJcHead(const MaliJcBuilder *b);
