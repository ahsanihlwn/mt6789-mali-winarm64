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

void *MaliVkAlloc(uint64_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)size);
}

void MaliVkFree(void *p)
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

/* CRT-free: struct assignment of these arrays makes the compiler emit a
 * memcpy call, which we cannot link. Copy field by field instead. */
static void MaliVkCopyExtension(VkExtensionProperties *dst, const VkExtensionProperties *src)
{
    uint32_t i;
    for (i = 0; i < VK_MAX_EXTENSION_NAME_SIZE; i++)
        dst->extensionName[i] = src->extensionName[i];
    dst->specVersion = src->specVersion;
}

static const VkExtensionProperties g_InstanceExtensions[] = {
    { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION },
    { VK_KHR_SURFACE_EXTENSION_NAME,                          VK_KHR_SURFACE_SPEC_VERSION },
    { VK_KHR_WIN32_SURFACE_EXTENSION_NAME,                    VK_KHR_WIN32_SURFACE_SPEC_VERSION },
};

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pCount, VkExtensionProperties *pProps)
{
    uint32_t total = (uint32_t)(sizeof(g_InstanceExtensions) / sizeof(g_InstanceExtensions[0]));
    uint32_t i;
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProps) { *pCount = total; return VK_SUCCESS; }
    if (*pCount > total) *pCount = total;
    for (i = 0; i < *pCount; i++) MaliVkCopyExtension(&pProps[i], &g_InstanceExtensions[i]);
    return (*pCount < total) ? VK_INCOMPLETE : VK_SUCCESS;
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

    if (!pDevices) { *pCount = available; return VK_SUCCESS; }
    if (*pCount == 0) return VK_SUCCESS;
    if (available == 0) { *pCount = 0; return VK_SUCCESS; }
    if (*pCount > available) *pCount = available;

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

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
    VkBaseOutStructure *chain;
    if (!pProperties) return;

    vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

    chain = (VkBaseOutStructure *)pProperties->pNext;
    while (chain) {
        if (chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
            VkPhysicalDeviceIDProperties *id = (VkPhysicalDeviceIDProperties *)chain;
            static const uint8_t devUuid[VK_UUID_SIZE] = {
                0x13, 0xB5, 0x90, 0x03, 0x67, 0x89, 0x00, 0x00,
                0x4D, 0x61, 0x6C, 0x69, 0x2D, 0x47, 0x35, 0x37
            };
            static const uint8_t drvUuid[VK_UUID_SIZE] = {
                0x57, 0x49, 0x4E, 0x41, 0x52, 0x4D, 0x36, 0x34,
                0x4D, 0x41, 0x4C, 0x49, 0x56, 0x4B, 0x00, 0x01
            };
            uint32_t i;
            for (i = 0; i < VK_UUID_SIZE; i++) {
                id->deviceUUID[i] = devUuid[i];
                id->driverUUID[i] = drvUuid[i];
            }
            /* A LUID is claimed so DXVK/DXGI interop code paths can take their
             * normal branch. There is no WDDM adapter behind it yet; this is a
             * placeholder until the render-only miniport exists (WDDM-PATH.md). */
            id->deviceLUID[0] = 0x89; id->deviceLUID[1] = 0x67;
            id->deviceLUID[2] = 0xB5; id->deviceLUID[3] = 0x13;
            id->deviceLUID[4] = 0x00; id->deviceLUID[5] = 0x01;
            id->deviceLUID[6] = 0x00; id->deviceLUID[7] = 0x00;
            id->deviceNodeMask = 1u;
            id->deviceLUIDValid = VK_TRUE;
        } else if (chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *drv = (VkPhysicalDeviceDriverProperties *)chain;
            static const char drvName[] = "Mali-G57 Jalur A";
            static const char drvInfo[] = "Mali-G57 MC2 Windows ARM64 UMD (windrv/umd/vulkan)";
            uint32_t i;
            drv->driverID = VK_DRIVER_ID_ARM_PROPRIETARY;
            for (i = 0; i < sizeof(drvName) && i < sizeof(drv->driverName); i++)
                drv->driverName[i] = drvName[i];
            for (i = 0; i < sizeof(drvInfo) && i < sizeof(drv->driverInfo); i++)
                drv->driverInfo[i] = drvInfo[i];
            drv->conformanceVersion.major = 1;
            drv->conformanceVersion.minor = 0;
            drv->conformanceVersion.subminor = 0;
            drv->conformanceVersion.patch = 0;
        } else if (chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
            VkPhysicalDeviceSubgroupProperties *sg = (VkPhysicalDeviceSubgroupProperties *)chain;
            sg->subgroupSize = 16;
            sg->supportedStages = VK_SHADER_STAGE_ALL;
            sg->supportedOperations = VK_SUBGROUP_FEATURE_BASIC_BIT;
            sg->quadOperationsInAllStages = VK_FALSE;
        }
        chain = chain->pNext;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2 *pProperties)
{
    vkGetPhysicalDeviceProperties2(physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures *pFeatures)
{
    (void)physicalDevice;
    MaliVkZero(pFeatures, sizeof(*pFeatures));
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures)
{
    if (!pFeatures) return;
    vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2 *pFeatures)
{
    vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
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

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice physicalDevice, uint32_t *pCount, VkQueueFamilyProperties2 *pProps)
{
    uint32_t i, n;
    if (!pCount) return;
    if (!pProps) { *pCount = 1; return; }
    n = *pCount;
    for (i = 0; i < n && i < 1u; i++) {
        uint32_t one = 1;
        MaliVkZero(&pProps[i], sizeof(pProps[i]));
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &one, &pProps[i].queueFamilyProperties);
    }
    if (n > 1u) *pCount = 1u;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(
    VkPhysicalDevice physicalDevice, uint32_t *pCount, VkQueueFamilyProperties2 *pProps)
{
    vkGetPhysicalDeviceQueueFamilyProperties2(physicalDevice, pCount, pProps);
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

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pProperties)
{
    if (!pProperties) return;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pProperties->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2KHR(
    VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties2 *pProperties)
{
    vkGetPhysicalDeviceMemoryProperties2(physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties *pFormatProperties)
{
    (void)physicalDevice;
    if (!pFormatProperties) return;
    MaliVkZero(pFormatProperties, sizeof(*pFormatProperties));
    if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM ||
        format == VK_FORMAT_R8G8B8A8_SRGB  || format == VK_FORMAT_B8G8R8A8_SRGB  ||
        format == VK_FORMAT_R8_UNORM) {
        pFormatProperties->linearTilingFeatures =
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        pFormatProperties->optimalTilingFeatures = pFormatProperties->linearTilingFeatures;
        pFormatProperties->bufferFeatures =
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2 *pFormatProperties)
{
    if (!pFormatProperties) return;
    vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &pFormatProperties->formatProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(
    VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2 *pFormatProperties)
{
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, pFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pProperties)
{
    (void)physicalDevice; (void)flags; (void)tiling; (void)usage;
    if (!pProperties) return VK_ERROR_INITIALIZATION_FAILED;
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM &&
        format != VK_FORMAT_R8G8B8A8_SRGB  && format != VK_FORMAT_B8G8R8A8_SRGB  &&
        format != VK_FORMAT_R8_UNORM) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    pProperties->maxExtent.width = 4096;
    pProperties->maxExtent.height = 4096;
    pProperties->maxExtent.depth = (type == VK_IMAGE_TYPE_3D) ? 256 : 1;
    pProperties->maxMipLevels = 12;
    pProperties->maxArrayLayers = 256;
    pProperties->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
    pProperties->maxResourceSize = 256ull * 1024ull * 1024ull;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties)
{
    if (!pImageFormatInfo || !pImageFormatProperties) return VK_ERROR_INITIALIZATION_FAILED;
    return vkGetPhysicalDeviceImageFormatProperties(
        physicalDevice, pImageFormatInfo->format, pImageFormatInfo->type,
        pImageFormatInfo->tiling, pImageFormatInfo->usage, pImageFormatInfo->flags,
        &pImageFormatProperties->imageFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties)
{
    return vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, pImageFormatInfo, pImageFormatProperties);
}

static const VkExtensionProperties g_DeviceExtensions[] = {
    { VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_GET_MEMORY_REQUIREMENTS_2_SPEC_VERSION },
    { VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,      VK_KHR_DEDICATED_ALLOCATION_SPEC_VERSION },
    { VK_KHR_MAINTENANCE1_EXTENSION_NAME,              VK_KHR_MAINTENANCE1_SPEC_VERSION },
    { VK_KHR_MAINTENANCE2_EXTENSION_NAME,              VK_KHR_MAINTENANCE2_SPEC_VERSION },
    { VK_KHR_MAINTENANCE3_EXTENSION_NAME,              VK_KHR_MAINTENANCE3_SPEC_VERSION },
    { VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,             VK_KHR_BIND_MEMORY_2_SPEC_VERSION },
    { VK_KHR_SWAPCHAIN_EXTENSION_NAME,                 VK_KHR_SWAPCHAIN_SPEC_VERSION },
};

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char *pLayerName,
    uint32_t *pCount, VkExtensionProperties *pProps)
{
    uint32_t total = (uint32_t)(sizeof(g_DeviceExtensions) / sizeof(g_DeviceExtensions[0]));
    uint32_t i;
    (void)physicalDevice;
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProps) { *pCount = total; return VK_SUCCESS; }
    if (*pCount > total) *pCount = total;
    for (i = 0; i < *pCount; i++) MaliVkCopyExtension(&pProps[i], &g_DeviceExtensions[i]);
    return (*pCount < total) ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t *pCount, VkLayerProperties *pProperties)
{
    (void)physicalDevice; (void)pProperties;
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

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements)
{
    if (!pInfo || !pMemoryRequirements) return;
    vkGetBufferMemoryRequirements(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2KHR(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements)
{
    vkGetBufferMemoryRequirements2(device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos)
{
    uint32_t i;
    if (!pBindInfos) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < bindInfoCount; i++) {
        VkResult r = vkBindBufferMemory(device, pBindInfos[i].buffer,
                                        pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2KHR(
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos)
{
    return vkBindBufferMemory2(device, bindInfoCount, pBindInfos);
}

/* ------------------------------------------------------------------ */
/* Images                                                              */
/* ------------------------------------------------------------------ */

uint32_t MaliVkFormatBytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM:          return 1;
    case VK_FORMAT_R8G8_UNORM:        return 2;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return 2;
    case VK_FORMAT_B5G6R5_UNORM_PACK16: return 2;
    default:                          return 4;
    }
}

uint64_t MaliVkAlign4096(uint64_t v)
{
    return (v + 4095ull) & ~4095ull;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkImage *img;
    uint64_t bytes;
    (void)pAllocator;

    if (!dev || !pCreateInfo || !pImage) return VK_ERROR_INITIALIZATION_FAILED;

    img = (MaliVkImage *)MaliVkAlloc(sizeof(*img));
    if (!img) return VK_ERROR_OUT_OF_HOST_MEMORY;

    img->Dev         = dev;
    img->Type        = pCreateInfo->imageType;
    img->Format      = pCreateInfo->format;
    img->Extent      = pCreateInfo->extent;
    img->MipLevels   = pCreateInfo->mipLevels ? pCreateInfo->mipLevels : 1;
    img->ArrayLayers = pCreateInfo->arrayLayers ? pCreateInfo->arrayLayers : 1;
    img->Samples     = pCreateInfo->samples;
    img->Tiling      = pCreateInfo->tiling;
    img->Usage       = pCreateInfo->usage;

    bytes = (uint64_t)img->Extent.width * (uint64_t)img->Extent.height *
            (uint64_t)img->Extent.depth * (uint64_t)MaliVkFormatBytes(img->Format) *
            (uint64_t)img->MipLevels * (uint64_t)img->ArrayLayers;
    img->Size = MaliVkAlign4096(bytes);

    *pImage = (VkImage)img;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)image);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements)
{
    MaliVkImage *img = (MaliVkImage *)image;
    (void)device;
    if (!pMemoryRequirements) return;

    MaliVkZero(pMemoryRequirements, sizeof(*pMemoryRequirements));
    pMemoryRequirements->size           = img ? img->Size : 4096;
    pMemoryRequirements->alignment      = 4096;
    pMemoryRequirements->memoryTypeBits = (1u << 0) | (1u << 1);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements)
{
    if (!pInfo || !pMemoryRequirements) return;
    vkGetImageMemoryRequirements(device, pInfo->image, &pMemoryRequirements->memoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2KHR(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
    VkMemoryRequirements2 *pMemoryRequirements)
{
    vkGetImageMemoryRequirements2(device, pInfo, pMemoryRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset)
{
    MaliVkImage *img = (MaliVkImage *)image;
    MaliVkMemory *mem = (MaliVkMemory *)memory;
    (void)device;

    if (!img || !mem) return VK_ERROR_INITIALIZATION_FAILED;
    if (memoryOffset + img->Size > mem->Bo.Size) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    img->Mem       = mem;
    img->MemOffset = memoryOffset;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(
    VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos)
{
    uint32_t i;
    if (!pBindInfos) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < bindInfoCount; i++) {
        VkResult r = vkBindImageMemory(device, pBindInfos[i].image,
                                       pBindInfos[i].memory, pBindInfos[i].memoryOffset);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2KHR(
    VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos)
{
    return vkBindImageMemory2(device, bindInfoCount, pBindInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(
    VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImageView *pView)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkImageView *view;
    (void)pAllocator;

    if (!dev || !pCreateInfo || !pView) return VK_ERROR_INITIALIZATION_FAILED;
    view = (MaliVkImageView *)MaliVkAlloc(sizeof(*view));
    if (!view) return VK_ERROR_OUT_OF_HOST_MEMORY;
    view->Dev      = dev;
    view->Image    = (MaliVkImage *)pCreateInfo->image;
    view->ViewType = pCreateInfo->viewType;
    view->Format   = pCreateInfo->format;
    *pView = (VkImageView)view;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(
    VkDevice device, VkImageView imageView, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)imageView);
}

VKAPI_ATTR void VKAPI_CALL vkGetImageSubresourceLayout(
    VkDevice device, VkImage image, const VkImageSubresource *pSubresource,
    VkSubresourceLayout *pLayout)
{
    MaliVkImage *img = (MaliVkImage *)image;
    (void)device; (void)pSubresource;
    if (!pLayout) return;
    MaliVkZero(pLayout, sizeof(*pLayout));
    pLayout->rowPitch   = img ? (uint64_t)img->Extent.width * MaliVkFormatBytes(img->Format) : 0;
    pLayout->arrayPitch = img ? pLayout->rowPitch * (uint64_t)img->Extent.height : 0;
    pLayout->depthPitch = img ? pLayout->arrayPitch * (uint64_t)img->Extent.depth : 0;
    pLayout->size       = img ? img->Size : 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBufferView(
    VkDevice device, const VkBufferViewCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    if (pView) *pView = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBufferView(
    VkDevice device, VkBufferView bufferView, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)bufferView; (void)pAllocator;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
    uint32_t *pPropertyCount, VkSparseImageFormatProperties2 *pProperties)
{
    (void)physicalDevice; (void)pFormatInfo; (void)pProperties;
    if (pPropertyCount) *pPropertyCount = 0;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2KHR(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
    uint32_t *pPropertyCount, VkSparseImageFormatProperties2 *pProperties)
{
    vkGetPhysicalDeviceSparseImageFormatProperties2(physicalDevice, pFormatInfo, pPropertyCount, pProperties);
}

/* ------------------------------------------------------------------ */
/* Shader modules and pipeline/descriptor objects                      */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkShaderModule *m;
    (void)pAllocator;

    if (!dev || !pCreateInfo || !pShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    m = (MaliVkShaderModule *)MaliVkAlloc(sizeof(*m));
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->Dev      = dev;
    m->CodeSize = (uint32_t)pCreateInfo->codeSize;
    m->Code     = (uint32_t *)MaliVkAlloc(pCreateInfo->codeSize ? pCreateInfo->codeSize : 4u);
    if (!m->Code) { MaliVkFree(m); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (pCreateInfo->pCode) {
        const unsigned char *s = (const unsigned char *)pCreateInfo->pCode;
        unsigned char *d = (unsigned char *)m->Code;
        uint64_t n = pCreateInfo->codeSize;
        while (n--) *d++ = *s++;
    }
    *pShaderModule = (VkShaderModule)m;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
    VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator)
{
    MaliVkShaderModule *m = (MaliVkShaderModule *)shaderModule;
    (void)device; (void)pAllocator;
    if (!m) return;
    MaliVkFree(m->Code);
    MaliVkFree(m);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkDescriptorSetLayout *l;
    (void)pCreateInfo; (void)pAllocator;
    if (!dev || !pSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    l = (MaliVkDescriptorSetLayout *)MaliVkAlloc(sizeof(*l));
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->Dev = dev;
    *pSetLayout = (VkDescriptorSetLayout)l;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)descriptorSetLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkPipelineLayout *l;
    (void)pCreateInfo; (void)pAllocator;
    if (!dev || !pPipelineLayout) return VK_ERROR_INITIALIZATION_FAILED;
    l = (MaliVkPipelineLayout *)MaliVkAlloc(sizeof(*l));
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->Dev = dev;
    *pPipelineLayout = (VkPipelineLayout)l;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)pipelineLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkDescriptorPool *p;
    (void)pCreateInfo; (void)pAllocator;
    if (!dev || !pDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    p = (MaliVkDescriptorPool *)MaliVkAlloc(sizeof(*p));
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->Dev = dev;
    *pDescriptorPool = (VkDescriptorPool)p;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)descriptorPool);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags)
{
    (void)device; (void)descriptorPool; (void)flags;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo,
    VkDescriptorSet *pDescriptorSets)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    uint32_t i;
    if (!dev || !pAllocateInfo || !pDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        MaliVkDescriptorSet *s = (MaliVkDescriptorSet *)MaliVkAlloc(sizeof(*s));
        if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
        s->Dev = dev;
        pDescriptorSets[i] = (VkDescriptorSet)s;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(
    VkDevice device, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount,
    const VkDescriptorSet *pDescriptorSets)
{
    uint32_t i;
    (void)device; (void)descriptorPool;
    for (i = 0; i < descriptorSetCount; i++) MaliVkFree((void *)pDescriptorSets[i]);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites,
    uint32_t descriptorCopyCount, const VkCopyDescriptorSet *pDescriptorCopies)
{
    (void)device; (void)descriptorWriteCount; (void)pDescriptorWrites;
    (void)descriptorCopyCount; (void)pDescriptorCopies;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(
    VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkRenderPass *r;
    (void)pCreateInfo; (void)pAllocator;
    if (!dev || !pRenderPass) return VK_ERROR_INITIALIZATION_FAILED;
    r = (MaliVkRenderPass *)MaliVkAlloc(sizeof(*r));
    if (!r) return VK_ERROR_OUT_OF_HOST_MEMORY;
    r->Dev = dev;
    *pRenderPass = (VkRenderPass)r;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(
    VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)renderPass);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkFramebuffer *f;
    (void)pCreateInfo; (void)pAllocator;
    if (!dev || !pFramebuffer) return VK_ERROR_INITIALIZATION_FAILED;
    f = (MaliVkFramebuffer *)MaliVkAlloc(sizeof(*f));
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    f->Dev = dev;
    *pFramebuffer = (VkFramebuffer)f;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(
    VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)framebuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    uint32_t i;
    (void)pipelineCache; (void)pCreateInfos; (void)pAllocator;
    if (!dev || !pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < createInfoCount; i++) {
        MaliVkPipeline *p = (MaliVkPipeline *)MaliVkAlloc(sizeof(*p));
        if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
        p->Dev = dev;
        p->BindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        pPipelines[i] = (VkPipeline)p;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    uint32_t i;
    (void)pipelineCache; (void)pCreateInfos; (void)pAllocator;
    if (!dev || !pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (i = 0; i < createInfoCount; i++) {
        MaliVkPipeline *p = (MaliVkPipeline *)MaliVkAlloc(sizeof(*p));
        if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
        p->Dev = dev;
        p->BindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
        pPipelines[i] = (VkPipeline)p;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
    VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)pipeline);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(
    VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSampler *pSampler)
{
    MaliVkDevice *dev = (MaliVkDevice *)device;
    MaliVkSampler *s;
    (void)pCreateInfo; (void)pAllocator;
    if (!dev || !pSampler) return VK_ERROR_INITIALIZATION_FAILED;
    s = (MaliVkSampler *)MaliVkAlloc(sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->Dev = dev;
    *pSampler = (VkSampler)s;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(
    VkDevice device, VkSampler sampler, const VkAllocationCallbacks *pAllocator)
{
    (void)device; (void)pAllocator;
    MaliVkFree((void *)sampler);
}
