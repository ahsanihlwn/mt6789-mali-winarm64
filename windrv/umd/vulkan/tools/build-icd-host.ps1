<#
  build-icd-host.ps1  -  Build and run the ICD entry-point self-test on the
                         development machine (x64, no Mali hardware needed).

  This compiles the same ICD sources as build-icd.ps1, but for x64, into
  windrv\umd\build\x64, then builds and runs test\mali_vk_iface.c. It proves
  that every entry point the ICD advertises resolves through
  vkGetInstanceProcAddr and that the physical-device query code runs without
  faulting. It does NOT exercise anything that needs the KMD (device creation,
  BOs, submissions) - that is mali_vk_smoke.exe on the tablet.

  Requires the Khronos Vulkan-Headers (run tools\fetch-deps.ps1 first).
#>
[CmdletBinding()]
param(
    [string]$VulkanHeaders = $env:VULKAN_HEADERS,
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER,
    [switch]$NoRun
)

$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$vulkan   = Split-Path -Parent $here
$umd      = Split-Path -Parent $vulkan
$windrv   = Split-Path -Parent $umd
$repoRoot = Split-Path -Parent $windrv

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

Push-Location $vulkan
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $clang = $t.ClangCl
    if (-not $clang -or -not (Test-Path -LiteralPath $clang)) {
        throw ("clang-cl.exe not found next to {0}." -f $t.Clang)
    }

    $umLibX64 = Join-Path $t.KitsRoot ("Lib\{0}\um\x64" -f $t.SdkVer)
    if (-not (Test-Path -LiteralPath $umLibX64)) {
        throw ("Windows SDK x64 user-mode libs not found: {0}" -f $umLibX64)
    }

    $out = Join-Path $umd 'build\x64'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $inc = @(
        '/I', (Join-Path $vulkan 'include'),
        '/I', (Join-Path $umd    'include'),
        '/I', (Join-Path $windrv 'kmd\include'),
        '/imsvc', $vkInc
    )

    $common = @(
        '/nologo','/c','/O2','/W4','/GS-','/Zl','-fno-builtin',
        '/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0a00000e',
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

    $dll = Join-Path $out 'mali_vk_icd.dll'
    $def = Join-Path $vulkan 'src\mali_vk_icd.def'
    $lc = @(
        '/nologo','/dll','/machine:x64','/entry:DllMain','/nodefaultlib','/debug',
        ("/libpath:{0}" -f $umLibX64),
        ("/def:{0}" -f $def),
        ("/out:{0}" -f $dll)
    ) + $objs + @('kernel32.lib','setupapi.lib','advapi32.lib')
    & $t.Linker @lc
    if ($LASTEXITCODE -ne 0) { throw 'link failed: mali_vk_icd.dll (x64)' }

    $ifaceObj = Join-Path $out 'icd_mali_vk_iface.obj'
    Write-Host ("CC  {0}" -f 'test\mali_vk_iface.c')
    & $clang @common "/Fo$ifaceObj" (Join-Path $vulkan 'test\mali_vk_iface.c')
    if ($LASTEXITCODE -ne 0) { throw 'compile failed: test\mali_vk_iface.c' }

    $iface = Join-Path $out 'mali_vk_iface.exe'
    $ill = @(
        '/nologo','/subsystem:console','/machine:x64','/entry:mainCRTStartup','/nodefaultlib','/debug',
        ("/libpath:{0}" -f $umLibX64),
        ("/out:{0}" -f $iface)
    ) + @($ifaceObj) + @((Join-Path $out 'mali_vk_icd.lib'), 'kernel32.lib')
    & $t.Linker @ill
    if ($LASTEXITCODE -ne 0) { throw 'link failed: mali_vk_iface.exe' }

    Write-Host ("OK: {0} ({1} bytes)" -f $dll, (Get-Item -LiteralPath $dll).Length) -ForegroundColor Green
    Write-Host ("    iface: {0} ({1} bytes)" -f $iface, (Get-Item -LiteralPath $iface).Length)

    if (-not $NoRun) {
        Write-Host ''
        & $iface
        if ($LASTEXITCODE -ne 0) { throw ("mali_vk_iface failed with exit code {0}" -f $LASTEXITCODE) }
    }
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}