<#
  build-icd.ps1  -  Build the Mali-G57 Vulkan ICD (mali_vk_icd.dll) for ARM64.

  Output (into windrv\umd\build):
      mali_vk_icd.dll

  Requires the Khronos Vulkan-Headers; run tools\fetch-deps.ps1 first (they land
  in the git-ignored external\Vulkan-Headers). Toolchain discovery is shared with
  the KMD (kmd\tools\winsdk.ps1); every path can be overridden with a parameter
  or an environment variable.

      -VulkanHeaders <path to the Vulkan-Headers checkout>
      -KitsRoot / $env:WINDOWSSDKDIR
      -WdkRoot  / $env:WDK_ROOT
      -Clang    / $env:MALI_CLANG
      -Linker   / $env:MALI_LINKER
#>
[CmdletBinding()]
param(
    [string]$VulkanHeaders = $env:VULKAN_HEADERS,
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path        # windrv\umd\vulkan\tools
$vulkan   = Split-Path -Parent $here                               # windrv\umd\vulkan
$umd      = Split-Path -Parent $vulkan                             # windrv\umd
$windrv   = Split-Path -Parent $umd                                # windrv
$repoRoot = Split-Path -Parent $windrv                             # repo root

# Locate Vulkan-Headers: parameter, env, or the fetched external\ copy.
if (-not $VulkanHeaders) {
    $candidates = @( (Join-Path $repoRoot 'external\Vulkan-Headers') )
    if ($env:VULKAN_SDK) { $candidates += (Join-Path $env:VULKAN_SDK 'Include') }
    foreach ($c in $candidates) {
        if (-not $c) { continue }
        if ((Test-Path -LiteralPath (Join-Path $c 'vulkan\vulkan.h')) -or
            (Test-Path -LiteralPath (Join-Path $c 'include\vulkan\vulkan.h'))) {
            $VulkanHeaders = $c; break
        }
    }
}
if (-not $VulkanHeaders) { throw 'Vulkan-Headers not found. Run tools\fetch-deps.ps1 or pass -VulkanHeaders <path>.' }

$vkInc = if (Test-Path -LiteralPath (Join-Path $VulkanHeaders 'include\vulkan\vulkan.h')) {
    Join-Path $VulkanHeaders 'include'
} else { $VulkanHeaders }
if (-not (Test-Path -LiteralPath (Join-Path $vkInc 'vulkan\vulkan.h'))) {
    throw ("Vulkan headers missing under {0}" -f $VulkanHeaders)
}

Push-Location $vulkan
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $clang = $t.ClangCl
    if (-not $clang -or -not (Test-Path -LiteralPath $clang)) {
        throw (("clang-cl.exe not found next to {0}. Install LLVM or the Visual Studio " +
                "'C++ Clang tools for Windows', or pass -Clang <path to clang.exe>.") -f $t.Clang)
    }

    $out = Join-Path $umd 'build'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $inc = @(
        '/I', (Join-Path $vulkan 'include'),
        '/I', (Join-Path $umd    'include'),
        '/I', (Join-Path $windrv 'kmd\include'),
        '/imsvc', $vkInc
    )

    $common = @(
        '--target=aarch64-pc-windows-msvc','/nologo','/c','/O2','/W4','/GS-','/Zl','-fno-builtin',
        '/D_ARM64_','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0a00000e',
        '/DVK_USE_PLATFORM_WIN32_KHR'
    ) + $inc + @(
        '/imsvc', $t.SdkUm,
        '/imsvc', $t.SdkShared,
        '/imsvc', $t.SdkUcrt
    )

    $srcs = @(
        'src\icd_entry.c',
        'src\icd_device.c',
        'src\icd_queue.c',
        'src\icd_wsi.c',
        (Join-Path $umd 'src\mali_umd.c'),
        (Join-Path $umd 'src\mali_jc.c')
    )

    $objs = @()
    foreach ($src in $srcs) {
        $full = if ([System.IO.Path]::IsPathRooted($src)) { $src } else { Join-Path $vulkan $src }
        $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
        $obj  = Join-Path $out ('icd_' + $name + '.obj')
        Write-Host ("CC  {0}" -f $src)
        & $clang @common "/Fo$obj" $full
        if ($LASTEXITCODE -ne 0) { throw ("compile failed: {0}" -f $src) }
        $objs += $obj
    }

    if (-not (Test-Path -LiteralPath $t.SdkUmLibArm64)) {
        throw (("Windows SDK ARM64 user-mode libs not found: {0}.") -f $t.SdkUmLibArm64)
    }

    $dll = Join-Path $out 'mali_vk_icd.dll'
    $def = Join-Path $vulkan 'src\mali_vk_icd.def'
    $lc = @(
        '/nologo','/dll','/machine:arm64','/entry:DllMain','/nodefaultlib','/debug',
        ("/libpath:{0}" -f $t.SdkUmLibArm64),
        ("/def:{0}" -f $def),
        ("/out:{0}" -f $dll)
    ) + $objs + @('kernel32.lib','setupapi.lib','advapi32.lib','gdi32.lib','user32.lib')
    & $t.Linker @lc
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }

    # Stage the loader manifest next to the DLL.
    Copy-Item -LiteralPath (Join-Path $vulkan 'manifest\mali_g57_icd.json') `
              -Destination (Join-Path $out 'mali_g57_icd.json') -Force

    # Smoke test: links the ICD import library and resolves entry points itself,
    # so it exercises the ICD directly (no Khronos loader required).
    $smokeObj = Join-Path $out 'icd_mali_vk_smoke.obj'
    Write-Host ("CC  {0}" -f 'test\mali_vk_smoke.c')
    & $clang @common "/Fo$smokeObj" (Join-Path $vulkan 'test\mali_vk_smoke.c')
    if ($LASTEXITCODE -ne 0) { throw 'compile failed: test\mali_vk_smoke.c' }

    $smoke = Join-Path $out 'mali_vk_smoke.exe'
    $sll = @(
        '/nologo','/subsystem:console','/machine:arm64','/entry:mainCRTStartup','/nodefaultlib','/debug',
        ("/libpath:{0}" -f $t.SdkUmLibArm64),
        ("/out:{0}" -f $smoke)
    ) + @($smokeObj) + @((Join-Path $out 'mali_vk_icd.lib'), 'kernel32.lib', 'user32.lib')
    & $t.Linker @sll
    if ($LASTEXITCODE -ne 0) { throw 'link failed: mali_vk_smoke.exe' }

    Write-Host ("OK: {0} ({1} bytes)" -f $dll, (Get-Item -LiteralPath $dll).Length) -ForegroundColor Green
    Write-Host ("    manifest: {0}" -f (Join-Path $out 'mali_g57_icd.json'))
    Write-Host ("    smoke:    {0} ({1} bytes)" -f $smoke, (Get-Item -LiteralPath $smoke).Length)
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
