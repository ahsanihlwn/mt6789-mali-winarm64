/*
 * directx/d3d12.h  -  redirect to the Windows SDK's <d3d12.h>.
 *
 * Mesa's win32 WSI (src/vulkan/wsi/wsi_common_win32.cpp) includes
 * <directx/d3d12.h>, the layout used by the DirectX-Headers subproject. Mesa
 * only resolves that dependency when a D3D12 backend is enabled, so in a
 * Vulkan-only build `dep_dxheaders` is a null dependency and the header is not
 * on the include path. The Windows SDK ships the same header as <d3d12.h>
 * (um/); this redirect lets the win32 WSI compile without pulling in
 * DirectX-Headers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_DIRECTX_D3D12_H
#define MALI_COMPAT_DIRECTX_D3D12_H

#include <d3d12.h>

#endif /* MALI_COMPAT_DIRECTX_D3D12_H */