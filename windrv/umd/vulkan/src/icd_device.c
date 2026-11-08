/*
 * icd_device.c  -  Vulkan ICD objects: instance, physical device, device,
 *                  memory (KMD BO) and buffers.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "mali_vk.h"

#define MALI_VK_MAGIC 0x4D616C69u   /* 'Mali' */

/* ------------------------------------------------------------------ */
/* CRT-free helpers (declared in mali_vk.h)                            */
/* ------------------------------------------------------------------ */

int MaliVkStrEq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void MaliVkZero(void *p, uint64_t n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--) *b++ = 0;
}

void MaliVkLog(const char *s)
{
    OutputDebugStringA("[mali_vk] ");
    OutputDebugStringA(s);
    OutputDebugStringA("\r\n");
}

static void *MaliVkAlloc(uint64_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)size);
}

static void MaliVkFree(void *p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

/* ------------------------------------------------------------------ */
/* Instance                                                            */
/* ------------------------------------------------------------------ */

static MaliVkPhysicalDevice g_PhysDev = { 0u };

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion)
{
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pCount, VkExtensionProperties *pProps)
{
    (void)pProps;
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
    uint32_t *pCount, VkLayerProperties *pProps)
{
    (void)pProps;
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
    MaliVkInstance *inst;
    (void)pCreateInfo; (void)pAllocator;

    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;
    inst = (MaliVkInstance *)MaliVkAlloc(sizeof(*inst));
    if (!inst) return VK_ERROR_OUT_OF_HOST_MEMORY;
    inst->Magic = MALI_VK_MAGIC;
    *pInstance = (VkInstance)inst;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,
                                             const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    MaliVkFree((void *)instance);
}

/* ------------------------------------------------------------------ */
/* Physical device                                                     */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pCount, VkPhysicalDevice *pDevices)
{
    PMALI_UMD_DEVICE probe;
    uint32_t available;

    (void)instance;
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;

    /* Only advertise a device when the Mali KMD is actually present, so an
     * application never sees an adapter it cannot create a device on. */
    probe = MaliUmdOpen();
    available = probe ? 1u : 0u;
    if (probe) MaliUmdClose(probe);

    if (!pDevices) { *pCount = available; return available ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED; }
    if (*pCount == 0) return VK_SUCCESS;
    if (available == 0) { *pCount = 0; return VK_ERROR_INITIALIZATION_FAILED; }

    pDevices[0] = (VkPhysicalDevice)&g_PhysDev;
    *pCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties *pProperties)
{
    VkPhysicalDeviceLimits *l = &pProperties->limits;
    (void)physicalDevice;

    MaliVkZero(pProperties, sizeof(*pProperties));
    pProperties->apiVersion   = VK_API_VERSION_1_0;
    pProperties->driverVersion = VK_MAKE_VERSION(0, 1, 0);
    pProperties->vendorID     = 0x13B5;   /* Arm Ltd */
    pProperties->deviceID     = 0x9003;   /* Mali-G57 (tNAx) */
    pProperties->deviceType   = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    {
        static const char name[] = "Mali-G57 MC2 (MT6789) - Jalur A";
        uint32_t i;
        for (i = 0; i < sizeof(name) && i < sizeof(pProperties->deviceName); i++)
            pProperties->deviceName[i] = name[i];
    }

    /* Vulkan 1.0 spec minimums for a functional, if minimal, device. */
    l->maxImageDimension2D          = 4096;
    l->maxImageDimensionCube        = 4096;
    l->maxImageArrayLayers          = 256;
    l->maxTexelBufferElements       = 65536;
    l->maxUniformBufferRange        = 16384;
    l->maxStorageBufferRange        = 65536;   /* BO cap is 256 MB overall */
    l->maxPushConstantsSize         = 128;
    l->maxMemoryAllocationCount     = 4096;
    l->maxSamplerAllocationCount    = 4000;
    l->bufferImageGranularity       = 4096;
    l->maxVertexInputAttributes     = 16;
    l->maxVertexInputBindings       = 16;
    l->maxVertexOutputComponents    = 64;
    l->maxFragmentInputComponents   = 64;
    l->maxFragmentOutputAttachments = 4;
    l->maxColorAttachments          = 4;
    l->maxComputeSharedMemorySize   = 16384;
    l->maxComputeWorkGroupInvocations = 128;
    l->nonCoherentAtomSize          = 256;
    l->minMemoryMapAlignment        = 4096;
    l->minTexelBufferOffsetAlignment = 16;
    l->minUniformBufferOffsetAlignment = 16;
    l->minStorageBufferOffsetAlignment = 16;
    l->optimalBufferCopyOffsetAlignment = 128;
    l->optimalBufferCopyRowPitchAlignment = 128;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
    (void)physicalDevice;
    MaliVkZero(pFeatures, sizeof(*pFeatures));
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pCount, VkQueueFamilyProperties *pProps)
{
    (void)physicalDevice;
    if (!pCount) return;
    if (!pProps) { *pCount = 1; return; }
    if (*pCount == 0) return;

    MaliVkZero(&pProps[0], sizeof(pProps[0]));
    pProps[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    pProps[0].queueCount = 1;
    pProps[0].timestampValidBits = 0;
    pProps[0].minImageTransferGranularity.width  = 1;
    pProps[0].minImageTransferGranularity.height = 1;
    pProps[0].minImageTransferGranularity.depth  = 1;
    *pCount = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties *pProperties)
{
    (void)physicalDevice;
    MaliVkZero(pProperties, sizeof(*pProperties));

    pProperties->memoryHeapCount = 1;
    pProperties->memoryHeaps[0].size  = 256ull * 1024ull * 1024ull;
    pProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    /* One physical pool; expose it as device-local and as host-visible. Our
     * BOs are contiguous and permanently CPU-mapped, so both views are honest. */
    pProperties->memoryTypeCount = 2;
    pProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pProperties->memoryTypes[0].heapIndex     = 0;
    pProperties->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pProperties->memoryTypes[1].heapIndex     = 0;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties)
{
    (void)physicalDevice; (void)format;
    MaliVkZero(pFormatProperties, sizeof(*pFormatProperties));
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pProperties)
{
    (void)physicalDevice; (void)format; (void)type; (void)tiling;
    (void)usage; (void)flags; (void)pProperties;
    return VK_ERROR_FORMAT_NOT_SUPPORTED;   /* images are Jalur B */
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pCount, VkExtensionProperties *pProps)
{
    (void)physicalDevice; (void)pLayerName; (void)pProps;
    if (pCount) *pCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type,
    VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageTiling tiling,
    uint32_t *pCount, VkSparseImageFormatProperties *pProperties)
{
    (void)physicalDevice; (void)format; (void)type; (void)samples;
    (void)usage; (void)tiling; (void)pProperties;
    if (pCount) *pCount = 0;
}

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
    MaliVkDevice *dev;
    (void)physicalDevice; (void)pCreateInfo; (void)pAllocator;

    if (!pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    dev = (MaliVkDevice *)MaliVkAlloc(sizeof(*dev));
    if (!dev) return VK_ERROR_OUT_OF_HOST_MEMORY;

    dev->Umd = MaliUmdOpen();
    if (!dev->Umd) { MaliVkFree(dev); return VK_ERROR_INITIALIZATION_FAILED; }

    if (!MaliUmdPowerOn(dev->Umd) || !MaliUmdMmuEnable(dev->Umd)) {
        MaliUmdClose(dev->Umd);
        MaliVkFree(dev);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!MaliUmdBoAlloc(dev->Umd, MALI_VK_CHAIN_BO_SIZE,
                        MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD | MALI_BO_GPU_WR,
                        &dev->ChainBo)) {
        MaliUmdClose(dev->Umd);
        MaliVkFree(dev);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    dev->ChainUsed = 0;

    dev->Queue.Dev    = dev;
    dev->Queue.Family = 0;

    *pDevice = (VkDevice)dev;
    MaliVkLog("device created (Mali KMD open, MMU on, chain BO ready)");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device,
                                           const VkAllocationCallbacks *pAllocator)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    (void)pAllocator;
    if (!dev) return;

    if (dev->ChainBo.Handle) MaliUmdBoFree(dev->Umd, dev->ChainBo.Handle);
    MaliUmdMmuDisable(dev->Umd);
    MaliUmdClose(dev->Umd);
    MaliVkFree(dev);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                            uint32_t queueIndex, VkQueue *pQueue)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    (void)queueFamilyIndex; (void)queueIndex;
    if (pQueue && dev) *pQueue = (VkQueue)&dev->Queue;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    (void)device;
    return VK_SUCCESS;   /* submissions are synchronous */
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
    (void)queue;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Device memory (KMD buffer objects)                                  */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkMemory *mem;
    uint64_t size;
    (void)pAllocator;

    if (!dev || !pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;

    size = pAllocateInfo->allocationSize;
    if (size == 0) size = 4096;

    mem = (MaliVkMemory *)MaliVkAlloc(sizeof(*mem));
    if (!mem) return VK_ERROR_OUT_OF_HOST_MEMORY;

    mem->Dev = dev;
    if (!MaliUmdBoAlloc(dev->Umd, size,
                        MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD | MALI_BO_GPU_WR,
                        &mem->Bo)) {
        MaliVkFree(mem);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    *pMemory = (VkDeviceMemory)mem;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                        const VkAllocationCallbacks *pAllocator)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkMemory *mem = (MaliVkMemory *)memory;
    (void)pAllocator;

    if (!dev || !mem) return;
    if (mem->Bo.Handle) MaliUmdBoFree(dev->Umd, mem->Bo.Handle);
    MaliVkFree(mem);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory,
                                           VkDeviceSize offset, VkDeviceSize size,
                                           VkMemoryMapFlags flags, void **ppData)
{
    MaliVkMemory *mem = (MaliVkMemory *)memory;
    (void)device; (void)size; (void)flags;

    if (!mem || !ppData) return VK_ERROR_INITIALIZATION_FAILED;
    *ppData = (void *)((unsigned char *)mem->Bo.CpuVa + offset);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    (void)device; (void)memory;   /* mapping is permanent for a BO */
}

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;   /* BOs are host-coherent */
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize *pCommittedMemoryInBytes)
{
    MaliVkMemory *mem = (MaliVkMemory *)memory;
    (void)device;
    if (pCommittedMemoryInBytes && mem) *pCommittedMemoryInBytes = mem->Bo.Size;
}

/* ------------------------------------------------------------------ */
/* Buffers                                                             */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkBuffer *buf;
    (void)pAllocator;

    if (!dev || !pCreateInfo || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;

    buf = (MaliVkBuffer *)MaliVkAlloc(sizeof(*buf));
    if (!buf) return VK_ERROR_OUT_OF_HOST_MEMORY;
    buf->Dev  = dev;
    buf->Size = pCreateInfo->size;
    *pBuffer = (VkBuffer)buf;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer,
                                           const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)buffer);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements)
{
    MaliVkBuffer *buf = (MaliVkBuffer *)buffer;
    (void)device;

    MaliVkZero(pMemoryRequirements, sizeof(*pMemoryRequirements));
    pMemoryRequirements->size           = buf ? buf->Size : 4096;
    pMemoryRequirements->alignment      = 4096;
    pMemoryRequirements->memoryTypeBits = (1u << 0) | (1u << 1);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
    MaliVkBuffer *buf = (MaliVkBuffer *)buffer;
    MaliVkMemory *mem = (MaliVkMemory *)memory;
    (void)device;

    if (!buf || !mem) return VK_ERROR_INITIALIZATION_FAILED;
    if (memoryOffset + buf->Size > mem->Bo.Size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    buf->Mem       = mem;
    buf->MemOffset = memoryOffset;
    return VK_SUCCESS;
}
