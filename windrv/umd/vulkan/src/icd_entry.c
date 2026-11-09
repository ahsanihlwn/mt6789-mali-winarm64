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

/* Vulkan 1.1 core + KHR-get-properties2 surface. */
extern void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2KHR(VkPhysicalDevice, VkFormat, VkFormatProperties2 *);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2KHR(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *, VkSparseImageFormatProperties2 *);
extern void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties2KHR(VkPhysicalDevice, const VkPhysicalDeviceSparseImageFormatInfo2 *, uint32_t *, VkSparseImageFormatProperties2 *);
extern VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t *, VkLayerProperties *);
extern void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice, const VkBufferMemoryRequirementsInfo2 *, VkMemoryRequirements2 *);
extern void VKAPI_CALL vkGetBufferMemoryRequirements2KHR(VkDevice, const VkBufferMemoryRequirementsInfo2 *, VkMemoryRequirements2 *);
extern VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice, uint32_t, const VkBindBufferMemoryInfo *);
extern VkResult VKAPI_CALL vkBindBufferMemory2KHR(VkDevice, uint32_t, const VkBindBufferMemoryInfo *);

/* Images and views. */
extern VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);
extern void VKAPI_CALL vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
extern void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *);
extern void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice, const VkImageMemoryRequirementsInfo2 *, VkMemoryRequirements2 *);
extern void VKAPI_CALL vkGetImageMemoryRequirements2KHR(VkDevice, const VkImageMemoryRequirementsInfo2 *, VkMemoryRequirements2 *);
extern VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
extern VkResult VKAPI_CALL vkBindImageMemory2(VkDevice, uint32_t, const VkBindImageMemoryInfo *);
extern VkResult VKAPI_CALL vkBindImageMemory2KHR(VkDevice, uint32_t, const VkBindImageMemoryInfo *);
extern VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *);
extern void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
extern void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice, VkImage, const VkImageSubresource *, VkSubresourceLayout *);
extern VkResult VKAPI_CALL vkCreateBufferView(VkDevice, const VkBufferViewCreateInfo *, const VkAllocationCallbacks *, VkBufferView *);
extern void VKAPI_CALL vkDestroyBufferView(VkDevice, VkBufferView, const VkAllocationCallbacks *);

/* Pipelines and descriptors. */
extern VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *, VkShaderModule *);
extern void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const VkAllocationCallbacks *, VkDescriptorSetLayout *);
extern void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *, VkPipelineLayout *);
extern void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *, const VkAllocationCallbacks *, VkDescriptorPool *);
extern void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags);
extern VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
extern VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet *);
extern void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const VkCopyDescriptorSet *);
extern VkResult VKAPI_CALL vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *, const VkAllocationCallbacks *, VkRenderPass *);
extern void VKAPI_CALL vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *, VkFramebuffer *);
extern void VKAPI_CALL vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
extern VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *, VkPipeline *);
extern void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreateSampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *);
extern void VKAPI_CALL vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *);

/* Extra command recording (host-executed) and draw/dispatch no-ops. */
extern void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void *);
extern void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy *);
extern void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *);
extern void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *);
extern void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearDepthStencilValue *, uint32_t, const VkImageSubresourceRange *);
extern void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer, uint32_t, const VkClearAttachment *, uint32_t, const VkClearRect *);
extern void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer, uint32_t, const VkCommandBuffer *);
extern void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
extern void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport *);
extern void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *);
extern void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer, float);
extern void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet *, uint32_t, const uint32_t *);
extern void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
extern void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
extern void VKAPI_CALL vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
extern void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
extern void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
extern void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
extern void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer);

/* Semaphores and events. */
extern VkResult VKAPI_CALL vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *);
extern void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkCreateEvent(VkDevice, const VkEventCreateInfo *, const VkAllocationCallbacks *, VkEvent *);
extern void VKAPI_CALL vkDestroyEvent(VkDevice, VkEvent, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkGetEventStatus(VkDevice, VkEvent);
extern VkResult VKAPI_CALL vkSetEvent(VkDevice, VkEvent);
extern VkResult VKAPI_CALL vkResetEvent(VkDevice, VkEvent);
extern void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags);
extern void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags);
extern void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer, uint32_t, const VkEvent *, VkPipelineStageFlags, VkPipelineStageFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *);

/* WSI (icd_wsi.c) */
extern VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);
extern void VKAPI_CALL vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *);
extern VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice, uint32_t);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
extern VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *);
extern VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *);
extern void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
extern VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
extern VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
extern VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);

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
    { "vkGetPhysicalDeviceProperties2",         (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2 },
    { "vkGetPhysicalDeviceProperties2KHR",      (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2KHR },
    { "vkGetPhysicalDeviceFeatures2",           (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2 },
    { "vkGetPhysicalDeviceFeatures2KHR",        (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2KHR },
    { "vkGetPhysicalDeviceQueueFamilyProperties2",    (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties2 },
    { "vkGetPhysicalDeviceQueueFamilyProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties2KHR },
    { "vkGetPhysicalDeviceMemoryProperties2",   (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2 },
    { "vkGetPhysicalDeviceMemoryProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2KHR },
    { "vkGetPhysicalDeviceFormatProperties2",   (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2 },
    { "vkGetPhysicalDeviceFormatProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2KHR },
    { "vkGetPhysicalDeviceImageFormatProperties2",    (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2 },
    { "vkGetPhysicalDeviceImageFormatProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties2KHR },
    { "vkGetPhysicalDeviceSparseImageFormatProperties2",    (PFN_vkVoidFunction)vkGetPhysicalDeviceSparseImageFormatProperties2 },
    { "vkGetPhysicalDeviceSparseImageFormatProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSparseImageFormatProperties2KHR },
    { "vkEnumerateDeviceLayerProperties",       (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties },
    { "vkGetBufferMemoryRequirements2",         (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2 },
    { "vkGetBufferMemoryRequirements2KHR",      (PFN_vkVoidFunction)vkGetBufferMemoryRequirements2KHR },
    { "vkBindBufferMemory2",                    (PFN_vkVoidFunction)vkBindBufferMemory2 },
    { "vkBindBufferMemory2KHR",                 (PFN_vkVoidFunction)vkBindBufferMemory2KHR },
    { "vkCreateImage",                          (PFN_vkVoidFunction)vkCreateImage },
    { "vkDestroyImage",                         (PFN_vkVoidFunction)vkDestroyImage },
    { "vkGetImageMemoryRequirements",           (PFN_vkVoidFunction)vkGetImageMemoryRequirements },
    { "vkGetImageMemoryRequirements2",          (PFN_vkVoidFunction)vkGetImageMemoryRequirements2 },
    { "vkGetImageMemoryRequirements2KHR",       (PFN_vkVoidFunction)vkGetImageMemoryRequirements2KHR },
    { "vkBindImageMemory",                      (PFN_vkVoidFunction)vkBindImageMemory },
    { "vkBindImageMemory2",                     (PFN_vkVoidFunction)vkBindImageMemory2 },
    { "vkBindImageMemory2KHR",                  (PFN_vkVoidFunction)vkBindImageMemory2KHR },
    { "vkCreateImageView",                      (PFN_vkVoidFunction)vkCreateImageView },
    { "vkDestroyImageView",                     (PFN_vkVoidFunction)vkDestroyImageView },
    { "vkGetImageSubresourceLayout",            (PFN_vkVoidFunction)vkGetImageSubresourceLayout },
    { "vkCreateBufferView",                     (PFN_vkVoidFunction)vkCreateBufferView },
    { "vkDestroyBufferView",                    (PFN_vkVoidFunction)vkDestroyBufferView },
    { "vkCreateShaderModule",                   (PFN_vkVoidFunction)vkCreateShaderModule },
    { "vkDestroyShaderModule",                  (PFN_vkVoidFunction)vkDestroyShaderModule },
    { "vkCreateDescriptorSetLayout",            (PFN_vkVoidFunction)vkCreateDescriptorSetLayout },
    { "vkDestroyDescriptorSetLayout",           (PFN_vkVoidFunction)vkDestroyDescriptorSetLayout },
    { "vkCreatePipelineLayout",                 (PFN_vkVoidFunction)vkCreatePipelineLayout },
    { "vkDestroyPipelineLayout",                (PFN_vkVoidFunction)vkDestroyPipelineLayout },
    { "vkCreateDescriptorPool",                 (PFN_vkVoidFunction)vkCreateDescriptorPool },
    { "vkDestroyDescriptorPool",                (PFN_vkVoidFunction)vkDestroyDescriptorPool },
    { "vkResetDescriptorPool",                  (PFN_vkVoidFunction)vkResetDescriptorPool },
    { "vkAllocateDescriptorSets",               (PFN_vkVoidFunction)vkAllocateDescriptorSets },
    { "vkFreeDescriptorSets",                   (PFN_vkVoidFunction)vkFreeDescriptorSets },
    { "vkUpdateDescriptorSets",                 (PFN_vkVoidFunction)vkUpdateDescriptorSets },
    { "vkCreateRenderPass",                     (PFN_vkVoidFunction)vkCreateRenderPass },
    { "vkDestroyRenderPass",                    (PFN_vkVoidFunction)vkDestroyRenderPass },
    { "vkCreateFramebuffer",                    (PFN_vkVoidFunction)vkCreateFramebuffer },
    { "vkDestroyFramebuffer",                   (PFN_vkVoidFunction)vkDestroyFramebuffer },
    { "vkCreateGraphicsPipelines",              (PFN_vkVoidFunction)vkCreateGraphicsPipelines },
    { "vkCreateComputePipelines",               (PFN_vkVoidFunction)vkCreateComputePipelines },
    { "vkDestroyPipeline",                      (PFN_vkVoidFunction)vkDestroyPipeline },
    { "vkCreateSampler",                        (PFN_vkVoidFunction)vkCreateSampler },
    { "vkDestroySampler",                       (PFN_vkVoidFunction)vkDestroySampler },
    { "vkCmdUpdateBuffer",                      (PFN_vkVoidFunction)vkCmdUpdateBuffer },
    { "vkCmdCopyBufferToImage",                 (PFN_vkVoidFunction)vkCmdCopyBufferToImage },
    { "vkCmdCopyImageToBuffer",                 (PFN_vkVoidFunction)vkCmdCopyImageToBuffer },
    { "vkCmdClearColorImage",                   (PFN_vkVoidFunction)vkCmdClearColorImage },
    { "vkCmdClearDepthStencilImage",            (PFN_vkVoidFunction)vkCmdClearDepthStencilImage },
    { "vkCmdClearAttachments",                  (PFN_vkVoidFunction)vkCmdClearAttachments },
    { "vkCmdExecuteCommands",                   (PFN_vkVoidFunction)vkCmdExecuteCommands },
    { "vkCmdBindPipeline",                      (PFN_vkVoidFunction)vkCmdBindPipeline },
    { "vkCmdSetViewport",                       (PFN_vkVoidFunction)vkCmdSetViewport },
    { "vkCmdSetScissor",                        (PFN_vkVoidFunction)vkCmdSetScissor },
    { "vkCmdSetLineWidth",                      (PFN_vkVoidFunction)vkCmdSetLineWidth },
    { "vkCmdBindDescriptorSets",                (PFN_vkVoidFunction)vkCmdBindDescriptorSets },
    { "vkCmdBindIndexBuffer",                   (PFN_vkVoidFunction)vkCmdBindIndexBuffer },
    { "vkCmdBindVertexBuffers",                 (PFN_vkVoidFunction)vkCmdBindVertexBuffers },
    { "vkCmdDraw",                              (PFN_vkVoidFunction)vkCmdDraw },
    { "vkCmdDrawIndexed",                       (PFN_vkVoidFunction)vkCmdDrawIndexed },
    { "vkCmdDispatch",                          (PFN_vkVoidFunction)vkCmdDispatch },
    { "vkCmdBeginRenderPass",                   (PFN_vkVoidFunction)vkCmdBeginRenderPass },
    { "vkCmdEndRenderPass",                     (PFN_vkVoidFunction)vkCmdEndRenderPass },
    { "vkCreateSemaphore",                      (PFN_vkVoidFunction)vkCreateSemaphore },
    { "vkDestroySemaphore",                     (PFN_vkVoidFunction)vkDestroySemaphore },
    { "vkCreateEvent",                          (PFN_vkVoidFunction)vkCreateEvent },
    { "vkDestroyEvent",                         (PFN_vkVoidFunction)vkDestroyEvent },
    { "vkGetEventStatus",                       (PFN_vkVoidFunction)vkGetEventStatus },
    { "vkSetEvent",                             (PFN_vkVoidFunction)vkSetEvent },
    { "vkResetEvent",                           (PFN_vkVoidFunction)vkResetEvent },
    { "vkCmdSetEvent",                          (PFN_vkVoidFunction)vkCmdSetEvent },
    { "vkCmdResetEvent",                        (PFN_vkVoidFunction)vkCmdResetEvent },
    { "vkCmdWaitEvents",                        (PFN_vkVoidFunction)vkCmdWaitEvents },

    /* WSI */
    { "vkCreateWin32SurfaceKHR",                (PFN_vkVoidFunction)vkCreateWin32SurfaceKHR },
    { "vkDestroySurfaceKHR",                    (PFN_vkVoidFunction)vkDestroySurfaceKHR },
    { "vkGetPhysicalDeviceWin32PresentationSupportKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceWin32PresentationSupportKHR },
    { "vkGetPhysicalDeviceSurfaceSupportKHR",   (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceSupportKHR },
    { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR },
    { "vkGetPhysicalDeviceSurfaceFormatsKHR",   (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceFormatsKHR },
    { "vkGetPhysicalDeviceSurfacePresentModesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfacePresentModesKHR },
    { "vkCreateSwapchainKHR",                   (PFN_vkVoidFunction)vkCreateSwapchainKHR },
    { "vkDestroySwapchainKHR",                  (PFN_vkVoidFunction)vkDestroySwapchainKHR },
    { "vkGetSwapchainImagesKHR",                (PFN_vkVoidFunction)vkGetSwapchainImagesKHR },
    { "vkAcquireNextImageKHR",                  (PFN_vkVoidFunction)vkAcquireNextImageKHR },
    { "vkQueuePresentKHR",                      (PFN_vkVoidFunction)vkQueuePresentKHR },
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
