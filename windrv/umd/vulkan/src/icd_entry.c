/*
 * icd_entry.c  -  Vulkan loader/ICD interface for the Mali-G57 ICD.
 *
 * Implements the Khronos loader contract (vk_icd.h) and resolves the Vulkan
 * entry points this ICD provides. We support loader/ICD interface version 2
 * (negotiation + vk_icdGetInstanceProcAddr); the loader builds the dispatch it
 * needs from these functions.
 *
 * Everything we implement is reachable through both vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr, which is the simple, well-understood pattern for a small
 * ICD.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vk_icd.h>
#include "mali_vk.h"

/* ---------------- entry points (defined in icd_device.c / icd_queue.c) ---- */

extern VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *);
extern VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *, uint32_t *, VkExtensionProperties *);
extern VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *, VkLayerProperties *);
extern VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
extern void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);
extern void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
extern void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);
extern void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *);
extern void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
extern void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat, VkFormatProperties *);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkImageTiling, VkImageUsageFlags, VkImageCreateFlags, VkImageFormatProperties *);
extern VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
extern void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice, VkFormat, VkImageType, VkSampleCountFlagBits, VkImageUsageFlags, VkImageTiling, uint32_t *, VkSparseImageFormatProperties *);
extern VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
extern void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks *);
extern void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
extern VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice);
extern VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue);
extern VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *);
extern void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
extern void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory);
extern VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange *);
extern VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice, uint32_t, const VkMappedMemoryRange *);
extern void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice, VkDeviceMemory, VkDeviceSize *);
extern VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
extern void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
extern void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
extern VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
extern VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *);
extern void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags);
extern VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
extern void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
extern VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
extern VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer);
extern VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);
extern void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t);
extern void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *);
extern void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *);
extern VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *);
extern void VKAPI_CALL vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t, const VkFence *);
extern VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence);
extern VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
extern VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);

/* ---------------- dispatch table ---------------------------------------- */

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char *);

typedef struct MaliVkEntry {
    const char         *Name;
    PFN_vkVoidFunction  Fn;
} MaliVkEntry;

static const MaliVkEntry g_Entries[] = {
    { "vkGetInstanceProcAddr",                  (PFN_vkVoidFunction)vkGetInstanceProcAddr },
    { "vkGetDeviceProcAddr",                    (PFN_vkVoidFunction)vkGetDeviceProcAddr },
    { "vkEnumerateInstanceVersion",             (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    { "vkEnumerateInstanceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties },
    { "vkEnumerateInstanceLayerProperties",     (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties },
    { "vkCreateInstance",                       (PFN_vkVoidFunction)vkCreateInstance },
    { "vkDestroyInstance",                      (PFN_vkVoidFunction)vkDestroyInstance },
    { "vkEnumeratePhysicalDevices",             (PFN_vkVoidFunction)vkEnumeratePhysicalDevices },
    { "vkGetPhysicalDeviceProperties",          (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties },
    { "vkGetPhysicalDeviceFeatures",            (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures },
    { "vkGetPhysicalDeviceQueueFamilyProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties },
    { "vkGetPhysicalDeviceMemoryProperties",    (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties },
    { "vkGetPhysicalDeviceFormatProperties",    (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties },
    { "vkGetPhysicalDeviceImageFormatProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties },
    { "vkGetPhysicalDeviceSparseImageFormatProperties", (PFN_vkVoidFunction)vkGetPhysicalDeviceSparseImageFormatProperties },
    { "vkEnumerateDeviceExtensionProperties",   (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties },
    { "vkCreateDevice",                         (PFN_vkVoidFunction)vkCreateDevice },
    { "vkDestroyDevice",                        (PFN_vkVoidFunction)vkDestroyDevice },
    { "vkGetDeviceQueue",                       (PFN_vkVoidFunction)vkGetDeviceQueue },
    { "vkDeviceWaitIdle",                       (PFN_vkVoidFunction)vkDeviceWaitIdle },
    { "vkQueueWaitIdle",                        (PFN_vkVoidFunction)vkQueueWaitIdle },
    { "vkAllocateMemory",                       (PFN_vkVoidFunction)vkAllocateMemory },
    { "vkFreeMemory",                           (PFN_vkVoidFunction)vkFreeMemory },
    { "vkMapMemory",                            (PFN_vkVoidFunction)vkMapMemory },
    { "vkUnmapMemory",                          (PFN_vkVoidFunction)vkUnmapMemory },
    { "vkFlushMappedMemoryRanges",              (PFN_vkVoidFunction)vkFlushMappedMemoryRanges },
    { "vkInvalidateMappedMemoryRanges",         (PFN_vkVoidFunction)vkInvalidateMappedMemoryRanges },
    { "vkGetDeviceMemoryCommitment",            (PFN_vkVoidFunction)vkGetDeviceMemoryCommitment },
    { "vkCreateBuffer",                         (PFN_vkVoidFunction)vkCreateBuffer },
    { "vkDestroyBuffer",                        (PFN_vkVoidFunction)vkDestroyBuffer },
    { "vkGetBufferMemoryRequirements",          (PFN_vkVoidFunction)vkGetBufferMemoryRequirements },
    { "vkBindBufferMemory",                     (PFN_vkVoidFunction)vkBindBufferMemory },
    { "vkCreateCommandPool",                    (PFN_vkVoidFunction)vkCreateCommandPool },
    { "vkDestroyCommandPool",                   (PFN_vkVoidFunction)vkDestroyCommandPool },
    { "vkResetCommandPool",                     (PFN_vkVoidFunction)vkResetCommandPool },
    { "vkAllocateCommandBuffers",               (PFN_vkVoidFunction)vkAllocateCommandBuffers },
    { "vkFreeCommandBuffers",                   (PFN_vkVoidFunction)vkFreeCommandBuffers },
    { "vkBeginCommandBuffer",                   (PFN_vkVoidFunction)vkBeginCommandBuffer },
    { "vkEndCommandBuffer",                     (PFN_vkVoidFunction)vkEndCommandBuffer },
    { "vkResetCommandBuffer",                   (PFN_vkVoidFunction)vkResetCommandBuffer },
    { "vkCmdFillBuffer",                        (PFN_vkVoidFunction)vkCmdFillBuffer },
    { "vkCmdCopyBuffer",                        (PFN_vkVoidFunction)vkCmdCopyBuffer },
    { "vkCmdPipelineBarrier",                   (PFN_vkVoidFunction)vkCmdPipelineBarrier },
    { "vkCreateFence",                          (PFN_vkVoidFunction)vkCreateFence },
    { "vkDestroyFence",                         (PFN_vkVoidFunction)vkDestroyFence },
    { "vkResetFences",                          (PFN_vkVoidFunction)vkResetFences },
    { "vkGetFenceStatus",                       (PFN_vkVoidFunction)vkGetFenceStatus },
    { "vkWaitForFences",                        (PFN_vkVoidFunction)vkWaitForFences },
    { "vkQueueSubmit",                          (PFN_vkVoidFunction)vkQueueSubmit },
    { 0, 0 }
};

static PFN_vkVoidFunction MaliVkLookup(const char *name)
{
    int i;
    if (!name) return 0;
    for (i = 0; g_Entries[i].Name; i++) {
        if (MaliVkStrEq(g_Entries[i].Name, name)) return g_Entries[i].Fn;
    }
    return 0;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    (void)instance;
    return MaliVkLookup(pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
    (void)device;
    return MaliVkLookup(pName);
}

/* ---------------- loader/ICD interface ---------------------------------- */

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion)
{
    if (!pVersion) return VK_ERROR_INITIALIZATION_FAILED;
    /* We implement the interface through version 2 (negotiation +
     * vk_icdGetInstanceProcAddr); clamp the loader's request. */
    if (*pVersion > 2u) *pVersion = 2u;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
    (void)instance;
    if (MaliVkStrEq(pName, "vk_icdNegotiateLoaderICDInterfaceVersion"))
        return (PFN_vkVoidFunction)vk_icdNegotiateLoaderICDInterfaceVersion;
    if (MaliVkStrEq(pName, "vk_icdGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    if (MaliVkStrEq(pName, "vk_icdGetPhysicalDeviceProcAddr"))
        return (PFN_vkVoidFunction)vk_icdGetPhysicalDeviceProcAddr;
    return MaliVkLookup(pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName)
{
    (void)instance;
    return MaliVkLookup(pName);
}

/* ---------------- DLL plumbing ------------------------------------------ */

BOOL WINAPI DllMain(HINSTANCE hinstDll, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hinstDll; (void)fdwReason; (void)lpvReserved;
    return TRUE;
}
