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
#include "mali_jc.h"

static void *MaliVkAlloc(uint64_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)size);
}
static void MaliVkFree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ------------------------------------------------------------------ */
/* GPU zero fill: WRITE_VALUE ZERO job chain                           */
/* ------------------------------------------------------------------ */

/*
 * Job descriptors are encoded by the shared, host-tested builder (mali_jc), so
 * the 128-byte alignment / job_index / next_job rules live in exactly one place.
 * WRITE_VALUE ZERO zeroes 8 bytes, so a fill becomes size/8 linked jobs.
 */
static VkResult MaliVkSubmitZeroJobs(MaliVkDevice *Dev, uint64_t GpuVa, uint64_t Size)
{
    MaliJcBuilder jc;
    uint32_t jobs, k;
    UINT32 js = 0xFFFFFFFFu, irq = 0;
    MALI_UMD_SUBMIT sub;

    if (!Dev->ChainBo.CpuVa || (Size & 7) != 0) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    jobs = (uint32_t)(Size / 8);
    if (jobs == 0 || jobs > MALI_VK_MAX_ZERO_JOBS) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if ((uint64_t)jobs * MALI_JC_STRIDE > MALI_VK_CHAIN_BO_SIZE) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    MaliJcInit(&jc, Dev->ChainBo.CpuVa, Dev->ChainBo.GpuVa, MALI_VK_CHAIN_BO_SIZE);
    for (k = 0; k < jobs; k++) {
        if (MaliJcWriteValueZero(&jc, GpuVa + (uint64_t)k * 8ull) == 0u) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    MaliVkZero(&sub, sizeof(sub));
    sub.Slot      = 0;
    sub.Flags     = 0;
    sub.Jc        = MaliJcHead(&jc);
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

static void MaliVkCmdBufferFreeOps(MaliVkCmdBuffer *cb)
{
    uint32_t i;
    if (!cb) return;
    for (i = 0; i < cb->OpCount; i++) {
        if (cb->Ops[i].DataAlloc) {
            MaliVkFree(cb->Ops[i].DataAlloc);
            cb->Ops[i].DataAlloc = NULL;
        }
    }
    cb->OpCount = 0;
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
        } else if (op->Kind == MALI_VK_OP_UPDATE) {
            if (op->DataAlloc && op->DstCpu) {
                const unsigned char *s = (const unsigned char *)op->DataAlloc;
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
    for (i = 0; i < pool->Count; i++) {
        MaliVkCmdBufferFreeOps((MaliVkCmdBuffer *)pool->Buffers[i]);
        MaliVkFree(pool->Buffers[i]);
    }
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
        MaliVkCmdBufferFreeOps(cb);
        cb->State = VK_SUCCESS;
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
        MaliVkCmdBuffer *cb = (MaliVkCmdBuffer *)pCommandBuffers[i];
        if (!cb) continue;
        if (pool) {
            for (j = 0; j < pool->Count; j++) {
                if (pool->Buffers[j] == (void *)cb) {
                    pool->Buffers[j] = pool->Buffers[--pool->Count];
                    break;
                }
            }
        }
        MaliVkCmdBufferFreeOps(cb);
        MaliVkFree((void *)cb);
    }
}

static MaliVkCmdBuffer *AsCmd(VkCommandBuffer cb) { return (MaliVkCmdBuffer *)cb; }

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    if (!cb) return VK_ERROR_INITIALIZATION_FAILED;
    MaliVkCmdBufferFreeOps(cb);
    cb->Usage = pBeginInfo ? pBeginInfo->flags : 0;
    cb->State = VK_SUCCESS;
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
    MaliVkCmdBufferFreeOps(cb);
    cb->State = VK_SUCCESS;
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

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(
    VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset,
    VkDeviceSize dataSize, const void *pData)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    MaliVkBuffer *dst = (MaliVkBuffer *)dstBuffer;
    MaliVkOp *op;
    void *copy;

    if (!cb || !dst || !dst->Mem || !pData || dataSize == 0) return;
    if (cb->OpCount >= MALI_VK_MAX_OPS) { cb->State = VK_ERROR_OUT_OF_HOST_MEMORY; return; }

    copy = MaliVkAlloc(dataSize);
    if (!copy) { cb->State = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
    {
        const unsigned char *s = (const unsigned char *)pData;
        unsigned char *d = (unsigned char *)copy;
        uint64_t n = dataSize;
        while (n--) *d++ = *s++;
    }

    op = &cb->Ops[cb->OpCount++];
    MaliVkZero(op, sizeof(*op));
    op->Kind      = MALI_VK_OP_UPDATE;
    op->Size      = dataSize;
    op->DstGpuVa  = dst->Mem->Bo.GpuVa + dst->MemOffset + dstOffset;
    op->DstCpu    = (unsigned char *)dst->Mem->Bo.CpuVa + dst->MemOffset + dstOffset;
    op->DataAlloc = copy;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage,
    VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    MaliVkBuffer *src = (MaliVkBuffer *)srcBuffer;
    MaliVkImage *dst = (MaliVkImage *)dstImage;
    uint32_t i;
    (void)dstImageLayout;

    if (!cb || !src || !dst || !src->Mem || !dst->Mem) return;

    for (i = 0; i < regionCount; i++) {
        MaliVkOp *op;
        uint32_t bpp = (dst->Format == VK_FORMAT_R8_UNORM) ? 1 : 4;
        uint64_t copyBytes = (uint64_t)pRegions[i].imageExtent.width *
                             (uint64_t)pRegions[i].imageExtent.height *
                             (uint64_t)pRegions[i].imageExtent.depth * bpp;

        if (cb->OpCount >= MALI_VK_MAX_OPS) { cb->State = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
        op = &cb->Ops[cb->OpCount++];
        MaliVkZero(op, sizeof(*op));
        op->Kind     = MALI_VK_OP_COPY;
        op->Size     = copyBytes;
        op->DstGpuVa = dst->Mem->Bo.GpuVa + dst->MemOffset;
        op->DstCpu   = (unsigned char *)dst->Mem->Bo.CpuVa + dst->MemOffset;
        op->SrcCpu   = (const unsigned char *)src->Mem->Bo.CpuVa + src->MemOffset + pRegions[i].bufferOffset;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(
    VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout,
    VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy *pRegions)
{
    MaliVkCmdBuffer *cb = AsCmd(commandBuffer);
    MaliVkImage *src = (MaliVkImage *)srcImage;
    MaliVkBuffer *dst = (MaliVkBuffer *)dstBuffer;
    uint32_t i;
    (void)srcImageLayout;

    if (!cb || !src || !dst || !src->Mem || !dst->Mem) return;

    for (i = 0; i < regionCount; i++) {
        MaliVkOp *op;
        uint32_t bpp = (src->Format == VK_FORMAT_R8_UNORM) ? 1 : 4;
        uint64_t copyBytes = (uint64_t)pRegions[i].imageExtent.width *
                             (uint64_t)pRegions[i].imageExtent.height *
                             (uint64_t)pRegions[i].imageExtent.depth * bpp;

        if (cb->OpCount >= MALI_VK_MAX_OPS) { cb->State = VK_ERROR_OUT_OF_HOST_MEMORY; return; }
        op = &cb->Ops[cb->OpCount++];
        MaliVkZero(op, sizeof(*op));
        op->Kind     = MALI_VK_OP_COPY;
        op->Size     = copyBytes;
        op->DstGpuVa = dst->Mem->Bo.GpuVa + dst->MemOffset + pRegions[i].bufferOffset;
        op->DstCpu   = (unsigned char *)dst->Mem->Bo.CpuVa + dst->MemOffset + pRegions[i].bufferOffset;
        op->SrcCpu   = (const unsigned char *)src->Mem->Bo.CpuVa + src->MemOffset;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(
    VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
    const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
    (void)commandBuffer; (void)image; (void)imageLayout; (void)pColor; (void)rangeCount; (void)pRanges;
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(
    VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout,
    const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
    (void)commandBuffer; (void)image; (void)imageLayout; (void)pDepthStencil; (void)rangeCount; (void)pRanges;
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(
    VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment *pAttachments,
    uint32_t rectCount, const VkClearRect *pRects)
{
    (void)commandBuffer; (void)attachmentCount; (void)pAttachments; (void)rectCount; (void)pRects;
}

VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
    VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers)
{
    (void)commandBuffer; (void)commandBufferCount; (void)pCommandBuffers;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
    (void)commandBuffer; (void)pipelineBindPoint; (void)pipeline;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(
    VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport *pViewports)
{
    (void)commandBuffer; (void)firstViewport; (void)viewportCount; (void)pViewports;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(
    VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D *pScissors)
{
    (void)commandBuffer; (void)firstScissor; (void)scissorCount; (void)pScissors;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
    (void)commandBuffer; (void)lineWidth;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets)
{
    (void)commandBuffer; (void)pipelineBindPoint; (void)layout; (void)firstSet;
    (void)descriptorSetCount; (void)pDescriptorSets; (void)dynamicOffsetCount; (void)pDynamicOffsets;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
{
    (void)commandBuffer; (void)buffer; (void)offset; (void)indexType;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount,
    const VkBuffer *pBuffers, const VkDeviceSize *pOffsets)
{
    (void)commandBuffer; (void)firstBinding; (void)bindingCount; (void)pBuffers; (void)pOffsets;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(
    VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance)
{
    (void)commandBuffer; (void)vertexCount; (void)instanceCount; (void)firstVertex; (void)firstInstance;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
    (void)commandBuffer; (void)indexCount; (void)instanceCount; (void)firstIndex;
    (void)vertexOffset; (void)firstInstance;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(
    VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
{
    (void)commandBuffer; (void)groupCountX; (void)groupCountY; (void)groupCountZ;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin, VkSubpassContents contents)
{
    (void)commandBuffer; (void)pRenderPassBegin; (void)contents;
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    (void)commandBuffer;
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
/* Semaphores                                                          */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(
    VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkSemaphore *s;
    (void)pCreateInfo; (void)pAllocator;

    if (!dev || !pSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    s = (MaliVkSemaphore *)MaliVkAlloc(sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->Dev = dev;
    s->Signaled = 0;
    *pSemaphore = (VkSemaphore)s;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)semaphore);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(
    VkDevice device, const VkEventCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkEvent *e;
    (void)pCreateInfo; (void)pAllocator;

    if (!dev || !pEvent) return VK_ERROR_INITIALIZATION_FAILED;
    e = (MaliVkEvent *)MaliVkAlloc(sizeof(*e));
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    e->Dev = dev;
    e->Signaled = 0;
    *pEvent = (VkEvent)e;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(
    VkDevice device, VkEvent event, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)event);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent event)
{
    MaliVkEvent *e = (MaliVkEvent *)event;
    (void)device;
    if (!e) return VK_ERROR_INITIALIZATION_FAILED;
    return e->Signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event)
{
    MaliVkEvent *e = (MaliVkEvent *)event;
    (void)device;
    if (!e) return VK_ERROR_INITIALIZATION_FAILED;
    e->Signaled = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event)
{
    MaliVkEvent *e = (MaliVkEvent *)event;
    (void)device;
    if (!e) return VK_ERROR_INITIALIZATION_FAILED;
    e->Signaled = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(
    VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    (void)commandBuffer; (void)stageMask;
    vkSetEvent(VK_NULL_HANDLE, event);
}

VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent(
    VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    (void)commandBuffer; (void)stageMask;
    vkResetEvent(VK_NULL_HANDLE, event);
}

VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents(
    VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
    VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers)
{
    (void)commandBuffer; (void)eventCount; (void)pEvents; (void)srcStageMask; (void)dstStageMask;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount; (void)pImageMemoryBarriers;
}

/* ------------------------------------------------------------------ */
/* Queue submission                                                    */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence)
{
    MaliVkQueue *q = (MaliVkQueue *)queue;
    uint32_t i, j, k;

    if (!q || !q->Dev) return VK_ERROR_INITIALIZATION_FAILED;

    for (i = 0; i < submitCount; i++) {
        const VkSubmitInfo *s = &pSubmits[i];
        for (j = 0; j < s->commandBufferCount; j++) {
            MaliVkExecuteSubmit(q->Dev, (MaliVkCmdBuffer *)s->pCommandBuffers[j]);
        }
        for (k = 0; k < s->signalSemaphoreCount; k++) {
            MaliVkSemaphore *sem = (MaliVkSemaphore *)s->pSignalSemaphores[k];
            if (sem) sem->Signaled = 1;
        }
    }

    if (fence) ((MaliVkFence *)fence)->Signaled = 1;
    return VK_SUCCESS;
}
