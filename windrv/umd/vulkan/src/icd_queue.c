/*
 * icd_queue.c  -  Vulkan ICD command pools/buffers, fetches, fills, copies and
 *                 queue submission (the part that actually drives the GPU).
 *
 * vkCmdFillBuffer records intent. vkQueueSubmit turns a zero fill into a real
 * Mali WRITE_VALUE job chain in the device's chain BO and submits it through
 * the KMD; anything the GPU path cannot express yet (non-zero fill, copies) is
 * done on the CPU, which is legal because every BO is host-visible and
 * host-coherent. Submission is synchronous, so fences are signalled on return.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mali_vk.h"

static void *MaliVkAlloc(uint64_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)size);
}
static void MaliVkFree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ------------------------------------------------------------------ */
/* GPU zero fill: WRITE_VALUE ZERO job chain                           */
/* ------------------------------------------------------------------ */

/*
 * Job descriptor (mali_job.h): 32-byte header, payload at +32, 128-byte stride.
 * WRITE_VALUE payload: u64 address; u32 value_descriptor(=3 ZERO); u32 rsvd;
 *                      u64 immediate. Word indices here are 32-bit words.
 */
static VkResult MaliVkSubmitZeroJobs(MaliVkDevice *Dev, uint64_t GpuVa, uint64_t Size)
{
    uint32_t *w = (uint32_t *)Dev->ChainBo.CpuVa;
    uint32_t jobs, k;
    UINT32 js = 0xFFFFFFFFu, irq = 0;
    MALI_UMD_SUBMIT sub;

    if (!w || (Size & 7) != 0) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    jobs = (uint32_t)(Size / 8);
    if (jobs == 0 || jobs > MALI_VK_MAX_ZERO_JOBS) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    for (k = 0; k < jobs; k++) {
        uint32_t *d = (uint32_t *)((unsigned char *)w + (uint64_t)k * MALI_VK_CHAIN_STRIDE);
        uint64_t nxt = (k + 1 < jobs)
                     ? (Dev->ChainBo.GpuVa + (uint64_t)(k + 1) * MALI_VK_CHAIN_STRIDE)
                     : 0ull;
        uint64_t tgt = GpuVa + (uint64_t)k * 8ull;

        d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0;                 /* exception/fault */
        d[4] = (1u << 0) | ((2u & 0x7Fu) << 1) | (((k + 1u) & 0xFFFFu) << 16);
        d[5] = 0;                                              /* dependencies   */
        d[6] = (uint32_t)(nxt & 0xFFFFFFFFu);
        d[7] = (uint32_t)(nxt >> 32);
        d[8]  = (uint32_t)(tgt & 0xFFFFFFFFu);                 /* payload address */
        d[9]  = (uint32_t)(tgt >> 32);
        d[10] = 3;                                             /* ZERO descriptor  */
        d[11] = 0; d[12] = 0; d[13] = 0;                       /* immediate = 0    */
    }

    MaliVkZero(&sub, sizeof(sub));
    sub.Slot      = 0;
    sub.Flags     = 0;
    sub.Jc        = Dev->ChainBo.GpuVa;
    sub.Affinity  = 0;
    sub.AsNumber  = 0;
    sub.TimeoutMs = 1000;

    if (!MaliUmdJobSubmit(Dev->Umd, &sub, &js, &irq)) {
        MaliVkLog("job submit IOCTL failed");
        return VK_ERROR_DEVICE_LOST;
    }
    if (js != 0) {
        MaliVkLog("WRITE_VALUE chain failed (JS_STATUS != 0)");
        return VK_ERROR_DEVICE_LOST;
    }
    return VK_SUCCESS;
}

void MaliVkExecuteSubmit(MaliVkDevice *Dev, MaliVkCmdBuffer *Cmd)
{
    uint32_t i;

    if (!Dev || !Cmd) return;

    for (i = 0; i < Cmd->OpCount; i++) {
        MaliVkOp *op = &Cmd->Ops[i];

        if (op->Kind == MALI_VK_OP_FILL) {
            if (op->Value == 0 && (op->Size & 7) == 0 &&
                (op->Size / 8) <= MALI_VK_MAX_ZERO_JOBS) {
                if (MaliVkSubmitZeroJobs(Dev, op->DstGpuVa, op->Size) == VK_SUCCESS) {
                    continue;   /* done on the GPU */
                }
            }
            /* Fallback: host fill. */
            if (op->DstCpu) {
                unsigned char *p = (unsigned char *)op->DstCpu;
                uint64_t n = op->Size;
                uint32_t v = op->Value;
                if ((v & 0xFFu) == ((v >> 8) & 0xFFu) && (v & 0xFFu) == ((v >> 16) & 0xFFu)
                    && (v & 0xFFu) == ((v >> 24) & 0xFFu)) {
                    unsigned char b = (unsigned char)(v & 0xFFu);
                    while (n--) *p++ = b;
                } else {
                    uint64_t q = 0;
                    while (n >= 4) { *(uint32_t *)(p + q) = v; q += 4; n -= 4; }
                    while (n--) *(p + q++) = (unsigned char)v;
                }
            }
        } else if (op->Kind == MALI_VK_OP_COPY) {
            if (op->SrcCpu && op->DstCpu) {
                const unsigned char *s = (const unsigned char *)op->SrcCpu;
                unsigned char *d = (unsigned char *)op->DstCpu;
                uint64_t n = op->Size;
                while (n--) *d++ = *s++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Command pool / buffer                                               */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkCommandPool *pool;
    (void)pCreateInfo; (void)pAllocator;

    if (!dev || !pCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    pool = (MaliVkCommandPool *)MaliVkAlloc(sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pool->Dev = dev;
    *pCommandPool = (VkCommandPool)pool;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator)
{
    MaliVkCommandPool *pool = (MaliVkCommandPool *)commandPool;
    uint32_t i;
    (void)device; (void)pAllocator;
    if (!pool) return;
    for (i = 0; i < pool->Count; i++) MaliVkFree(pool->Buffers[i]);
    MaliVkFree(pool);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
    VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
{
    MaliVkCommandPool *pool = (MaliVkCommandPool *)commandPool;
    uint32_t i;
    (void)device; (void)flags;
    if (!pool) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < pool->Count; i++) {
        MaliVkCmdBuffer *cb = (MaliVkCmdBuffer *)pool->Buffers[i];
        cb->OpCount = 0;
        cb->State   = VK_SUCCESS;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
    VkCommandBuffer *pCommandBuffers)
{
    MaliVkCommandPool *pool = (MaliVkCommandPool *)pAllocateInfo->commandPool;
    MaliVkDevice *dev = (MaliVkDevice *)device;
    uint32_t i;

    if (!pool || !pCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;

    for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        MaliVkCmdBuffer *cb = (MaliVkCmdBuffer *)MaliVkAlloc(sizeof(*cb));
        if (!cb) return VK_ERROR_OUT_OF_HOST_MEMORY;
        cb->Dev   = dev;
        cb->Level = pAllocateInfo->level;
        cb->State = VK_SUCCESS;
        if (pool->Count < 64) pool->Buffers[pool->Count++] = cb;
        pCommandBuffers[i] = (VkCommandBuffer)cb;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice device, VkCommandPool commandPool,
    uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
    MaliVkCommandPool *pool = (MaliVkCommandPool *)commandPool;
    uint32_t i, j;
    (void)device;

    for (i = 0; i < commandBufferCount; i++) {
        if (!pCommandBuffers[i]) continue;
        if (pool) {
            for (j = 0; j < pool->Count; j++) {
                if (pool->Buffers[j] == (void *)pCommandBuffers[i]) {
                    pool->Buffers[j] = pool->Buffers[--pool->Count];
                    break;
                }
            }
        }
        MaliVkFree((void *)pCommandBuffers[i]);
    }
}

static MaliVkCmdBuffer *AsCmd(VkCommandBuffer cb) { return (MaliVkCmdBuffer *)cb; }

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    cb->Usage   = pBeginInfo ? pBeginInfo->flags : 0;
    cb->OpCount = 0;
    cb->State   = VK_SUCCESS;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    return cb->State;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
    VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    (void)flags;
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    cb->OpCount = 0;
    cb->State   = VK_SUCCESS;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Fills and copies                                                    */
/* ------------------------------------------------------------------ */

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(
    VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
    VkDeviceSize size, uint32_t data)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    MaliVkBuffer *dst = (MaliVkBuffer *)dstBuffer;
    MaliVkOp *op;

    if (!cb || !dst || !dst->Mem) return;
    if (cb->OpCount >= MALI_VK_MAX_OPS) { cb->State = VK_ERROR_OUT_OF_HOST_MEMORY; return; }

    op = &cb->Ops[cb->OpCount++];
    MaliVkZero(op, sizeof(*op));
    op->Kind     = MALI_VK_OP_FILL;
    op->Value    = data;
    op->Size     = size;
    op->DstGpuVa = dst->Mem->Bo.GpuVa + dst->MemOffset + dstOffset;
    op->DstCpu   = (unsigned char *)dst->Mem->Bo.CpuVa + dst->MemOffset + dstOffset;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
    uint32_t regionCount, const VkBufferCopy *pRegions)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    MaliVkBuffer *src = (MaliVkBuffer *)srcBuffer;
    MaliVkBuffer *dst = (MaliVkBuffer *)dstBuffer;
    uint32_t i;

    if (!cb || !src || !dst || !src->Mem || !dst->Mem) return;

    for (i = 0; i < regionCount; i++) {
        MaliVkOp *op;
        if (cb->OpCount >= MALI_VK_MAX_OPS) { cb->State = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
        op = &cb->Ops[cb->OpCount++];
        MaliVkZero(op, sizeof(*op));
        op->Kind     = MALI_VK_OP_COPY;
        op->Size     = pRegions[i].size;
        op->DstGpuVa = dst->Mem->Bo.GpuVa + dst->MemOffset + pRegions[i].dstOffset;
        op->DstCpu   = (unsigned char *)dst->Mem->Bo.CpuVa + dst->MemOffset + pRegions[i].dstOffset;
        op->SrcCpu   = (const unsigned char *)src->Mem->Bo.CpuVa + src->MemOffset + pRegions[i].srcOffset;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    /* BOs are host-coherent and submission is synchronous, so barriers are
     * no-ops for this bring-up ICD. */
    (void)commandBuffer; (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

/* ------------------------------------------------------------------ */
/* Fences                                                              */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
    VkDevice device, const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFence *pFence)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkFence *f;
    (void)pAllocator;

    if (!dev || !pFence) return VK_ERROR_INITIALIZATION_FAILED;
    f = (MaliVkFence *)MaliVkAlloc(sizeof(*f));
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    f->Dev = dev;
    f->Signaled = (pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT)) ? 1u : 0u;
    *pFence = (VkFence)f;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
    VkDevice device, uint32_t fenceCount, const VkFence *pFences)
{
    uint32_t i;
    (void)device;
    for (i = 0; i < fenceCount; i++) {
        MaliVkFence *f = (MaliVkFence *)pFences[i];
        if (f) f->Signaled = 0;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence)
{
    MaliVkFence *f = (MaliVkFence *)fence;
    (void)device;
    if (!f) return VK_ERROR_INITIALIZATION_FAILED;
    return f->Signaled ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
    VkDevice device, uint32_t fenceCount, const VkFence *pFences,
    VkBool32 waitAll, uint64_t timeout)
{
    uint32_t i;
    (void)device; (void)timeout;

    for (i = 0; i < fenceCount; i++) {
        MaliVkFence *f = (MaliVkFence *)pFences[i];
        if (!f) continue;
        if (!f->Signaled && waitAll) return VK_TIMEOUT;
    }
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Queue submission                                                    */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
    MaliVkQueue *q = (MaliVkQueue *)queue;
    uint32_t i, j;

    if (!q || !q->Dev) return VK_ERROR_INITIALIZATION_FAILED;

    for (i = 0; i < submitCount; i++) {
        const VkSubmitInfo *s = &pSubmits[i];
        for (j = 0; j < s->commandBufferCount; j++) {
            MaliVkExecuteSubmit(q->Dev, (MaliVkCmdBuffer *)s->pCommandBuffers[j]);
        }
    }

    if (fence) ((MaliVkFence *)fence)->Signaled = 1;
    return VK_SUCCESS;
}
