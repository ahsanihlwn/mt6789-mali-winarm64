<#
  build-umd.ps1  -  Build the Mali UMD winsys + round-trip test for ARM64.

  Outputs (into windrv\umd\build):
    mali_umd_test.exe     CRT-free test that walks open -> BO -> job -> close

  The winsys sources (src\mali_umd.c) are compiled straight into the test, so
  there is no separate static library to install. The Vulkan ICD (vulkan\)
  compiles the same mali_umd.c into its own image; see vulkan\tools\build-icd.ps1.

  Toolchain discovery is shared with the KMD: kmd\tools\winsdk.ps1. Every path
  can be overridden with a parameter or an environment variable:

      -KitsRoot / $env:WINDOWSSDKDIR
      -WdkRoot  / $env:WDK_ROOT
      -Clang    / $env:MALI_CLANG
      -Linker   / $env:MALI_LINKER
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Stop'

$umdRoot  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\umd
$windrv   = Split-Path -Parent $umdRoot                                            # windrv
$kmdTools = Join-Path $windrv 'kmd\tools'

Push-Location $umdRoot
try {
    . (Join-Path $kmdTools 'winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $clang = $t.ClangCl
    if (-not $clang -or -not (Test-Path -LiteralPath $clang)) {
        throw (("clang-cl.exe not found next to {0}. Install LLVM or the Visual Studio " +
                "'C++ Clang tools for Windows', or pass -Clang <path to clang.exe>.") -f $t.Clang)
    }

    $out = Join-Path $umdRoot 'build'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $inc = @(
        '/I', (Join-Path $umdRoot 'include'),
        '/I', (Join-Path $windrv  'kmd\include')
    )

    $common = @(
        '--target=aarch64-pc-windows-msvc','/nologo','/c','/O2','/W4','/GS-','/Zl','-fno-builtin',
        '/D_ARM64_','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0a00000e'
    ) + $inc + @(
        '/imsvc', $t.SdkUm,
        '/imsvc', $t.SdkShared,
        '/imsvc', $t.SdkUcrt
    )

    $objs = @()
    foreach ($src in @('src\mali_umd.c', 'src\mali_umd_test.c')) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
        $obj  = Join-Path $out ($name + '.obj')
        Write-Host ("CC  {0}" -f $src)
        & $clang @common "/Fo$obj" (Join-Path $umdRoot $src)
        if ($LASTEXITCODE -ne 0) { throw ("compile failed: {0}" -f $src) }
        $objs += $obj
    }

    if (-not (Test-Path -LiteralPath $t.SdkUmLibArm64)) {
        throw (("Windows SDK ARM64 user-mode libs not found: {0}. " +
                "Install the ARM64 components of the Windows SDK.") -f $t.SdkUmLibArm64)
    }

    $exe = Join-Path $out 'mali_umd_test.exe'
    $lc = @(
        '/nologo','/subsystem:console','/machine:arm64','/entry:mainCRTStartup','/nodefaultlib','/debug',
        ("/libpath:{0}" -f $t.SdkUmLibArm64),
        ("/out:{0}" -f $exe)
    ) + $objs + @('kernel32.lib','setupapi.lib','advapi32.lib')
    & $t.Linker @lc
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }

    Write-Host ("OK: {0} ({1} bytes)" -f $exe, (Get-Item -LiteralPath $exe).Length) -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
