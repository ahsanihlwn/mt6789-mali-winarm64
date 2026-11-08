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
#include <vulkan/vulkan.h>

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
