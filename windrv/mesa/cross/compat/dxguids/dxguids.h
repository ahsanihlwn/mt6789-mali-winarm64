/*
 * dxguids/dxguids.h  -  stand-in for DirectX-Headers' dxguids header.
 *
 * Mesa's win32 WSI includes <dxguids/dxguids.h> after <dxgi1_4.h> and
 * <directx/d3d12.h>. In the DirectX-Headers project that header spells the
 * DXGI/D3D12 interface GUIDs out inline (needed for MinGW, whose headers do not
 * attach __declspec(uuid(...)) to the interfaces).
 *
 * With the MSVC Windows SDK the interfaces in <dxgi1_4.h> and <d3d12.h> do
 * carry __declspec(uuid(...)), and the only GUID consumer in that file is
 * IID_PPV_ARGS from <combaseapi.h>, which expands to __uuidof. So no IID_*
 * definitions are required; the include just has to resolve.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MALI_COMPAT_DXGUIDS_H
#define MALI_COMPAT_DXGUIDS_H

#include <guiddef.h>

#endif /* MALI_COMPAT_DXGUIDS_H */