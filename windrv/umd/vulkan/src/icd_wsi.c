/*
 * icd_wsi.c  -  Win32 window-system integration for the Mali-G57 ICD.
 *
 * Implements VK_KHR_surface + VK_KHR_win32_surface + VK_KHR_swapchain.
 *
 * Present is a GDI blit (StretchDIBits) from the swapchain image's CPU mapping
 * into the window DC. That is the L2 rung of windrv/DISPLAY-ANDROID.md: Mali has
 * no scan-out, and on this machine the panel is driven by the Microsoft Basic
 * Display Adapter, so a copy through GDI needs no WDDM and no display miniport.
 * The price is one CPU copy per frame; that is documented and accepted.
 *
 * There is no real vblank/fence wait here: vkQueueSubmit is synchronous, so an
 * image is by definition complete when the application presents it. Acquire
 * signals its semaphore/fence immediately. When the KMD grows a real scheduler
 * (IRQ + timeline fences), only MaliVkWaitImageReady() has to change.
 *
 * SPDX-License-Identifier: MIT
 */
#define WIN32_LEAN_AND_MEAN
#include "mali_vk.h"

/* ------------------------------------------------------------------ */
/* Surface                                                             */
/* ------------------------------------------------------------------ */

VKAPI_ATTR VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(
    VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    MaliVkSurface *surf;
    (void)pAllocator;

    if (!pCreateInfo || !pSurface) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo->hwnd)        return VK_ERROR_INITIALIZATION_FAILED;

    surf = (MaliVkSurface *)MaliVkAlloc(sizeof(*surf));
    if (!surf) return VK_ERROR_OUT_OF_HOST_MEMORY;

    surf->Inst = (MaliVkInstance *)instance;
    surf->Hwnd = pCreateInfo->hwnd;

    *pSurface = (VkSurfaceKHR)surf;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *pAllocator)
{
    (void)instance; (void)pAllocator;
    MaliVkFree((void *)surface);
}

VKAPI_ATTR VkBool32 VKAPI_CALL vkGetPhysicalDeviceWin32PresentationSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex)
{
    (void)physicalDevice;
    /* We expose a single graphics+compute family, and GDI presents on the CPU,
     * so every family we have can present. */
    return queueFamilyIndex == 0u ? VK_TRUE : VK_FALSE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
    VkSurfaceKHR surface, VkBool32 *pSupported)
{
    (void)physicalDevice;
    if (!surface || !pSupported) return VK_ERROR_INITIALIZATION_FAILED;
    *pSupported = (queueFamilyIndex == 0u) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

/* Current size of the window's client area; 0 on failure. */
static int MaliVkClientSize(HWND hwnd, uint32_t *pW, uint32_t *pH)
{
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) return 0;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return 0;
    if (pW) *pW = (uint32_t)(rc.right - rc.left);
    if (pH) *pH = (uint32_t)(rc.bottom - rc.top);
    return 1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *pCapabilities)
{
    MaliVkSurface *surf = (MaliVkSurface *)surface;
    uint32_t w = 0, h = 0;
    (void)physicalDevice;

    if (!surf || !pCapabilities) return VK_ERROR_INITIALIZATION_FAILED;
    MaliVkZero(pCapabilities, sizeof(*pCapabilities));

    MaliVkClientSize(surf->Hwnd, &w, &h);

    pCapabilities->minImageCount           = 2u;
    pCapabilities->maxImageCount           = MALI_VK_MAX_SWAPCHAIN_IMAGES;
    pCapabilities->currentExtent.width     = w;
    pCapabilities->currentExtent.height    = h;
    pCapabilities->minImageExtent.width    = 1u;
    pCapabilities->minImageExtent.height   = 1u;
    pCapabilities->maxImageExtent.width    = 16384u;
    pCapabilities->maxImageExtent.height   = 16384u;
    pCapabilities->maxImageArrayLayers     = 1u;
    pCapabilities->supportedTransforms     = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCapabilities->currentTransform        = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pCapabilities->supportedUsageFlags     =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT    |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT    |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}

/*
 * Only 32-bpp BGRA: that is the one layout a BI_RGB DIB blits 1:1, so present
 * stays a straight memcpy by GDI with no per-pixel conversion.
 */
static const VkSurfaceFormatKHR g_SurfaceFormats[] = {
    { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};

#define MALI_VK_SURFACE_FORMAT_COUNT \
    (uint32_t)(sizeof(g_SurfaceFormats) / sizeof(g_SurfaceFormats[0]))

static const VkPresentModeKHR g_PresentModes[] = {
    VK_PRESENT_MODE_FIFO_KHR,
};

#define MALI_VK_PRESENT_MODE_COUNT \
    (uint32_t)(sizeof(g_PresentModes) / sizeof(g_PresentModes[0]))

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pSurfaceFormatCount, VkSurfaceFormatKHR *pSurfaceFormats)
{
    (void)physicalDevice;
    if (!surface || !pSurfaceFormatCount) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pSurfaceFormats) {
        *pSurfaceFormatCount = MALI_VK_SURFACE_FORMAT_COUNT;
        return VK_SUCCESS;
    }
    if (*pSurfaceFormatCount < MALI_VK_SURFACE_FORMAT_COUNT) {
        *pSurfaceFormatCount = MALI_VK_SURFACE_FORMAT_COUNT;
        return VK_INCOMPLETE;
    }
    {
        uint32_t i;
        for (i = 0; i < MALI_VK_SURFACE_FORMAT_COUNT; i++)
            pSurfaceFormats[i] = g_SurfaceFormats[i];
    }
    *pSurfaceFormatCount = MALI_VK_SURFACE_FORMAT_COUNT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
    uint32_t *pPresentModeCount, VkPresentModeKHR *pPresentModes)
{
    (void)physicalDevice;
    if (!surface || !pPresentModeCount) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pPresentModes) {
        *pPresentModeCount = MALI_VK_PRESENT_MODE_COUNT;
        return VK_SUCCESS;
    }
    if (*pPresentModeCount < MALI_VK_PRESENT_MODE_COUNT) {
        *pPresentModeCount = MALI_VK_PRESENT_MODE_COUNT;
        return VK_INCOMPLETE;
    }
    pPresentModes[0] = g_PresentModes[0];
    *pPresentModeCount = MALI_VK_PRESENT_MODE_COUNT;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Swapchain                                                           */
/* ------------------------------------------------------------------ */

static void MaliVkSwapchainDropImages(MaliVkSwapchain *sc)
{
    uint32_t i;
    if (!sc) return;
    for (i = 0; i < MALI_VK_MAX_SWAPCHAIN_IMAGES; i++) {
        if (sc->Mem[i]) {
            if (sc->Mem[i]->Bo.Handle && sc->Dev && sc->Dev->Umd)
                MaliUmdBoFree(sc->Dev->Umd, sc->Mem[i]->Bo.Handle);
            MaliVkFree(sc->Mem[i]);
            sc->Mem[i] = NULL;
        }
        if (sc->Images[i]) {
            MaliVkFree(sc->Images[i]);
            sc->Images[i] = NULL;
        }
    }
    sc->ImageCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    MaliVkDevice  *dev = (MaliVkDevice *)device;
    MaliVkSwapchain *sc;
    uint32_t i, count, w, h;
    uint64_t bytes;
    (void)pAllocator;

    if (!dev || !dev->Umd || !pCreateInfo || !pSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo->surface) return VK_ERROR_INITIALIZATION_FAILED;

    if (pCreateInfo->imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
        pCreateInfo->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;

    if (pCreateInfo->presentMode != VK_PRESENT_MODE_FIFO_KHR)
        return VK_ERROR_INCOMPATIBLE_DRIVER;

    sc = (MaliVkSwapchain *)MaliVkAlloc(sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;

    sc->Dev    = dev;
    sc->Surf   = (MaliVkSurface *)pCreateInfo->surface;
    sc->Format = pCreateInfo->imageFormat;
    sc->Current = 0;

    count = pCreateInfo->minImageCount;
    if (count < 2u) count = 2u;
    if (count > MALI_VK_MAX_SWAPCHAIN_IMAGES) count = MALI_VK_MAX_SWAPCHAIN_IMAGES;
    sc->ImageCount = count;

    w = pCreateInfo->imageExtent.width;
    h = pCreateInfo->imageExtent.height;
    if (w == 0u || h == 0u) {
        if (!MaliVkClientSize(sc->Surf->Hwnd, &w, &h)) {
            MaliVkFree(sc);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    sc->Extent.width  = w;
    sc->Extent.height = h;

    bytes = MaliVkAlign4096((uint64_t)w * (uint64_t)h * 4ull);
    if (bytes == 0ull) { MaliVkFree(sc); return VK_ERROR_INITIALIZATION_FAILED; }

    for (i = 0; i < count; i++) {
        MaliVkMemory *mem = (MaliVkMemory *)MaliVkAlloc(sizeof(*mem));
        MaliVkImage  *img = (MaliVkImage  *)MaliVkAlloc(sizeof(*img));

        if (!mem || !img) {
            if (mem) MaliVkFree(mem);
            if (img) MaliVkFree(img);
            MaliVkSwapchainDropImages(sc);
            MaliVkFree(sc);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        mem->Dev = dev;
        if (!MaliUmdBoAlloc(dev->Umd, bytes,
                            MALI_BO_CPU_RD | MALI_BO_CPU_WR | MALI_BO_GPU_RD | MALI_BO_GPU_WR,
                            &mem->Bo)) {
            MaliVkFree(mem);
            MaliVkFree(img);
            MaliVkSwapchainDropImages(sc);
            MaliVkFree(sc);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }

        img->Dev         = dev;
        img->Type        = VK_IMAGE_TYPE_2D;
        img->Format      = sc->Format;
        img->Extent.width  = w;
        img->Extent.height = h;
        img->Extent.depth  = 1u;
        img->MipLevels   = 1u;
        img->ArrayLayers = 1u;
        img->Samples     = VK_SAMPLE_COUNT_1_BIT;
        img->Tiling      = VK_IMAGE_TILING_OPTIMAL;
        img->Usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT    |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT    |
                           VK_IMAGE_USAGE_SAMPLED_BIT;
        img->Size        = bytes;
        img->Mem         = mem;
        img->MemOffset   = 0ull;

        sc->Mem[i]    = mem;
        sc->Images[i] = img;
    }

    *pSwapchain = (VkSwapchainKHR)sc;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator)
{
    MaliVkSwapchain *sc = (MaliVkSwapchain *)swapchain;
    (void)device; (void)pAllocator;
    if (!sc) return;
    MaliVkSwapchainDropImages(sc);
    MaliVkFree(sc);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain,
    uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
    MaliVkSwapchain *sc = (MaliVkSwapchain *)swapchain;
    (void)device;
    if (!sc || !pSwapchainImageCount) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->ImageCount;
        return VK_SUCCESS;
    }
    if (*pSwapchainImageCount < sc->ImageCount) {
        *pSwapchainImageCount = sc->ImageCount;
        return VK_INCOMPLETE;
    }
    {
        uint32_t i;
        for (i = 0; i < sc->ImageCount; i++)
            pSwapchainImages[i] = (VkImage)sc->Images[i];
    }
    *pSwapchainImageCount = sc->ImageCount;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    MaliVkSwapchain *sc = (MaliVkSwapchain *)swapchain;
    uint32_t idx;
    (void)device; (void)timeout;

    if (!sc || !pImageIndex || sc->ImageCount == 0u) return VK_ERROR_INITIALIZATION_FAILED;

    idx = sc->Current;
    sc->Current = (sc->Current + 1u) % sc->ImageCount;

    /* Completion is synchronous today, so the image is ready now. */
    if (semaphore) ((MaliVkSemaphore *)semaphore)->Signaled = 1u;
    if (fence)     ((MaliVkFence *)fence)->Signaled = 1u;

    *pImageIndex = idx;
    return VK_SUCCESS;
}

/* Copy one swapchain image to the window. */
static VkResult MaliVkPresentImage(MaliVkSwapchain *sc, uint32_t index)
{
    MaliVkImage *img;
    BITMAPINFO bi;
    HDC dc;
    uint32_t dw = 0, dh = 0;
    const void *src;

    if (!sc || index >= sc->ImageCount) return VK_ERROR_INITIALIZATION_FAILED;
    img = sc->Images[index];
    if (!img || !img->Mem || !img->Mem->Bo.CpuVa) return VK_ERROR_INITIALIZATION_FAILED;

    if (!MaliVkClientSize(sc->Surf->Hwnd, &dw, &dh)) return VK_SUCCESS;  /* minimised */

    dc = GetDC(sc->Surf->Hwnd);
    if (!dc) return VK_ERROR_SURFACE_LOST_KHR;

    MaliVkZero(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = (DWORD)sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = (LONG)img->Extent.width;
    bi.bmiHeader.biHeight      = -(LONG)img->Extent.height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    src = (const void *)((const unsigned char *)img->Mem->Bo.CpuVa + img->MemOffset);

    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, (int)dw, (int)dh,
                  0, 0, (int)img->Extent.width, (int)img->Extent.height,
                  src, &bi, DIB_RGB_COLORS, SRCCOPY);

    ReleaseDC(sc->Surf->Hwnd, dc);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    VkResult first = VK_SUCCESS;
    uint32_t i;

    (void)queue;
    if (!pPresentInfo) return VK_ERROR_INITIALIZATION_FAILED;

    for (i = 0; i < pPresentInfo->swapchainCount; i++) {
        MaliVkSwapchain *sc = (MaliVkSwapchain *)pPresentInfo->pSwapchains[i];
        VkResult r = MaliVkPresentImage(sc, pPresentInfo->pImageIndices[i]);
        if (pPresentInfo->pResults) pPresentInfo->pResults[i] = r;
        if (r != VK_SUCCESS && first == VK_SUCCESS) first = r;
    }
    return first;
}