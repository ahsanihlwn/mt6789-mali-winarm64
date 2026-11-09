<#
  build-gl-smoke.ps1  -  build the offscreen OpenGL bring-up test (ARM64).

  It links against the same CRT-free recipe as the rest of Jalur B: clang-cl
  /Zl-style /nodefaultlib plus the mali_cxxrt.lib shim (mainCRTStartup, operator
  new, the UCRT/[[noreturn]] bits). No import library for opengl32 is used on
  purpose - the test LoadLibrary()s the copy next to itself, so the local Mesa
  build wins over System32 without registering a driver.

  Run tools\setup.ps1 and tools\build-runtime.ps1 first.

  Output: build\gl-smoke\mali_gl_smoke.exe
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Stop'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot

Push-Location $mesaRoot
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $binDir  = Split-Path -Parent $t.ClangCl
    $lld     = Join-Path $binDir 'lld-link.exe'

    $libUm   = Join-Path $t.KitsRoot ("Lib\{0}\um\arm64"   -f $t.SdkVer)
    $libUcrt = Join-Path $t.KitsRoot ("Lib\{0}\ucrt\arm64" -f $t.SdkVer)

    $shim = Join-Path $mesaRoot 'build\runtime\mali_cxxrt.lib'
    if (-not (Test-Path -LiteralPath $shim)) { throw 'build\runtime\mali_cxxrt.lib missing; run tools\build-runtime.ps1' }

    $outDir = Join-Path $mesaRoot 'build\gl-smoke'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $src = Join-Path $mesaRoot 'tools\gl-smoke\mali_gl_smoke.c'
    $obj = Join-Path $outDir 'mali_gl_smoke.obj'
    $exe = Join-Path $outDir 'mali_gl_smoke.exe'

    $inc = @('/imsvc', $t.SdkUcrt, '/imsvc', $t.SdkUm, '/imsvc', $t.SdkShared)

    Write-Host 'compiling mali_gl_smoke.c (aarch64) ...'
    & $t.ClangCl (@('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/O2', '/MD',
                    '/W3', '/clang:-fno-builtin') + $inc + @("/Fo$obj", $src))
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $obj)) { throw 'compile failed' }

    if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }

    # Shim first on purpose: see runtime\mali_vcrt.cpp.
    $libs = @($shim, 'ucrt.lib', 'BufferOverflow.lib', 'msvcurt.lib',
              'kernel32.lib', 'ntdll.lib', 'user32.lib', 'gdi32.lib')

    Write-Host 'linking mali_gl_smoke.exe ...'
    $linkArgs = @('/nologo', '/machine:arm64', '/nodefaultlib', "/libpath:$libUm", "/libpath:$libUcrt",
                  "/out:$exe", $obj) + $libs
    & $lld @linkArgs
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe)) { throw 'link failed' }

    $size = (Get-Item -LiteralPath $exe).Length
    Write-Host ''
    Write-Host ("GL smoke  {0}  ({1} bytes)" -f $exe, $size) -ForegroundColor Green
    Write-Host 'BUILD: PASS' -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}