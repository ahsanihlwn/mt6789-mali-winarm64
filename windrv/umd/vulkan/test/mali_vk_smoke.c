/*
 * mali_vk_smoke.c  -  End-to-end smoke test for the Mali-G57 Vulkan ICD.
 *
 * This links the ICD's import library and resolves every entry point through
 * vkGetInstanceProcAddr, so it exercises the ICD directly (no Khronos loader
 * needed). It walks the Jalur A definition of done from GRAPHICS-STACK.md:
 *
 *   V1  instance -> physical device ("Mali-G57 MC2") -> device + queue
 *   V2  allocate memory -> buffer -> bind -> map -> CPU write/read
 *   V3  vkCmdFillBuffer (zero) -> vkQueueSubmit -> fence -> GPU wrote zeros
 *
 * A non-zero fill is also checked, which the ICD performs on the host.
 *
 * CRT-free, same style as the other tools: own entry point, /nodefaultlib.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

/* Supplied by the ICD import library (mali_vk_icd.lib). */
extern PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);

static HANDLE g_out;
static int    g_fail;

static void Out(const char *s) { DWORD n; WriteFile(g_out, s, (DWORD)lstrlenA(s), &n, NULL); }
static void OutU(unsigned v, unsigned base, int width)
{
    char b[24]; const char *h = "0123456789ABCDEF"; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v) { b[i++] = h[v % base]; v /= base; }
    while (i < width) b[i++] = '0';
    while (i) { DWORD n; WriteFile(g_out, &b[--i], 1, &n, NULL); }
}
static void OutHex(unsigned v) { Out("0x"); OutU(v, 16, 8); }
static void OutDec(unsigned v) { OutU(v, 10, 0); }
static void Step(const char *s) { Out(s); Out("\r\n"); }
static void Fail(const char *s) { Out("  FAIL: "); Out(s); Out("\r\n"); g_fail = 1; }

typedef struct Api {
    PFN_vkCreateInstance                 CreateInstance;
    PFN_vkDestroyInstance                DestroyInstance;
    PFN_vkEnumeratePhysicalDevices       EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties    GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetQueueFamilyProperties;
    PFN_vkCreateDevice                   CreateDevice;
    PFN_vkDestroyDevice                  DestroyDevice;
    PFN_vkGetDeviceQueue                 GetDeviceQueue;
    PFN_vkDeviceWaitIdle                 DeviceWaitIdle;
    PFN_vkAllocateMemory                 AllocateMemory;
    PFN_vkFreeMemory                     FreeMemory;
    PFN_vkMapMemory                      MapMemory;
    PFN_vkUnmapMemory                    UnmapMemory;
    PFN_vkCreateBuffer                   CreateBuffer;
    PFN_vkDestroyBuffer                  DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements    GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory               BindBufferMemory;
    PFN_vkCreateCommandPool              CreateCommandPool;
    PFN_vkDestroyCommandPool             DestroyCommandPool;
    PFN_vkAllocateCommandBuffers         AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers             FreeCommandBuffers;
    PFN_vkBeginCommandBuffer             BeginCommandBuffer;
    PFN_vkEndCommandBuffer               EndCommandBuffer;
    PFN_vkCmdFillBuffer                  CmdFillBuffer;
    PFN_vkCreateFence                    CreateFence;
    PFN_vkDestroyFence                   DestroyFence;
    PFN_vkWaitForFences                  WaitForFences;
    PFN_vkQueueSubmit                    QueueSubmit;
} Api;

#define GET(api, inst, field, name) \
    do { (api)->field = (PFN_##name)vkGetInstanceProcAddr((inst), #name); \
         if (!(api)->field) { Fail("missing entry point " #name); return 1; } } while (0)

/* ------------------------------------------------------------------ */
/* Device-object coverage: images, semaphores, events, shader modules, */
/* pipelines and the transfer commands that move data between them.    */
/* None of this needs a shader compiler; it proves the object model    */
/* and the transfer paths that vkd3d-proton/DXVK will lean on first.   */
/* ------------------------------------------------------------------ */

static int MakeBuffer(VkInstance instance, VkDevice device, uint32_t memType, uint64_t size,
                      VkBuffer *outBuf, VkDeviceMemory *outMem, void **outMap)
{
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkGetBufferMemoryRequirements GetReq;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkMapMemory MapMemory;
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkResult r;
    size_t i;

    CreateBuffer       = (PFN_vkCreateBuffer)vkGetInstanceProcAddr(instance, "vkCreateBuffer");
    GetReq             = (PFN_vkGetBufferMemoryRequirements)vkGetInstanceProcAddr(instance, "vkGetBufferMemoryRequirements");
    AllocateMemory     = (PFN_vkAllocateMemory)vkGetInstanceProcAddr(instance, "vkAllocateMemory");
    BindBufferMemory   = (PFN_vkBindBufferMemory)vkGetInstanceProcAddr(instance, "vkBindBufferMemory");
    MapMemory          = (PFN_vkMapMemory)vkGetInstanceProcAddr(instance, "vkMapMemory");
    if (!CreateBuffer || !GetReq || !AllocateMemory || !BindBufferMemory || !MapMemory) return 1;

    for (i = 0; i < sizeof(bci); i++) ((unsigned char *)&bci)[i] = 0;
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = CreateBuffer(device, &bci, NULL, outBuf);
    if (r != VK_SUCCESS) { Fail("vkCreateBuffer (object test)"); return 1; }

    for (i = 0; i < sizeof(req); i++) ((unsigned char *)&req)[i] = 0;
    GetReq(device, *outBuf, &req);

    for (i = 0; i < sizeof(mai); i++) ((unsigned char *)&mai)[i] = 0;
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size < size ? size : req.size;
    mai.memoryTypeIndex = memType;
    r = AllocateMemory(device, &mai, NULL, outMem);
    if (r != VK_SUCCESS) { Fail("vkAllocateMemory (object test)"); return 1; }

    r = BindBufferMemory(device, *outBuf, *outMem, 0);
    if (r != VK_SUCCESS) { Fail("vkBindBufferMemory (object test)"); return 1; }

    return MapMemory(device, *outMem, 0, VK_WHOLE_SIZE, 0, outMap) == VK_SUCCESS ? 0 : 1;
}

static int RunDeviceObjects(VkInstance instance, VkDevice device, VkQueue queue, uint32_t memType)
{
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageReq;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdUpdateBuffer CmdUpdateBuffer;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    PFN_vkCreateEvent CreateEvent;
    PFN_vkDestroyEvent DestroyEvent;
    PFN_vkGetEventStatus GetEventStatus;
    PFN_vkSetEvent SetEvent;
    PFN_vkResetEvent ResetEvent;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets FreeDescriptorSets;
    PFN_vkCreateComputePipelines CreateComputePipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;

    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkBuffer srcBuf = VK_NULL_HANDLE, dstBuf = VK_NULL_HANDLE;
    VkDeviceMemory srcMem = VK_NULL_HANDLE, dstMem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkEvent ev = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkImageCreateInfo ici;
    VkMemoryRequirements ireq;
    VkMemoryAllocateInfo mai;
    VkImageViewCreateInfo vci;
    VkCommandPoolCreateInfo pci;
    VkCommandBufferAllocateInfo cbai;
    VkCommandBufferBeginInfo cbbi;
    VkSubmitInfo si;
    VkFenceCreateInfo fci;
    VkSemaphoreCreateInfo sci;
    VkEventCreateInfo eci;
    VkShaderModuleCreateInfo smci;
    VkDescriptorSetLayoutCreateInfo dslci;
    VkPipelineLayoutCreateInfo plci;
    VkDescriptorPoolSize dps;
    VkDescriptorPoolCreateInfo dpci;
    VkDescriptorSetAllocateInfo dsai;
    VkComputePipelineCreateInfo cpci;
    VkPipelineShaderStageCreateInfo stage;
    VkSamplerCreateInfo samci;
    VkAttachmentDescription att;
    VkAttachmentReference ref;
    VkSubpassDescription sub;
    VkRenderPassCreateInfo rpci;
    VkFramebufferCreateInfo fbci;
    VkBufferImageCopy region;
    void *srcMap = NULL, *dstMap = NULL;
    uint32_t pattern[4], i;
    size_t z;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkResult r;
    int ok = 0;

#define LOAD(field, name) \
    do { field = (PFN_##name)vkGetInstanceProcAddr(instance, #name); \
         if (!field) { Fail("missing entry point " #name); return 1; } } while (0)

    LOAD(CreateImage, vkCreateImage);
    LOAD(DestroyImage, vkDestroyImage);
    LOAD(GetImageReq, vkGetImageMemoryRequirements);
    LOAD(BindImageMemory, vkBindImageMemory);
    LOAD(CreateImageView, vkCreateImageView);
    LOAD(DestroyImageView, vkDestroyImageView);
    LOAD(AllocateMemory, vkAllocateMemory);
    LOAD(FreeMemory, vkFreeMemory);
    LOAD(DestroyBuffer, vkDestroyBuffer);
    LOAD(UnmapMemory, vkUnmapMemory);
    LOAD(CreateCommandPool, vkCreateCommandPool);
    LOAD(DestroyCommandPool, vkDestroyCommandPool);
    LOAD(AllocateCommandBuffers, vkAllocateCommandBuffers);
    LOAD(BeginCommandBuffer, vkBeginCommandBuffer);
    LOAD(EndCommandBuffer, vkEndCommandBuffer);
    LOAD(CmdUpdateBuffer, vkCmdUpdateBuffer);
    LOAD(CmdCopyBufferToImage, vkCmdCopyBufferToImage);
    LOAD(CmdCopyImageToBuffer, vkCmdCopyImageToBuffer);
    LOAD(CreateFence, vkCreateFence);
    LOAD(DestroyFence, vkDestroyFence);
    LOAD(QueueSubmit, vkQueueSubmit);
    LOAD(CreateSemaphore, vkCreateSemaphore);
    LOAD(DestroySemaphore, vkDestroySemaphore);
    LOAD(CreateEvent, vkCreateEvent);
    LOAD(DestroyEvent, vkDestroyEvent);
    LOAD(GetEventStatus, vkGetEventStatus);
    LOAD(SetEvent, vkSetEvent);
    LOAD(ResetEvent, vkResetEvent);
    LOAD(CreateShaderModule, vkCreateShaderModule);
    LOAD(DestroyShaderModule, vkDestroyShaderModule);
    LOAD(CreateDescriptorSetLayout, vkCreateDescriptorSetLayout);
    LOAD(DestroyDescriptorSetLayout, vkDestroyDescriptorSetLayout);
    LOAD(CreatePipelineLayout, vkCreatePipelineLayout);
    LOAD(DestroyPipelineLayout, vkDestroyPipelineLayout);
    LOAD(CreateDescriptorPool, vkCreateDescriptorPool);
    LOAD(DestroyDescriptorPool, vkDestroyDescriptorPool);
    LOAD(AllocateDescriptorSets, vkAllocateDescriptorSets);
    LOAD(FreeDescriptorSets, vkFreeDescriptorSets);
    LOAD(CreateComputePipelines, vkCreateComputePipelines);
    LOAD(DestroyPipeline, vkDestroyPipeline);
    LOAD(CreateSampler, vkCreateSampler);
    LOAD(DestroySampler, vkDestroySampler);
    LOAD(CreateRenderPass, vkCreateRenderPass);
    LOAD(DestroyRenderPass, vkDestroyRenderPass);
    LOAD(CreateFramebuffer, vkCreateFramebuffer);
    LOAD(DestroyFramebuffer, vkDestroyFramebuffer);

#undef LOAD

    for (z = 0; z < sizeof(pci); z++) ((unsigned char *)&pci)[z] = 0;
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = 0;
    if (CreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS) { Fail("vkCreateCommandPool"); return 1; }

    for (z = 0; z < sizeof(cbai); z++) ((unsigned char *)&cbai)[z] = 0;
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (AllocateCommandBuffers(device, &cbai, &cb) != VK_SUCCESS) { Fail("vkAllocateCommandBuffers"); goto objdone; }

    for (z = 0; z < sizeof(fci); z++) ((unsigned char *)&fci)[z] = 0;
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (CreateFence(device, &fci, NULL, &fence) != VK_SUCCESS) { Fail("vkCreateFence"); goto objdone; }

    for (z = 0; z < sizeof(si); z++) ((unsigned char *)&si)[z] = 0;
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;

    for (z = 0; z < sizeof(cbbi); z++) ((unsigned char *)&cbbi)[z] = 0;
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    /* ---- vkCmdUpdateBuffer: inline payload copied to the BO ---- */
    if (MakeBuffer(instance, device, memType, 16384, &srcBuf, &srcMem, &srcMap) != 0) goto objdone;
    if (MakeBuffer(instance, device, memType, 16384, &dstBuf, &dstMem, &dstMap) != 0) goto objdone;

    pattern[0] = 0x11223344u; pattern[1] = 0x55667788u;
    pattern[2] = 0x99AABBCCu; pattern[3] = 0xDDEEFF00u;
    for (i = 0; i < 16384 / 4; i++) ((uint32_t *)srcMap)[i] = 0;
    for (i = 0; i < 16384 / 4; i++) ((uint32_t *)dstMap)[i] = 0xCCCCCCCCu;

    BeginCommandBuffer(cb, &cbbi);
    CmdUpdateBuffer(cb, srcBuf, 0, 16, pattern);
    EndCommandBuffer(cb);
    if (QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) { Fail("vkQueueSubmit (update)"); goto objdone; }
    for (i = 0; i < 4; i++) {
        if (((uint32_t *)srcMap)[i] != pattern[i]) { Fail("vkCmdUpdateBuffer payload"); goto objdone; }
    }
    Step("V4 vkCmdUpdateBuffer payload landed in the BO");

    /* ---- image create / bind / buffer<->image transfer ---- */
    for (z = 0; z < sizeof(ici); z++) ((unsigned char *)&ici)[z] = 0;
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent.width = 64; ici.extent.height = 64; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = CreateImage(device, &ici, NULL, &image);
    if (r != VK_SUCCESS || !image) { Fail("vkCreateImage"); goto objdone; }

    for (z = 0; z < sizeof(ireq); z++) ((unsigned char *)&ireq)[z] = 0;
    GetImageReq(device, image, &ireq);
    for (z = 0; z < sizeof(mai); z++) ((unsigned char *)&mai)[z] = 0;
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = ireq.size < 4096 ? 4096 : ireq.size;
    mai.memoryTypeIndex = memType;
    r = AllocateMemory(device, &mai, NULL, &imageMem);
    if (r != VK_SUCCESS || !imageMem) { Fail("vkAllocateMemory (image)"); goto objdone; }
    r = BindImageMemory(device, image, imageMem, 0);
    if (r != VK_SUCCESS) { Fail("vkBindImageMemory"); goto objdone; }

    for (z = 0; z < sizeof(vci); z++) ((unsigned char *)&vci)[z] = 0;
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    r = CreateImageView(device, &vci, NULL, &view);
    if (r != VK_SUCCESS || !view) { Fail("vkCreateImageView"); goto objdone; }

    for (z = 0; z < sizeof(region); z++) ((unsigned char *)&region)[z] = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = 64; region.imageExtent.height = 64; region.imageExtent.depth = 1;

    /* Fill the source buffer with a deterministic pattern for the round trip. */
    for (i = 0; i < 16384; i++) ((unsigned char *)srcMap)[i] = (unsigned char)(i * 7 + 3);
    for (i = 0; i < 16384; i++) ((unsigned char *)dstMap)[i] = 0xCC;

    BeginCommandBuffer(cb, &cbbi);
    CmdCopyBufferToImage(cb, srcBuf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    CmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstBuf, 1, &region);
    EndCommandBuffer(cb);
    if (QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) { Fail("vkQueueSubmit (image)"); goto objdone; }

    for (i = 0; i < 16384; i++) {
        if (((unsigned char *)dstMap)[i] != ((unsigned char *)srcMap)[i]) {
            Fail("image round-trip mismatch"); goto objdone;
        }
    }
    Step("V4 image (64x64 R8G8B8A8) buffer->image->buffer round-trip OK");

    /* ---- semaphore: submit signal + wait, then destroy ---- */
    for (z = 0; z < sizeof(sci); z++) ((unsigned char *)&sci)[z] = 0;
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (CreateSemaphore(device, &sci, NULL, &sem) != VK_SUCCESS) { Fail("vkCreateSemaphore"); goto objdone; }
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem;
    if (QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) { Fail("vkQueueSubmit (signal)"); goto objdone; }
    si.signalSemaphoreCount = 0;
    si.pSignalSemaphores = NULL;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem;
    si.pWaitDstStageMask = &waitStage;
    if (QueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) { Fail("vkQueueSubmit (wait)"); goto objdone; }
    si.waitSemaphoreCount = 0;
    si.pWaitSemaphores = NULL;
    si.pWaitDstStageMask = NULL;
    Step("V4 semaphore signal/wait accepted by the queue");

    /* ---- events ---- */
    for (z = 0; z < sizeof(eci); z++) ((unsigned char *)&eci)[z] = 0;
    eci.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
    if (CreateEvent(device, &eci, NULL, &ev) != VK_SUCCESS) { Fail("vkCreateEvent"); goto objdone; }
    if (GetEventStatus(device, ev) != VK_EVENT_RESET) { Fail("event initial state"); goto objdone; }
    if (SetEvent(device, ev) != VK_SUCCESS ||
        GetEventStatus(device, ev) != VK_EVENT_SET) { Fail("vkSetEvent"); goto objdone; }
    if (ResetEvent(device, ev) != VK_SUCCESS ||
        GetEventStatus(device, ev) != VK_EVENT_RESET) { Fail("vkResetEvent"); goto objdone; }
    Step("V4 event set/reset/query OK");

    /* ---- shader module + compute pipeline objects (stubs) ---- */
    {
        static const uint32_t spirv[] = { 0x07230203u, 0x00010000u, 0x00000000u, 0x00000001u };
        for (z = 0; z < sizeof(smci); z++) ((unsigned char *)&smci)[z] = 0;
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(spirv);
        smci.pCode = spirv;
        if (CreateShaderModule(device, &smci, NULL, &module) != VK_SUCCESS) {
            Fail("vkCreateShaderModule"); goto objdone;
        }
    }

    for (z = 0; z < sizeof(dslci); z++) ((unsigned char *)&dslci)[z] = 0;
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    if (CreateDescriptorSetLayout(device, &dslci, NULL, &dsl) != VK_SUCCESS) {
        Fail("vkCreateDescriptorSetLayout"); goto objdone;
    }

    for (z = 0; z < sizeof(plci); z++) ((unsigned char *)&plci)[z] = 0;
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    if (CreatePipelineLayout(device, &plci, NULL, &pl) != VK_SUCCESS) {
        Fail("vkCreatePipelineLayout"); goto objdone;
    }

    for (z = 0; z < sizeof(dps); z++) ((unsigned char *)&dps)[z] = 0;
    dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dps.descriptorCount = 1;
    for (z = 0; z < sizeof(dpci); z++) ((unsigned char *)&dpci)[z] = 0;
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &dps;
    if (CreateDescriptorPool(device, &dpci, NULL, &dpool) != VK_SUCCESS) {
        Fail("vkCreateDescriptorPool"); goto objdone;
    }

    for (z = 0; z < sizeof(dsai); z++) ((unsigned char *)&dsai)[z] = 0;
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    if (AllocateDescriptorSets(device, &dsai, &dset) != VK_SUCCESS) {
        Fail("vkAllocateDescriptorSets"); goto objdone;
    }

    for (z = 0; z < sizeof(stage); z++) ((unsigned char *)&stage)[z] = 0;
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    for (z = 0; z < sizeof(cpci); z++) ((unsigned char *)&cpci)[z] = 0;
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage = stage;
    cpci.layout = pl;
    if (CreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline) != VK_SUCCESS || !pipeline) {
        Fail("vkCreateComputePipelines"); goto objdone;
    }
    Step("V4 shader module + descriptor + compute pipeline objects OK");

    /* ---- sampler, render pass, framebuffer ---- */
    for (z = 0; z < sizeof(samci); z++) ((unsigned char *)&samci)[z] = 0;
    samci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samci.magFilter = VK_FILTER_NEAREST;
    samci.minFilter = VK_FILTER_NEAREST;
    samci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samci.maxLod = 1.0f;
    if (CreateSampler(device, &samci, NULL, &sampler) != VK_SUCCESS) { Fail("vkCreateSampler"); goto objdone; }

    for (z = 0; z < sizeof(att); z++) ((unsigned char *)&att)[z] = 0;
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    for (z = 0; z < sizeof(ref); z++) ((unsigned char *)&ref)[z] = 0;
    ref.attachment = 0;
    ref.layout = VK_IMAGE_LAYOUT_GENERAL;

    for (z = 0; z < sizeof(sub); z++) ((unsigned char *)&sub)[z] = 0;
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;

    for (z = 0; z < sizeof(rpci); z++) ((unsigned char *)&rpci)[z] = 0;
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    if (CreateRenderPass(device, &rpci, NULL, &pass) != VK_SUCCESS) { Fail("vkCreateRenderPass"); goto objdone; }

    for (z = 0; z < sizeof(fbci); z++) ((unsigned char *)&fbci)[z] = 0;
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = pass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = 64; fbci.height = 64; fbci.layers = 1;
    if (CreateFramebuffer(device, &fbci, NULL, &fb) != VK_SUCCESS) { Fail("vkCreateFramebuffer"); goto objdone; }
    Step("V4 sampler + render pass + framebuffer OK");

    ok = 1;

objdone:
    if (fb)          DestroyFramebuffer(device, fb, NULL);
    if (pass)        DestroyRenderPass(device, pass, NULL);
    if (sampler)     DestroySampler(device, sampler, NULL);
    if (pipeline)    DestroyPipeline(device, pipeline, NULL);
    if (dset)        { VkDescriptorSet one = dset; FreeDescriptorSets(device, dpool, 1, &one); }
    if (dpool)       DestroyDescriptorPool(device, dpool, NULL);
    if (pl)          DestroyPipelineLayout(device, pl, NULL);
    if (dsl)         DestroyDescriptorSetLayout(device, dsl, NULL);
    if (module)      DestroyShaderModule(device, module, NULL);
    if (ev)          DestroyEvent(device, ev, NULL);
    if (sem)         DestroySemaphore(device, sem, NULL);
    if (fence)       DestroyFence(device, fence, NULL);
    if (view)        DestroyImageView(device, view, NULL);
    if (image)       DestroyImage(device, image, NULL);
    if (imageMem)    FreeMemory(device, imageMem, NULL);
    if (pool)        DestroyCommandPool(device, pool, NULL);
    if (srcMap)      UnmapMemory(device, srcMem);
    if (dstMap)      UnmapMemory(device, dstMem);
    if (srcBuf)      DestroyBuffer(device, srcBuf, NULL);
    if (dstBuf)      DestroyBuffer(device, dstBuf, NULL);
    if (srcMem)      FreeMemory(device, srcMem, NULL);
    if (dstMem)      FreeMemory(device, dstMem, NULL);

    return (ok == 1) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* V5: Win32 WSI - surface, swapchain (KMD BOs), acquire, GDI present  */
/* ------------------------------------------------------------------ */

static void WsiZero(void *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) ((unsigned char *)p)[i] = 0;
}

static HWND WsiCreateWindow(void)
{
    WNDCLASSA wc;
    WsiZero(&wc, (uint32_t)sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "MaliVkSmokeWnd";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return NULL;
    return CreateWindowExA(0, "MaliVkSmokeWnd", "mali_vk_smoke",
                           WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                           NULL, NULL, wc.hInstance, NULL);
}

static int RunWsi(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, VkQueue queue)
{
    PFN_vkCreateWin32SurfaceKHR                   pCreateSurface;
    PFN_vkDestroySurfaceKHR                       pDestroySurface;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR      pSupport;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pCaps;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR      pFormats;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pModes;
    PFN_vkCreateSwapchainKHR                      pCreateSwapchain;
    PFN_vkDestroySwapchainKHR                     pDestroySwapchain;
    PFN_vkGetSwapchainImagesKHR                   pGetImages;
    PFN_vkAcquireNextImageKHR                     pAcquire;
    PFN_vkQueuePresentKHR                         pPresent;
    PFN_vkCreateSemaphore                         pCreateSem;
    PFN_vkDestroySemaphore                        pDestroySem;

    HWND hwnd = NULL;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkSemaphore sem = VK_NULL_HANDLE;
    VkWin32SurfaceCreateInfoKHR  sci;
    VkSwapchainCreateInfoKHR     scci;
    VkSemaphoreCreateInfo        semci;
    VkPresentInfoKHR             pi;
    VkSurfaceCapabilitiesKHR     caps;
    VkSurfaceFormatKHR           fmt;
    VkPresentModeKHR             mode;
    VkBool32 supported = VK_FALSE;
    VkImage images[8];
    uint32_t n, idx = 0;
    VkResult r;
    int rc = 1;

    pCreateSurface   = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
    pDestroySurface  = (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(inst, "vkDestroySurfaceKHR");
    pSupport         = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceSurfaceSupportKHR");
    pCaps            = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    pFormats         = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    pModes           = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    pCreateSwapchain = (PFN_vkCreateSwapchainKHR)vkGetInstanceProcAddr(inst, "vkCreateSwapchainKHR");
    pDestroySwapchain= (PFN_vkDestroySwapchainKHR)vkGetInstanceProcAddr(inst, "vkDestroySwapchainKHR");
    pGetImages       = (PFN_vkGetSwapchainImagesKHR)vkGetInstanceProcAddr(inst, "vkGetSwapchainImagesKHR");
    pAcquire         = (PFN_vkAcquireNextImageKHR)vkGetInstanceProcAddr(inst, "vkAcquireNextImageKHR");
    pPresent         = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(inst, "vkQueuePresentKHR");
    pCreateSem       = (PFN_vkCreateSemaphore)vkGetInstanceProcAddr(inst, "vkCreateSemaphore");
    pDestroySem      = (PFN_vkDestroySemaphore)vkGetInstanceProcAddr(inst, "vkDestroySemaphore");

    if (!pCreateSurface || !pDestroySurface || !pSupport || !pCaps || !pFormats ||
        !pModes || !pCreateSwapchain || !pDestroySwapchain || !pGetImages ||
        !pAcquire || !pPresent || !pCreateSem || !pDestroySem) {
        Fail("WSI entry points");
        return 1;
    }

    hwnd = WsiCreateWindow();
    if (!hwnd) { Fail("WSI window creation"); return 1; }

    WsiZero(&sci, (uint32_t)sizeof(sci));
    sci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hwnd      = hwnd;
    sci.hinstance = GetModuleHandleA(NULL);

    r = pCreateSurface(inst, &sci, NULL, &surf);
    if (r != VK_SUCCESS || !surf) { Fail("vkCreateWin32SurfaceKHR"); goto done; }

    r = pSupport(phys, 0, surf, &supported);
    if (r != VK_SUCCESS || supported != VK_TRUE) { Fail("surface support"); goto done; }

    r = pCaps(phys, surf, &caps);
    if (r != VK_SUCCESS || caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        Fail("surface capabilities");
        goto done;
    }

    n = 8;
    r = pFormats(phys, surf, &n, &fmt);
    if (r != VK_SUCCESS || n != 2) { Fail("surface formats"); goto done; }

    n = 8;
    r = pModes(phys, surf, &n, &mode);
    if (r != VK_SUCCESS || n != 1 || mode != VK_PRESENT_MODE_FIFO_KHR) { Fail("present modes"); goto done; }

    /* Swapchain images are KMD BOs; creating them proves the BO path. */
    WsiZero(&scci, (uint32_t)sizeof(scci));
    scci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scci.surface          = surf;
    scci.minImageCount    = 2;
    scci.imageFormat      = fmt.format;
    scci.imageColorSpace  = fmt.colorSpace;
    scci.imageExtent      = caps.currentExtent;
    scci.imageArrayLayers = 1;
    scci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scci.preTransform     = caps.currentTransform;
    scci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    scci.clipped          = VK_TRUE;
    scci.oldSwapchain     = VK_NULL_HANDLE;

    r = pCreateSwapchain(dev, &scci, NULL, &sc);
    if (r != VK_SUCCESS || !sc) { Fail("vkCreateSwapchainKHR"); goto done; }

    n = 8;
    r = pGetImages(dev, sc, &n, images);
    if (r != VK_SUCCESS || n < 2) { Fail("vkGetSwapchainImagesKHR"); goto done; }

    Step("V5 swapchain created (images are KMD BOs)");

    WsiZero(&semci, (uint32_t)sizeof(semci));
    semci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    r = pCreateSem(dev, &semci, NULL, &sem);
    if (r != VK_SUCCESS || !sem) { Fail("vkCreateSemaphore (WSI)"); goto done; }

    r = pAcquire(dev, sc, 1000000000ull, sem, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS || idx >= n) { Fail("vkAcquireNextImageKHR"); goto done; }

    WsiZero(&pi, (uint32_t)sizeof(pi));
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &sem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &sc;
    pi.pImageIndices      = &idx;

    r = pPresent(queue, &pi);
    if (r != VK_SUCCESS) { Fail("vkQueuePresentKHR (GDI blit)"); goto done; }

    Step("V5 acquire + present via GDI blit to the window OK");
    rc = 0;

done:
    if (sem) pDestroySem(dev, sem, NULL);
    if (sc)  pDestroySwapchain(dev, sc, NULL);
    if (surf) pDestroySurface(inst, surf, NULL);
    if (hwnd) DestroyWindow(hwnd);
    return rc;
}

static int Run(void)
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    Api api;
    VkResult r;
    uint32_t count;
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memProps;
    VkDeviceQueueCreateInfo qci;
    VkDeviceCreateInfo dci;
    float prio = 1.0f;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai;
    VkCommandBufferBeginInfo cbbi;
    VkSubmitInfo si;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci;
    VkMemoryAllocateInfo mai;
    VkBuffer buf = VK_NULL_HANDLE;
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci;
    void *mapped = NULL;
    volatile uint32_t *words;
    uint32_t i, visibleType = 0xFFFFFFFFu;
    int rc = 1, ok = 1;

    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (i = 0; i < sizeof(api); i++) ((unsigned char *)&api)[i] = 0;
    for (i = 0; i < sizeof(app); i++) ((unsigned char *)&app)[i] = 0;

    /* ---- V1: instance ---- */
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "mali_vk_smoke";
    app.applicationVersion = 1;
    app.pEngineName = "mali_vk_smoke";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_0;

    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pNext = NULL; ici.flags = 0;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 0; ici.ppEnabledLayerNames = NULL;
    ici.enabledExtensionCount = 0; ici.ppEnabledExtensionNames = NULL;

    GET(&api, instance, CreateInstance, vkCreateInstance);
    r = api.CreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS || !instance) { Fail("vkCreateInstance"); return 1; }
    Step("V1 instance created");

    GET(&api, instance, EnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    GET(&api, instance, GetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    GET(&api, instance, GetQueueFamilyProperties, vkGetPhysicalDeviceQueueFamilyProperties);
    GET(&api, instance, CreateDevice, vkCreateDevice);
    GET(&api, instance, DestroyDevice, vkDestroyDevice);
    GET(&api, instance, GetDeviceQueue, vkGetDeviceQueue);
    GET(&api, instance, DeviceWaitIdle, vkDeviceWaitIdle);
    GET(&api, instance, AllocateMemory, vkAllocateMemory);
    GET(&api, instance, FreeMemory, vkFreeMemory);
    GET(&api, instance, MapMemory, vkMapMemory);
    GET(&api, instance, UnmapMemory, vkUnmapMemory);
    GET(&api, instance, CreateBuffer, vkCreateBuffer);
    GET(&api, instance, DestroyBuffer, vkDestroyBuffer);
    GET(&api, instance, GetBufferMemoryRequirements, vkGetBufferMemoryRequirements);
    GET(&api, instance, BindBufferMemory, vkBindBufferMemory);
    GET(&api, instance, CreateCommandPool, vkCreateCommandPool);
    GET(&api, instance, DestroyCommandPool, vkDestroyCommandPool);
    GET(&api, instance, AllocateCommandBuffers, vkAllocateCommandBuffers);
    GET(&api, instance, FreeCommandBuffers, vkFreeCommandBuffers);
    GET(&api, instance, BeginCommandBuffer, vkBeginCommandBuffer);
    GET(&api, instance, EndCommandBuffer, vkEndCommandBuffer);
    GET(&api, instance, CmdFillBuffer, vkCmdFillBuffer);
    GET(&api, instance, CreateFence, vkCreateFence);
    GET(&api, instance, DestroyFence, vkDestroyFence);
    GET(&api, instance, WaitForFences, vkWaitForFences);
    GET(&api, instance, QueueSubmit, vkQueueSubmit);

    count = 0;
    r = api.EnumeratePhysicalDevices(instance, &count, NULL);
    if (r != VK_SUCCESS || count == 0) { Fail("no physical device (is MaliKmd installed?)"); goto done; }
    count = 1;
    r = api.EnumeratePhysicalDevices(instance, &count, &phys);
    if (r != VK_SUCCESS || count != 1) { Fail("vkEnumeratePhysicalDevices"); goto done; }

    api.GetPhysicalDeviceProperties(phys, &props);
    Out("V1 physical device: "); Out(props.deviceName);
    Out("  api "); OutDec(VK_VERSION_MAJOR(props.apiVersion)); Out(".");
    OutDec(VK_VERSION_MINOR(props.apiVersion));
    Out("  vendor "); OutHex(props.vendorID);
    Out("  device "); OutHex(props.deviceID); Out("\r\n");

    /* ---- V1: device + queue ---- */
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.pNext = NULL; qci.flags = 0; qci.queueFamilyIndex = 0; qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = NULL; dci.flags = 0;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledLayerCount = 0; dci.ppEnabledLayerNames = NULL;
    dci.enabledExtensionCount = 0; dci.ppEnabledExtensionNames = NULL;
    dci.pEnabledFeatures = NULL;

    r = api.CreateDevice(phys, &dci, NULL, &device);
    if (r != VK_SUCCESS || !device) { Fail("vkCreateDevice"); goto done; }
    api.GetDeviceQueue(device, 0, 0, &queue);
    if (!queue) { Fail("vkGetDeviceQueue"); goto done; }
    Step("V1 device + queue created");

    /* ---- V2: memory -> buffer -> bind -> map ---- */
    for (i = 0; i < sizeof(memProps); i++) ((unsigned char *)&memProps)[i] = 0;

    {
        PFN_vkGetPhysicalDeviceMemoryProperties gmp =
            (PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties");
        if (!gmp) { Fail("vkGetPhysicalDeviceMemoryProperties"); goto done; }
        gmp(phys, &memProps);
    }
    for (i = 0; i < memProps.memoryTypeCount; i++) {
        if (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            visibleType = i; break;
        }
    }
    if (visibleType == 0xFFFFFFFFu) { Fail("no host-visible memory type"); goto done; }

    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.pNext = NULL; bci.flags = 0; bci.size = 4096;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bci.queueFamilyIndexCount = 0; bci.pQueueFamilyIndices = NULL;
    r = api.CreateBuffer(device, &bci, NULL, &buf);
    if (r != VK_SUCCESS || !buf) { Fail("vkCreateBuffer"); goto done; }

    for (i = 0; i < sizeof(req); i++) ((unsigned char *)&req)[i] = 0;
    api.GetBufferMemoryRequirements(device, buf, &req);

    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = NULL;
    mai.allocationSize = req.size < 4096 ? 4096 : req.size;
    mai.memoryTypeIndex = visibleType;
    r = api.AllocateMemory(device, &mai, NULL, &mem);
    if (r != VK_SUCCESS || !mem) { Fail("vkAllocateMemory"); goto done; }
    r = api.BindBufferMemory(device, buf, mem, 0);
    if (r != VK_SUCCESS) { Fail("vkBindBufferMemory"); goto done; }

    r = api.MapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS || !mapped) { Fail("vkMapMemory"); goto done; }
    words = (volatile uint32_t *)mapped;
    words[0] = 0xDEADBEEFu;
    if (words[0] != 0xDEADBEEFu) { Fail("BO CPU write/read"); goto done; }
    Step("V2 buffer object mapped, CPU r/w OK");

    /* ---- V3: fill 0 on the GPU -> submit -> fence -> verify ---- */
    for (i = 0; i < 1024; i++) words[i] = 0xDEADBEEFu;   /* dirty the whole buffer */

    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.pNext = NULL; pci.flags = 0; pci.queueFamilyIndex = 0;
    r = api.CreateCommandPool(device, &pci, NULL, &pool);
    if (r != VK_SUCCESS || !pool) { Fail("vkCreateCommandPool"); goto done; }

    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.pNext = NULL; cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    r = api.AllocateCommandBuffers(device, &cbai, &cb);
    if (r != VK_SUCCESS || !cb) { Fail("vkAllocateCommandBuffers"); goto done; }

    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.pNext = NULL; cbbi.flags = 0; cbbi.pInheritanceInfo = NULL;
    api.BeginCommandBuffer(cb, &cbbi);
    /* Zero 1 KiB = 128 WRITE_VALUE jobs, under the ICD's 256-job GPU budget. */
    api.CmdFillBuffer(cb, buf, 0, 1024, 0);
    api.EndCommandBuffer(cb);

    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fci.pNext = NULL; fci.flags = 0;
    r = api.CreateFence(device, &fci, NULL, &fence);
    if (r != VK_SUCCESS || !fence) { Fail("vkCreateFence"); goto done; }

    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = NULL; si.waitSemaphoreCount = 0; si.pWaitSemaphores = NULL;
    si.pWaitDstStageMask = NULL; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 0; si.pSignalSemaphores = NULL;
    r = api.QueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) { Fail("vkQueueSubmit"); goto done; }

    r = api.WaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull);
    if (r != VK_SUCCESS) { Fail("vkWaitForFences"); goto done; }

    for (i = 0; i < 256; i++) {
        if (words[i] != 0) { ok = 0; break; }
    }
    if (!ok) { Fail("GPU zero fill did not clear the buffer"); goto done; }
    Step("V3 GPU vkCmdFillBuffer(0) cleared 1 KiB via a WRITE_VALUE chain");

    /* Non-zero fill: ICD fallback path (host). */
    api.BeginCommandBuffer(cb, &cbbi);
    api.CmdFillBuffer(cb, buf, 0, 64, 0xABABABABu);
    api.EndCommandBuffer(cb);
    r = api.QueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) { Fail("vkQueueSubmit (nonzero)"); goto done; }
    for (i = 0; i < 16; i++) {
        if (words[i] != 0xABABABABu) { ok = 0; break; }
    }
    if (!ok) { Fail("non-zero fill mismatch"); goto done; }
    Step("V3 non-zero fill OK (host fallback)");

    /* ---- V4 object model: images, semaphores, events, pipelines ---- */
    if (RunDeviceObjects(instance, device, queue, visibleType) != 0) goto done;
    Step("V4 device-object coverage done");

    /* ---- V5 Win32 WSI: surface -> swapchain -> acquire -> present ---- */
    if (RunWsi(instance, phys, device, queue) != 0) goto done;

    rc = 0;

done:
    if (mapped) api.UnmapMemory(device, mem);
    if (mem)    api.FreeMemory(device, mem, NULL);
    if (buf)    api.DestroyBuffer(device, buf, NULL);
    if (fence)  api.DestroyFence(device, fence, NULL);
    if (pool)   api.DestroyCommandPool(device, pool, NULL);
    if (device) api.DestroyDevice(device, NULL);
    if (instance) {
        PFN_vkDestroyInstance di =
            (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
        if (di) di(instance, NULL);
    }

    Out(rc == 0 ? "RESULT: PASS\r\n" : "RESULT: FAIL\r\n");
    return rc;
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)Run());
}
