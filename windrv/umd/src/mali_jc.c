/*
 * mali_jc.c  -  Mali job-chain builder. See mali_jc.h for the encoding.
 *
 * Pure C: no OS headers, no libc. Compiles for ARM64 (the ICD) and for the host
 * (tools/jc_test.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include "mali_jc.h"

static uint32_t *MaliJcDesc(MaliJcBuilder *b, uint32_t k)
{
    return (uint32_t *)(void *)(b->Base + (uint64_t)k * MALI_JC_STRIDE);
}

/* Set the 64-bit next_job field (words 6 and 7) of descriptor k. */
static void MaliJcSetNext(MaliJcBuilder *b, uint32_t k, uint64_t nextGpuVa)
{
    uint32_t *d = MaliJcDesc(b, k);
    d[6] = (uint32_t)(nextGpuVa & 0xFFFFFFFFu);
    d[7] = (uint32_t)(nextGpuVa >> 32);
}

static uint32_t MaliJcEmit(MaliJcBuilder *b, uint32_t jobType)
{
    uint32_t k, i;
    uint32_t *d;

    if (!b || !b->Base) return 0;
    if ((uint64_t)(b->Count + 1u) * MALI_JC_STRIDE > (uint64_t)b->Capacity) return 0;

    k = b->Count;
    d = MaliJcDesc(b, k);

    /* Zero the whole descriptor so no payload field keeps stale bytes. */
    for (i = 0; i < MALI_JC_STRIDE / 4u; i++) d[i] = 0;

    /* word 4: size(1) | job_type | job_index (1-based) */
    d[4] = 1u | ((jobType & 0x7Fu) << 1) | ((b->NextIndex & 0xFFFFu) << 16);

    /* Link the previous descriptor to this one; this one ends the chain for now. */
    if (k > 0u) MaliJcSetNext(b, k - 1u, b->GpuVa + (uint64_t)k * MALI_JC_STRIDE);

    b->Count++;
    return b->NextIndex++;
}

void MaliJcInit(MaliJcBuilder *b, void *base, uint64_t baseGpuVa, uint32_t capacity)
{
    if (!b) return;
    b->Base       = (uint8_t *)base;
    b->GpuVa      = baseGpuVa;
    b->Capacity   = capacity;
    b->Count      = 0;
    b->FirstIndex = 1;
    b->NextIndex  = 1;
}

uint32_t MaliJcCount(const MaliJcBuilder *b)
{
    return b ? b->Count : 0;
}

int MaliJcHasRoom(const MaliJcBuilder *b, uint32_t n)
{
    if (!b || !b->Base || n == 0) return 0;
    return ((uint64_t)(b->Count + n) * MALI_JC_STRIDE) <= (uint64_t)b->Capacity;
}

uint32_t MaliJcNull(MaliJcBuilder *b)
{
    return MaliJcEmit(b, MALI_JC_JOB_NULL);
}

uint32_t MaliJcCacheFlush(MaliJcBuilder *b)
{
    return MaliJcEmit(b, MALI_JC_JOB_CACHE_FLUSH);
}

uint32_t MaliJcWriteValue(MaliJcBuilder *b, uint64_t address,
                          uint32_t valueDescriptor, uint64_t immediate)
{
    uint32_t idx = MaliJcEmit(b, MALI_JC_JOB_WRITE_VALUE);
    uint32_t *d;

    if (idx == 0u) return 0u;   /* chain full */
    d = MaliJcDesc(b, b->Count - 1u);

    d[8]  = (uint32_t)(address & 0xFFFFFFFFu);
    d[9]  = (uint32_t)(address >> 32);
    d[10] = valueDescriptor;
    d[11] = 0;
    d[12] = (uint32_t)(immediate & 0xFFFFFFFFu);
    d[13] = (uint32_t)(immediate >> 32);
    return idx;
}

uint32_t MaliJcWriteValueZero(MaliJcBuilder *b, uint64_t address)
{
    return MaliJcWriteValue(b, address, MALI_JC_WV_ZERO, 0);
}

uint64_t MaliJcHead(const MaliJcBuilder *b)
{
    if (!b || b->Count == 0u) return 0ull;
    return b->GpuVa;
}
