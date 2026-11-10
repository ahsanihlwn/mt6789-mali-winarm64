/*
 * panvk_smoke.c  -  first-run validation for the PanVK ICD
 *                   (vulkan_panfrost.dll, Windows ARM64, MT6789 Mali-G57).
 *
 * WHY THIS EXISTS
 * ---------------
 * The Valhall v9 PanVK stack builds and links, but until it has run on the
 * tablet nobody knows whether the kbase uAPI emulation in mali_drm.cpp is
 * right, or where it stops.  This program is the smallest thing that answers
 * that question, in stages, so a failure names the layer that broke:
 *
 *   V0  preflight: the DLL next to the exe, the KMD device interface
 *   V1  load the ICD and find its three loader entry points
 *   V2  negotiate the loader/ICD interface version
 *   V3  vkCreateInstance
 *   V4  vkEnumeratePhysicalDevices  (needs the KMD to be openable)
 *   V5  physical-device properties and memory heaps   (informational)
 *   V6  queue families                                (informational)
 *   V7  vkCreateDevice
 *   V8  buffer + memory + bind + map + write
 *   V9  vkCmdFillBuffer, submit, wait                 (pass criterion)
 *   V10 map again and verify the pattern              (pass criterion)
 *
 * RESULT: PASS requires V1-V4 and V7-V10.  V5/V6 only report.
 *
 * V9 is the real test: on PanVK vkCmdFillBuffer is a compute dispatch through
 * the meta shaders, so it exercises shader compilation, BO allocation, the
 * job chain and the KMD submit path.  A CPU fallback cannot fake it - if the
 * pattern comes back correct, the GPU ran.  Cross-check with the KMD's job
 * counter (malitest.exe --irq) afterwards; it must have advanced.
 *
 * The KMD device is created exclusive (WdfDeviceInitSetExclusive), so nothing
 * else may hold it while this runs: close malitest.exe and any other Mali
 * client first.
 *
 * PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 is set here, not left to the operator.
 * panvk_physical_device.c gates arch 9 (Valhall) on it, and this port exists
 * precisely to run v9, so the gate is noise in this context.  It is set with
 * _putenv_s() before the DLL is loaded so both getenv() and
 * GetEnvironmentVariable() see it.
 *
 * The ICD is loaded directly through its loader interface instead of through
 * vulkan-1.dll: there is no Vulkan loader on the tablet and no import library
 * in this tree, and this is exactly what the loader does anyway.
 *
 * SPDX-License-Identifier: MIT
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS 1

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES 1
#endif
#include <vulkan/vulkan.h>

/* Only for MaliUmdEnumerate(): the preflight check that the KMD interface is
 * present.  Nothing here opens the device - the ICD owns it exclusively. */
#include "mali_umd.h"

/* ------------------------------------------------------------------ */
/* loader interface (not part of the application surface)              */
/* ------------------------------------------------------------------ */

typedef VkResult (VKAPI_CALL *PFN_MALI_icdNegotiate)(uint32_t *);
typedef PFN_vkVoidFunction (VKAPI_CALL *PFN_MALI_icdGipa)(VkInstance,
                                                         const char *);

static HMODULE               g_mod;
static PFN_MALI_icdNegotiate g_negotiate;
static PFN_MALI_icdGipa      g_gipa;

static int                   g_fail;
static VkInstance            g_instance;

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static void Stage(const char *name)
{
    printf("\n[%s]\n", name);
    fflush(stdout);
}

static void Note(const char *fmt, ...)
{
    va_list ap;
    printf("  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void Fail(const char *fmt, ...)
{
    va_list ap;
    printf("  FAIL  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    g_fail = 1;
}

static const char *ResultName(VkResult r)
{
    switch ((int)r) {
    case VK_SUCCESS:                        return "VK_SUCCESS";
    case VK_NOT_READY:                      return "VK_NOT_READY";
    case VK_TIMEOUT:                        return "VK_TIMEOUT";
    case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
    default:                                return "VK_ERROR_<other>";
    }
}

/* ------------------------------------------------------------------ */
/* V0  preflight                                                       */
/* ------------------------------------------------------------------ */

static int Preflight(char *dllPath, size_t cap)
{
    char  dir[MAX_PATH];
    char  narrow[260];
    DWORD n;
    ULONG count;
    WCHAR paths[8][260];

    Stage("V0 preflight");

    n = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
    if (n == 0 || n >= sizeof(dir)) {
        Fail("GetModuleFileNameA failed, GetLastError=%lu", GetLastError());
        return 0;
    }
    {
        char *slash = strrchr(dir, '\\');
        if (slash)
            *slash = '\0';
    }
    snprintf(dllPath, cap, "%s\\vulkan_panfrost.dll", dir);

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        Fail("vulkan_panfrost.dll is not next to the exe (%s)", dllPath);
        return 0;
    }
    Note("icd                %s", dllPath);

    count = MaliUmdEnumerate(&paths[0][0], (ULONG)(sizeof(paths) / sizeof(paths[0])));
    Note("kmd interfaces     %lu", (unsigned long)count);
    if (count == 0) {
        Note("no Mali KMD device interface is present.");
        Note("the ICD will report 0 adapters; install/start MaliKmd first.");
    } else if (WideCharToMultiByte(CP_ACP, 0, paths[0], -1, narrow,
                                   (int)sizeof(narrow), NULL, NULL) > 0) {
        Note("first              %s", narrow);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* V1  load the ICD                                                    */
/* ------------------------------------------------------------------ */

static int LoadIcd(const char *dllPath)
{
    char  full[MAX_PATH];
    DWORD n;

    Stage("V1 load the ICD");

    g_mod = LoadLibraryA(dllPath);
    if (!g_mod) {
        Fail("LoadLibraryA failed, GetLastError=%lu", GetLastError());
        return 0;
    }

    n = GetModuleFileNameA(g_mod, full, (DWORD)sizeof(full));
    Note("module             %s", n ? full : "(path unavailable)");

    g_negotiate = (PFN_MALI_icdNegotiate)(void *)
                  GetProcAddress(g_mod, "vk_icdNegotiateLoaderICDInterfaceVersion");
    g_gipa      = (PFN_MALI_icdGipa)(void *)
                  GetProcAddress(g_mod, "vk_icdGetInstanceProcAddr");

    if (!g_negotiate) {
        Fail("vk_icdNegotiateLoaderICDInterfaceVersion is not exported");
        return 0;
    }
    if (!g_gipa) {
        Fail("vk_icdGetInstanceProcAddr is not exported");
        return 0;
    }
    if (!GetProcAddress(g_mod, "vk_icdGetPhysicalDeviceProcAddr")) {
        Fail("vk_icdGetPhysicalDeviceProcAddr is not exported");
        return 0;
    }
    Note("exports            negotiate, getInstanceProcAddr, getPhysicalDeviceProcAddr");
    return 1;
}

/* ------------------------------------------------------------------ */
/* V2  loader interface version                                        */
/* ------------------------------------------------------------------ */

static int Negotiate(void)
{
    uint32_t ver = 7;   /* what Mesa's vk_instance.c reports */
    VkResult r;

    Stage("V2 loader/ICD interface");

    r = g_negotiate(&ver);
    if (r != VK_SUCCESS) {
        Fail("negotiation -> %s", ResultName(r));
        return 0;
    }
    if (ver < 5) {
        Fail("interface version %u is too old (need >= 5)", ver);
        return 0;
    }
    Note("interface version  %u", ver);
    return 1;
}

/* ------------------------------------------------------------------ */
/* V3  instance                                                        */
/* ------------------------------------------------------------------ */

static int CreateInstance(void)
{
    PFN_vkEnumerateInstanceVersion pVersion;
    PFN_vkCreateInstance           pCreate;
    VkApplicationInfo              app;
    VkInstanceCreateInfo           ici;
    VkResult                       r;
    uint32_t                       api = 0;

    Stage("V3 vkCreateInstance");

    pVersion = (PFN_vkEnumerateInstanceVersion)g_gipa(NULL, "vkEnumerateInstanceVersion");
    if (pVersion && pVersion(&api) == VK_SUCCESS) {
        Note("icd apiVersion     %u.%u.%u",
             VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
             VK_API_VERSION_PATCH(api));
    } else {
        Note("icd apiVersion     (vkEnumerateInstanceVersion unavailable)");
    }

    pCreate = (PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance");
    if (!pCreate) {
        Fail("vkCreateInstance is not resolvable without an instance");
        return 0;
    }

    memset(&app, 0, sizeof(app));
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "panvk_smoke";
    app.applicationVersion = 1;
    app.pEngineName        = "panvk_smoke";
    app.engineVersion      = 1;
    app.apiVersion         = VK_API_VERSION_1_0;

    memset(&ici, 0, sizeof(ici));
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    r = pCreate(&ici, NULL, &g_instance);
    if (r != VK_SUCCESS || !g_instance) {
        Fail("vkCreateInstance -> %s", ResultName(r));
        return 0;
    }
    Note("instance           created (Vulkan 1.0)");
    return 1;
}

/* ------------------------------------------------------------------ */
/* V4/V5/V6  physical device                                           */
/* ------------------------------------------------------------------ */

static int EnumerateDevice(VkPhysicalDevice *out)
{
    PFN_vkEnumeratePhysicalDevices pEnum;
    VkPhysicalDevice               devices[8];
    uint32_t                       count = 0;
    VkResult                       r;

    Stage("V4 vkEnumeratePhysicalDevices");

    pEnum = (PFN_vkEnumeratePhysicalDevices)
            g_gipa(g_instance, "vkEnumeratePhysicalDevices");
    if (!pEnum) {
        Fail("vkEnumeratePhysicalDevices is not resolvable");
        return 0;
    }

    r = pEnum(g_instance, &count, NULL);
    if (r != VK_SUCCESS) {
        Fail("query -> %s", ResultName(r));
        Note("that is the ICD failing to open a device, not an empty machine:");
        Note("see the V0 KMD interface count above, then the ICD's stderr log.");
        return 0;
    }
    Note("adapters           %u", count);
    if (count == 0) {
        Fail("no physical device: the ICD found no usable KMD node");
        Note("the kbase node is /dev/mali0; check that mali_drm.cpp's open()");
        Note("accepted it and that kbase VERSION_CHECK succeeded.");
        return 0;
    }

    if (count > (uint32_t)(sizeof(devices) / sizeof(devices[0])))
        count = (uint32_t)(sizeof(devices) / sizeof(devices[0]));
    r = pEnum(g_instance, &count, devices);
    if (r != VK_SUCCESS) {
        Fail("list -> %s", ResultName(r));
        return 0;
    }

    *out = devices[0];
    return 1;
}

static void ReportDevice(VkPhysicalDevice phys)
{
    PFN_vkGetPhysicalDeviceProperties        pProps;
    PFN_vkGetPhysicalDeviceMemoryProperties  pMem;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties pQueues;
    VkPhysicalDeviceProperties               props;
    VkPhysicalDeviceMemoryProperties         mem;
    VkQueueFamilyProperties                  queues[8];
    uint32_t                                 nq = 0, i, j;

    Stage("V5 physical device properties");

    pProps = (PFN_vkGetPhysicalDeviceProperties)
             g_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (pProps) {
        memset(&props, 0, sizeof(props));
        pProps(phys, &props);
        Note("deviceName         %s", props.deviceName);
        Note("vendorID           0x%04X", props.vendorID);
        Note("deviceID           0x%04X", props.deviceID);
        Note("apiVersion         %u.%u.%u",
             VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));
        Note("driverVersion      0x%08X", props.driverVersion);
        Note("deviceType         %u", (unsigned)props.deviceType);
    } else {
        Fail("vkGetPhysicalDeviceProperties is not resolvable");
    }

    pMem = (PFN_vkGetPhysicalDeviceMemoryProperties)
           g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (pMem) {
        memset(&mem, 0, sizeof(mem));
        pMem(phys, &mem);
        Note("memory types       %u in %u heap(s)",
             mem.memoryTypeCount, mem.memoryHeapCount);
        for (i = 0; i < mem.memoryTypeCount && i < 8; i++) {
            Note("  type %u           0x%02X %s%s%s", i,
                 (unsigned)mem.memoryTypes[i].propertyFlags,
                 (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)  ? "device-local " : "",
                 (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)  ? "host-visible " : "",
                 (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "coherent"     : "");
        }
    }

    Stage("V6 queue families");

    pQueues = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
              g_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!pQueues) {
        Fail("vkGetPhysicalDeviceQueueFamilyProperties is not resolvable");
        return;
    }
    pQueues(phys, &nq, NULL);
    Note("families           %u", nq);
    if (nq > (uint32_t)(sizeof(queues) / sizeof(queues[0])))
        nq = (uint32_t)(sizeof(queues) / sizeof(queues[0]));
    pQueues(phys, &nq, queues);
    for (j = 0; j < nq; j++) {
        Note("  family %u          %u queue(s), flags 0x%02X%s%s%s", j,
             queues[j].queueCount, (unsigned)queues[j].queueFlags,
             (queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? " graphics" : "",
             (queues[j].queueFlags & VK_QUEUE_COMPUTE_BIT)  ? " compute"  : "",
             (queues[j].queueFlags & VK_QUEUE_TRANSFER_BIT) ? " transfer" : "");
    }
}

/* ------------------------------------------------------------------ */
/* V7  device                                                          */
/* ------------------------------------------------------------------ */

static int CreateDevice(VkPhysicalDevice phys, VkDevice *out,
                        uint32_t *queueFamily)
{
    PFN_vkGetPhysicalDeviceQueueFamilyProperties pQueues;
    PFN_vkCreateDevice                            pCreate;
    VkQueueFamilyProperties                       queues[8];
    VkDeviceQueueCreateInfo                       qci;
    VkDeviceCreateInfo                            dci;
    uint32_t                                      nq = 0, i, chosen = 0;
    static float                                  prio = 1.0f;
    VkResult                                      r;

    Stage("V7 vkCreateDevice");

    pQueues = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
              g_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!pQueues) {
        Fail("queue family query unavailable");
        return 0;
    }
    pQueues(phys, &nq, NULL);
    if (nq > (uint32_t)(sizeof(queues) / sizeof(queues[0])))
        nq = (uint32_t)(sizeof(queues) / sizeof(queues[0]));
    pQueues(phys, &nq, queues);

    chosen = 0;
    for (i = 0; i < nq; i++) {
        if ((queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && queues[i].queueCount) {
            chosen = i;
            break;
        }
    }

    pCreate = (PFN_vkCreateDevice)g_gipa(g_instance, "vkCreateDevice");
    if (!pCreate) {
        Fail("vkCreateDevice is not resolvable");
        return 0;
    }

    memset(&qci, 0, sizeof(qci));
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = chosen;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &prio;

    memset(&dci, 0, sizeof(dci));
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    r = pCreate(phys, &dci, NULL, out);
    if (r != VK_SUCCESS || !*out) {
        Fail("vkCreateDevice -> %s", ResultName(r));
        return 0;
    }
    Note("device             created (family %u)", chosen);
    *queueFamily = chosen;
    return 1;
}

/* ------------------------------------------------------------------ */
/* V8-V10  buffer fill round trip                                      */
/* ------------------------------------------------------------------ */

#define FILL_SIZE    (64u * 1024u)
#define FILL_WORD    0xC0FFEE01u
#define FILL_WAIT_NS (5ull * 1000ull * 1000ull * 1000ull)

static int FindMemoryType(const VkPhysicalDeviceMemoryProperties *mem,
                          uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < mem->memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        if ((mem->memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    }
    for (i = 0; i < mem->memoryTypeCount; i++) {
        if (bits & (1u << i))
            return (int)i;
    }
    return -1;
}

static int FillRoundTrip(VkPhysicalDevice phys, VkDevice dev, uint32_t family)
{
    PFN_vkCreateBuffer             pCreateBuffer = NULL;
    PFN_vkGetBufferMemoryRequirements pReqs = NULL;
    PFN_vkAllocateMemory           pAlloc = NULL;
    PFN_vkBindBufferMemory         pBind = NULL;
    PFN_vkMapMemory                pMap = NULL;
    PFN_vkUnmapMemory              pUnmap = NULL;
    PFN_vkCreateCommandPool        pCreatePool = NULL;
    PFN_vkAllocateCommandBuffers   pAllocCmd = NULL;
    PFN_vkBeginCommandBuffer       pBegin = NULL;
    PFN_vkEndCommandBuffer         pEnd = NULL;
    PFN_vkCmdFillBuffer            pFill = NULL;
    PFN_vkCreateFence              pCreateFence = NULL;
    PFN_vkQueueSubmit              pSubmit = NULL;
    PFN_vkWaitForFences            pWait = NULL;
    PFN_vkGetDeviceQueue           pGetQueue = NULL;
    PFN_vkGetPhysicalDeviceMemoryProperties pMemProps = NULL;

    VkPhysicalDeviceMemoryProperties mem;
    VkBufferCreateInfo               bci;
    VkMemoryRequirements             reqs;
    VkMemoryAllocateInfo             mai;
    VkCommandPoolCreateInfo          pci;
    VkCommandBufferAllocateInfo      cai;
    VkCommandBufferBeginInfo         cbi;
    VkFenceCreateInfo                fci;
    VkSubmitInfo                     si;

    VkBuffer       buf  = VK_NULL_HANDLE;
    VkDeviceMemory memh = VK_NULL_HANDLE;
    VkCommandPool  pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence        fence = VK_NULL_HANDLE;
    VkQueue        queue = VK_NULL_HANDLE;
    void          *map = NULL;
    int            type, ok = 0;
    uint32_t      *words, i;
    VkResult       r;
    ULONGLONG      t0, t1;

#define RESOLVE(vkname, var)                                                 \
    var = (PFN_vk##vkname)g_gipa(g_instance, "vk" #vkname);                   \
    if (!var) { Fail("vk" #vkname " is not resolvable"); goto done; }

    Stage("V8 buffer + memory");

    RESOLVE(CreateBuffer, pCreateBuffer);
    RESOLVE(GetBufferMemoryRequirements, pReqs);
    RESOLVE(AllocateMemory, pAlloc);
    RESOLVE(BindBufferMemory, pBind);
    RESOLVE(MapMemory, pMap);
    RESOLVE(UnmapMemory, pUnmap);
    RESOLVE(CreateCommandPool, pCreatePool);
    RESOLVE(AllocateCommandBuffers, pAllocCmd);
    RESOLVE(BeginCommandBuffer, pBegin);
    RESOLVE(EndCommandBuffer, pEnd);
    RESOLVE(CmdFillBuffer, pFill);
    RESOLVE(CreateFence, pCreateFence);
    RESOLVE(QueueSubmit, pSubmit);
    RESOLVE(WaitForFences, pWait);
    RESOLVE(GetDeviceQueue, pGetQueue);
#undef RESOLVE

    pMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
                g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pMemProps) { Fail("memory properties unavailable"); goto done; }
    memset(&mem, 0, sizeof(mem));
    pMemProps(phys, &mem);

    memset(&bci, 0, sizeof(bci));
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = FILL_SIZE;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    r = pCreateBuffer(dev, &bci, NULL, &buf);
    if (r != VK_SUCCESS) { Fail("vkCreateBuffer -> %s", ResultName(r)); goto done; }

    memset(&reqs, 0, sizeof(reqs));
    pReqs(dev, buf, &reqs);
    Note("buffer             %u bytes, needs %llu bytes, typeBits 0x%X",
         FILL_SIZE, (unsigned long long)reqs.size, reqs.memoryTypeBits);

    type = FindMemoryType(&mem, reqs.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0) { Fail("no host-visible memory type for the buffer"); goto done; }

    memset(&mai, 0, sizeof(mai));
    mai.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = reqs.size;
    mai.memoryTypeIndex = (uint32_t)type;

    r = pAlloc(dev, &mai, NULL, &memh);
    if (r != VK_SUCCESS) { Fail("vkAllocateMemory -> %s", ResultName(r)); goto done; }
    Note("memory             type %d, %llu bytes",
         type, (unsigned long long)reqs.size);

    r = pBind(dev, buf, memh, 0);
    if (r != VK_SUCCESS) { Fail("vkBindBufferMemory -> %s", ResultName(r)); goto done; }

    r = pMap(dev, memh, 0, VK_WHOLE_SIZE, 0, &map);
    if (r != VK_SUCCESS || !map) { Fail("vkMapMemory -> %s", ResultName(r)); goto done; }

    words = (uint32_t *)map;
    for (i = 0; i < FILL_SIZE / 4; i++)
        words[i] = 0xDEADBEEFu;
    pUnmap(dev, memh);
    map = NULL;
    Note("cpu                wrote 0xDEADBEEF, unmapped");

    Stage("V9 vkCmdFillBuffer + submit");

    memset(&pci, 0, sizeof(pci));
    pci.sType      = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    r = pCreatePool(dev, &pci, NULL, &pool);
    if (r != VK_SUCCESS) { Fail("vkCreateCommandPool -> %s", ResultName(r)); goto done; }

    memset(&cai, 0, sizeof(cai));
    cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool        = pool;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = pAllocCmd(dev, &cai, &cmd);
    if (r != VK_SUCCESS) { Fail("vkAllocateCommandBuffers -> %s", ResultName(r)); goto done; }

    memset(&cbi, 0, sizeof(cbi));
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = pBegin(cmd, &cbi);
    if (r != VK_SUCCESS) { Fail("vkBeginCommandBuffer -> %s", ResultName(r)); goto done; }

    pFill(cmd, buf, 0, VK_WHOLE_SIZE, FILL_WORD);

    r = pEnd(cmd);
    if (r != VK_SUCCESS) { Fail("vkEndCommandBuffer -> %s", ResultName(r)); goto done; }

    memset(&fci, 0, sizeof(fci));
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = pCreateFence(dev, &fci, NULL, &fence);
    if (r != VK_SUCCESS) { Fail("vkCreateFence -> %s", ResultName(r)); goto done; }

    pGetQueue(dev, family, 0, &queue);
    if (!queue) { Fail("vkGetDeviceQueue returned NULL"); goto done; }

    memset(&si, 0, sizeof(si));
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;

    t0 = GetTickCount64();
    r = pSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) { Fail("vkQueueSubmit -> %s", ResultName(r)); goto done; }

    r = pWait(dev, 1, &fence, VK_TRUE, FILL_WAIT_NS);
    t1 = GetTickCount64();
    if (r != VK_SUCCESS) {
        Fail("vkWaitForFences -> %s after %llu ms",
             ResultName(r), (unsigned long long)(t1 - t0));
        Note("a hang here means the job never completed: check the KMD job");
        Note("status and the MMU fault registers before anything else.");
        goto done;
    }
    Note("submit             completed in %llu ms",
         (unsigned long long)(t1 - t0));

    Stage("V10 read back");

    r = pMap(dev, memh, 0, VK_WHOLE_SIZE, 0, &map);
    if (r != VK_SUCCESS || !map) { Fail("vkMapMemory -> %s", ResultName(r)); goto done; }

    words = (uint32_t *)map;
    for (i = 0; i < FILL_SIZE / 4; i++) {
        if (words[i] != FILL_WORD) {
            Fail("word %u is 0x%08X, expected 0x%08X", i, words[i], FILL_WORD);
            break;
        }
    }
    if (i == FILL_SIZE / 4)
        Note("pattern            %u words of 0x%08X verified", i, FILL_WORD);
    pUnmap(dev, memh);
    map = NULL;

    ok = (i == FILL_SIZE / 4);

done:
    if (map)   pUnmap(dev, memh);
    if (fence) { PFN_vkDestroyFence d = (PFN_vkDestroyFence)g_gipa(g_instance, "vkDestroyFence"); if (d) d(dev, fence, NULL); }
    if (pool)  { PFN_vkDestroyCommandPool d = (PFN_vkDestroyCommandPool)g_gipa(g_instance, "vkDestroyCommandPool"); if (d) d(dev, pool, NULL); }
    if (buf)   { PFN_vkDestroyBuffer d = (PFN_vkDestroyBuffer)g_gipa(g_instance, "vkDestroyBuffer"); if (d) d(dev, buf, NULL); }
    if (memh)  { PFN_vkFreeMemory d = (PFN_vkFreeMemory)g_gipa(g_instance, "vkFreeMemory"); if (d) d(dev, memh, NULL); }
    return ok;
}

/* ------------------------------------------------------------------ */
/* run                                                                 */
/* ------------------------------------------------------------------ */

static int Run(const char *dllOverride)
{
    char           dllPath[MAX_PATH];
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice       dev    = VK_NULL_HANDLE;
    uint32_t       family = 0;
    int            pass   = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("panvk_smoke: PanVK (vulkan_panfrost.dll) first-run validation\n");
    printf("MT6789 Mali-G57 MC2 / Valhall v9, Windows ARM64\n");

    /* The v9 gate in panvk_physical_device.c, and nothing else, needs this.
     * _putenv_s() updates both the CRT environment and the process block. */
    _putenv_s("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1");

    if (dllOverride && dllOverride[0]) {
        snprintf(dllPath, sizeof(dllPath), "%s", dllOverride);
    } else if (!Preflight(dllPath, sizeof(dllPath))) {
        goto done;
    }

    if (!LoadIcd(dllPath))       goto done;
    if (!Negotiate())            goto done;
    if (!CreateInstance())       goto done;
    if (!EnumerateDevice(&phys)) goto done;

    ReportDevice(phys);

    if (!CreateDevice(phys, &dev, &family)) goto done;
    pass = FillRoundTrip(phys, dev, family);

done:
    if (dev) {
        PFN_vkDestroyDevice d = (PFN_vkDestroyDevice)g_gipa(g_instance, "vkDestroyDevice");
        if (d) d(dev, NULL);
    }
    if (g_instance) {
        PFN_vkDestroyInstance d = (PFN_vkDestroyInstance)g_gipa(g_instance, "vkDestroyInstance");
        if (d) d(g_instance, NULL);
    }
    if (g_mod)
        FreeLibrary(g_mod);

    printf("\n");
    if (g_fail || !pass) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}

int main(int argc, char **argv)
{
    return Run(argc > 1 ? argv[1] : NULL);
}
