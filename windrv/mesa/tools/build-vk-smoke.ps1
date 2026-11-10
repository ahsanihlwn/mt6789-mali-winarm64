<#
  build-vk-smoke.ps1  -  build the PanVK first-run validation harness (ARM64).

  It links the same CRT-free recipe as the rest of Jalur B: clang-cl with
  /nodefaultlib plus the mali_cxxrt.lib shim (mainCRTStartup, operator new, the
  UCRT bits).  No import library for vulkan-1 is used on purpose - the harness
  LoadLibrary()s vulkan_panfrost.dll from the folder it lives in and goes
  through the ICD's loader interface, which is what the Vulkan loader does and
  what the tablet can support (it has no loader installed).

  setupapi comes from mali_umd.obj inside the shim: the V0 preflight calls
  MaliUmdEnumerate() to report whether the KMD device interface exists.  It
  never opens the device - the ICD owns it exclusively.

  Run tools\setup.ps1 and tools\build-runtime.ps1 first.

  Output: build\vk-smoke\panvk_smoke.exe
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

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot
$repoRoot = Split-Path -Parent $windrv

if (-not $VulkanHeaders) {
    foreach ($c in @((Join-Path $repoRoot 'external\Vulkan-Headers'))) {
        if (Test-Path -LiteralPath (Join-Path $c 'include\vulkan\vulkan.h')) {
            $VulkanHeaders = $c; break
        }
    }
}
if (-not $VulkanHeaders) {
    throw 'Vulkan-Headers not found (expected external\Vulkan-Headers\include\vulkan\vulkan.h)'
}

Push-Location $mesaRoot
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $binDir = Split-Path -Parent $t.ClangCl
    $lld    = Join-Path $binDir 'lld-link.exe'

    $libUm   = Join-Path $t.KitsRoot ("Lib\{0}\um\arm64"   -f $t.SdkVer)
    $libUcrt = Join-Path $t.KitsRoot ("Lib\{0}\ucrt\arm64" -f $t.SdkVer)

    $shim = Join-Path $mesaRoot 'build\runtime\mali_cxxrt.lib'
    if (-not (Test-Path -LiteralPath $shim)) { throw 'build\runtime\mali_cxxrt.lib missing; run tools\build-runtime.ps1' }

    $outDir = Join-Path $mesaRoot 'build\vk-smoke'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $src = Join-Path $mesaRoot 'tools\vk-smoke\panvk_smoke.c'
    $obj = Join-Path $outDir 'panvk_smoke.obj'
    $exe = Join-Path $outDir 'panvk_smoke.exe'

    $inc = @('/imsvc', $t.SdkUcrt, '/imsvc', $t.SdkUm, '/imsvc', $t.SdkShared,
             '/imsvc', (Join-Path $VulkanHeaders 'include'),
             '/I', (Join-Path $windrv 'umd\include'),
             '/I', (Join-Path $windrv 'kmd\include'))

    Write-Host 'compiling panvk_smoke.c (aarch64) ...'
    & $t.ClangCl (@('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/O2', '/MD',
                    '/W3', '/clang:-fno-builtin') + $inc + @("/Fo$obj", $src))
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $obj)) { throw 'compile failed' }

    if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }

    # Shim first on purpose: see runtime\mali_vcrt.cpp.
    $libs = @($shim, 'ucrt.lib', 'BufferOverflow.lib', 'msvcurt.lib',
              'kernel32.lib', 'ntdll.lib', 'user32.lib', 'gdi32.lib', 'setupapi.lib')

    Write-Host 'linking panvk_smoke.exe ...'
    $linkArgs = @('/nologo', '/machine:arm64', '/nodefaultlib', "/libpath:$libUm", "/libpath:$libUcrt",
                  "/out:$exe", $obj) + $libs
    & $lld @linkArgs
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) { throw 'link failed' }

    $size = (Get-Item -LiteralPath $exe).Length
    Write-Host ''
    Write-Host ("PanVK smoke  {0}  ({1} bytes)" -f $exe, $size) -ForegroundColor Green
    Write-Host 'BUILD: PASS' -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
