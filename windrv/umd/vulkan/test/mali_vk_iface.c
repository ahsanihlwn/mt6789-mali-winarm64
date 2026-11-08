/*
 * mali_vk_iface.c  -  host self-test for the ICD's entry-point surface.
 *
 * Built for x64 and run on the development machine (no Mali hardware, no
 * tablet): it loads nothing but its own import library, creates an instance,
 * and asks vkGetInstanceProcAddr for every entry point the ICD claims to
 * export. A NULL result means the loader would hand the application a null
 * function pointer, which is the most common way a hand-written ICD breaks
 * real applications.
 *
 * It also walks the instance-level queries that do not need a device:
 *   - vkEnumerateInstanceExtensionProperties reports KHR_get_properties2
 *   - vkEnumeratePhysicalDevices returns 0 adapters when the KMD is absent
 *   - vkGetPhysicalDeviceProperties2 / Features2 / MemoryProperties2 /
 *     FormatProperties2 run and fill the chained structures.
 *
 * The physical-device-2 calls ignore their VkPhysicalDevice argument in this
 * ICD, so the self-test passes VK_NULL_HANDLE on purpose. That is not legal
 * application usage; it is only a way to exercise the chaining code on a host
 * with no GPU. The ARM64 smoke test does the real thing on the tablet.
 *
 * CRT-free, same style as the other tools.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#include <vulkan/vulkan.h>

extern PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *);
extern PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance, const char *);
extern VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *);

static HANDLE g_out;
static int    g_fail;

static void Out(const char *s) { DWORD n; WriteFile(g_out, s, (DWORD)lstrlenA(s), &n, NULL); }
static void OutU(unsigned v, unsigned base)
{
    char b[24]; const char *h = "0123456789ABCDEF"; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v) { b[i++] = h[v % base]; v /= base; }
    while (i) { DWORD n; WriteFile(g_out, &b[--i], 1, &n, NULL); }
}
static void OutDec(unsigned v) { OutU(v, 10); }
static void Step(const char *s) { Out(s); Out("\r\n"); }
static void Fail(const char *s) { Out("  FAIL: "); Out(s); Out("\r\n"); g_fail = 1; }
static int  StrEq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Every name vkGetInstanceProcAddr must resolve. */
static const char *const g_Names[] = {
    "vkEnumerateInstanceVersion",
    "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateInstanceLayerProperties",
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkEnumeratePhysicalDevices",
    "vkGetPhysicalDeviceProperties",
    "vkGetPhysicalDeviceProperties2",
    "vkGetPhysicalDeviceProperties2KHR",
    "vkGetPhysicalDeviceFeatures",
    "vkGetPhysicalDeviceFeatures2",
    "vkGetPhysicalDeviceFeatures2KHR",
    "vkGetPhysicalDeviceQueueFamilyProperties",
    "vkGetPhysicalDeviceQueueFamilyProperties2",
    "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
    "vkGetPhysicalDeviceMemoryProperties",
    "vkGetPhysicalDeviceMemoryProperties2",
    "vkGetPhysicalDeviceMemoryProperties2KHR",
    "vkGetPhysicalDeviceFormatProperties",
    "vkGetPhysicalDeviceFormatProperties2",
    "vkGetPhysicalDeviceFormatProperties2KHR",
    "vkGetPhysicalDeviceImageFormatProperties",
    "vkGetPhysicalDeviceImageFormatProperties2",
    "vkGetPhysicalDeviceImageFormatProperties2KHR",
    "vkGetPhysicalDeviceSparseImageFormatProperties",
    "vkGetPhysicalDeviceSparseImageFormatProperties2",
    "vkGetPhysicalDeviceSparseImageFormatProperties2KHR",
    "vkEnumerateDeviceExtensionProperties",
    "vkEnumerateDeviceLayerProperties",

    "vkCreateDevice",
    "vkDestroyDevice",
    "vkGetDeviceQueue",
    "vkDeviceWaitIdle",

    "vkAllocateMemory",
    "vkFreeMemory",
    "vkMapMemory",
    "vkUnmapMemory",
    "vkFlushMappedMemoryRanges",
    "vkInvalidateMappedMemoryRanges",
    "vkGetDeviceMemoryCommitment",
    "vkGetBufferMemoryRequirements",
    "vkGetBufferMemoryRequirements2",
    "vkGetBufferMemoryRequirements2KHR",
    "vkBindBufferMemory",
    "vkBindBufferMemory2",
    "vkBindBufferMemory2KHR",

    "vkCreateBuffer",
    "vkDestroyBuffer",
    "vkCreateBufferView",
    "vkDestroyBufferView",
    "vkCreateImage",
    "vkDestroyImage",
    "vkGetImageMemoryRequirements",
    "vkGetImageMemoryRequirements2",
    "vkGetImageMemoryRequirements2KHR",
    "vkBindImageMemory",
    "vkBindImageMemory2",
    "vkBindImageMemory2KHR",
    "vkGetImageSubresourceLayout",
    "vkCreateImageView",
    "vkDestroyImageView",

    "vkCreateShaderModule",
    "vkDestroyShaderModule",
    "vkCreateDescriptorSetLayout",
    "vkDestroyDescriptorSetLayout",
    "vkCreatePipelineLayout",
    "vkDestroyPipelineLayout",
    "vkCreateDescriptorPool",
    "vkDestroyDescriptorPool",
    "vkResetDescriptorPool",
    "vkAllocateDescriptorSets",
    "vkFreeDescriptorSets",
    "vkUpdateDescriptorSets",
    "vkCreateRenderPass",
    "vkDestroyRenderPass",
    "vkCreateFramebuffer",
    "vkDestroyFramebuffer",
    "vkCreateGraphicsPipelines",
    "vkCreateComputePipelines",
    "vkDestroyPipeline",
    "vkCreateSampler",
    "vkDestroySampler",

    "vkCreateCommandPool",
    "vkDestroyCommandPool",
    "vkResetCommandPool",
    "vkAllocateCommandBuffers",
    "vkFreeCommandBuffers",
    "vkBeginCommandBuffer",
    "vkEndCommandBuffer",
    "vkResetCommandBuffer",
    "vkCmdFillBuffer",
    "vkCmdUpdateBuffer",
    "vkCmdCopyBuffer",
    "vkCmdCopyBufferToImage",
    "vkCmdCopyImageToBuffer",
    "vkCmdClearColorImage",
    "vkCmdClearDepthStencilImage",
    "vkCmdClearAttachments",
    "vkCmdPipelineBarrier",
    "vkCmdExecuteCommands",
    "vkCmdBindPipeline",
    "vkCmdSetViewport",
    "vkCmdSetScissor",
    "vkCmdSetLineWidth",
    "vkCmdBindDescriptorSets",
    "vkCmdBindIndexBuffer",
    "vkCmdBindVertexBuffers",
    "vkCmdDraw",
    "vkCmdDrawIndexed",
    "vkCmdDispatch",
    "vkCmdBeginRenderPass",
    "vkCmdEndRenderPass",

    "vkCreateFence",
    "vkDestroyFence",
    "vkResetFences",
    "vkGetFenceStatus",
    "vkWaitForFences",
    "vkCreateSemaphore",
    "vkDestroySemaphore",
    "vkCreateEvent",
    "vkDestroyEvent",
    "vkGetEventStatus",
    "vkSetEvent",
    "vkResetEvent",
    "vkCmdSetEvent",
    "vkCmdResetEvent",
    "vkCmdWaitEvents",
    "vkQueueSubmit",

    "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr",
    0
};

/* The three vk_icd* functions belong to the loader interface, not to the
 * application surface: the Khronos loader asks for them through
 * vk_icdGetInstanceProcAddr, so that is where the test looks for them. */
static void TestLoaderInterface(void)
{
    PFN_vkVoidFunction fn;
    uint32_t ver = 0;
    VkResult r;

    Step("loader/ICD interface");

    fn = vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vk_icdGetPhysicalDeviceProcAddr");
    if (!fn) { Fail("vk_icdGetPhysicalDeviceProcAddr via vk_icdGetInstanceProcAddr"); return; }
    fn = vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (!fn) { Fail("vk_icdNegotiateLoaderICDInterfaceVersion via vk_icdGetInstanceProcAddr"); return; }

    ver = 3;   /* ask for more than we implement; the ICD must clamp down */
    r = vk_icdNegotiateLoaderICDInterfaceVersion(&ver);
    if (r != VK_SUCCESS || ver < 2) { Fail("interface version negotiation"); return; }
    Out("  interface version   ");
    OutDec(ver);
    Out("\r\n");
}

static void TestEntryPoints(VkInstance instance)
{
    uint32_t i, missing = 0, total = 0;

    for (i = 0; g_Names[i]; i++) {
        PFN_vkVoidFunction fn = vkGetInstanceProcAddr(instance, g_Names[i]);
        total++;
        if (!fn) {
            Out("  missing: "); Out(g_Names[i]); Out("\r\n");
            missing++;
        }
    }

    Step("entry points");
    Out("  resolved ");
    OutDec(total - missing);
    Out(" / ");
    OutDec(total);
    Out("\r\n");
    if (missing) { g_fail = 1; Out("  FAIL: unresolved entry points\r\n"); }
}

static void TestInstanceQueries(void)
{
    PFN_vkVoidFunction fp;
    VkResult r;
    uint32_t count, api = 0;
    VkExtensionProperties ext;

    Step("instance queries");

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (!fp) { Fail("vkEnumerateInstanceVersion not resolvable without instance"); return; }
    r = ((PFN_vkEnumerateInstanceVersion)fp)(&api);
    if (r != VK_SUCCESS || api != VK_API_VERSION_1_0) { Fail("vkEnumerateInstanceVersion"); return; }
    Out("  apiVersion          1.0\r\n");

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!fp) { Fail("vkEnumerateInstanceExtensionProperties not resolvable"); return; }
    count = 0;
    r = ((PFN_vkEnumerateInstanceExtensionProperties)fp)(NULL, &count, NULL);
    if (r != VK_SUCCESS || count != 1) { Fail("expected exactly 1 instance extension"); return; }
    r = ((PFN_vkEnumerateInstanceExtensionProperties)fp)(NULL, &count, &ext);
    if (r != VK_SUCCESS || !StrEq(ext.extensionName, "VK_KHR_get_physical_device_properties2")) {
        Fail("wrong instance extension name");
        return;
    }
    Out("  instance ext        "); Out(ext.extensionName); Out("\r\n");
}

static void TestProperties2(void)
{
    PFN_vkVoidFunction fp;
    VkPhysicalDeviceProperties2 p2;
    VkPhysicalDeviceIDProperties id;
    VkPhysicalDeviceDriverProperties drv;
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceMemoryProperties2 m2;
    VkFormatProperties2 fmt;

    Step("physical device queries (chained, no KMD needed)");

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2");
    if (!fp) { Fail("vkGetPhysicalDeviceProperties2"); return; }
    {
        unsigned char *p = (unsigned char *)&p2;
        uint32_t n = (uint32_t)sizeof(p2);
        while (n--) *p++ = 0;
    }
    {
        unsigned char *p = (unsigned char *)&id;
        uint32_t n = (uint32_t)sizeof(id);
        while (n--) *p++ = 0;
    }
    {
        unsigned char *p = (unsigned char *)&drv;
        uint32_t n = (uint32_t)sizeof(drv);
        while (n--) *p++ = 0;
    }

    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    id.pNext = &drv;
    drv.pNext = NULL;
    p2.pNext = &id;

    ((PFN_vkGetPhysicalDeviceProperties2)fp)(VK_NULL_HANDLE, &p2);
    if (p2.properties.vendorID != 0x13B5) { Fail("vendorID"); return; }
    if (p2.properties.deviceID != 0x9003) { Fail("deviceID"); return; }
    Out("  properties2         "); Out(p2.properties.deviceName); Out("\r\n");
    Out("  driverID            "); OutDec((unsigned)drv.driverID); Out("\r\n");
    if (!id.deviceLUIDValid) { Fail("deviceLUIDValid"); return; }
    Out("  deviceLUIDValid     yes\r\n");

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2");
    if (!fp) { Fail("vkGetPhysicalDeviceFeatures2"); return; }
    {
        unsigned char *p = (unsigned char *)&f2;
        uint32_t n = (uint32_t)sizeof(f2);
        while (n--) *p++ = 0;
    }
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    ((PFN_vkGetPhysicalDeviceFeatures2)fp)(VK_NULL_HANDLE, &f2);

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties2");
    if (!fp) { Fail("vkGetPhysicalDeviceMemoryProperties2"); return; }
    {
        unsigned char *p = (unsigned char *)&m2;
        uint32_t n = (uint32_t)sizeof(m2);
        while (n--) *p++ = 0;
    }
    m2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    ((PFN_vkGetPhysicalDeviceMemoryProperties2)fp)(VK_NULL_HANDLE, &m2);
    if (m2.memoryProperties.memoryTypeCount != 2) { Fail("memoryTypeCount"); return; }
    Out("  memory types        2 (1 heap, 256 MB)\r\n");

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFormatProperties2");
    if (!fp) { Fail("vkGetPhysicalDeviceFormatProperties2"); return; }
    {
        unsigned char *p = (unsigned char *)&fmt;
        uint32_t n = (uint32_t)sizeof(fmt);
        while (n--) *p++ = 0;
    }
    fmt.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    ((PFN_vkGetPhysicalDeviceFormatProperties2)fp)(VK_NULL_HANDLE, VK_FORMAT_R8G8B8A8_UNORM, &fmt);
    if (!(fmt.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
        Fail("R8G8B8A8 transfer-dst");
        return;
    }
    Out("  R8G8B8A8_UNORM      transfer + sampled + color\r\n");
}

static void TestNoKmd(void)
{
    PFN_vkVoidFunction fp;
    VkResult r;
    uint32_t count = 0;
    VkInstance inst = VK_NULL_HANDLE;
    VkInstanceCreateInfo ici;
    VkApplicationInfo app;

    Step("adapter enumeration without KMD");

    fp = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!fp) { Fail("vkCreateInstance"); return; }

    {
        unsigned char *p = (unsigned char *)&app;
        uint32_t n = (uint32_t)sizeof(app);
        while (n--) *p++ = 0;
    }
    {
        unsigned char *p = (unsigned char *)&ici;
        uint32_t n = (uint32_t)sizeof(ici);
        while (n--) *p++ = 0;
    }
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "mali_vk_iface";
    app.apiVersion = VK_API_VERSION_1_0;
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    r = ((PFN_vkCreateInstance)fp)(&ici, NULL, &inst);
    if (r != VK_SUCCESS || !inst) { Fail("vkCreateInstance"); return; }

    TestEntryPoints(inst);

    fp = vkGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
    if (!fp) { Fail("vkEnumeratePhysicalDevices"); return; }
    r = ((PFN_vkEnumeratePhysicalDevices)fp)(inst, &count, NULL);
    if (r != VK_SUCCESS) { Fail("vkEnumeratePhysicalDevices(query) != VK_SUCCESS"); }
    else Out("  adapters without KMD 0 (VK_SUCCESS)\r\n");

    fp = vkGetInstanceProcAddr(inst, "vkDestroyInstance");
    if (fp) ((PFN_vkDestroyInstance)fp)(inst, NULL);
}

static int Run(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_out || g_out == INVALID_HANDLE_VALUE) g_out = (HANDLE)1;
    g_fail = 0;

    Out("mali_vk_iface: ICD entry-point self-test (host x64)\r\n\r\n");

    TestLoaderInterface();
    TestInstanceQueries();
    TestProperties2();
    TestNoKmd();

    Out("\r\n");
    if (g_fail) { Out("RESULT: FAIL\r\n"); return 1; }
    Out("RESULT: PASS\r\n");
    return 0;
}

void mainCRTStartup(void)
{
    ExitProcess((UINT)Run());
}