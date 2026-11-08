<#
  build-malitest.ps1  -  Build the ARM64 user-mode bring-up tool malitest.exe.

  The tool is built CRT-free (clang-cl with /Zl + our own mainCRTStartup,
  linked with /nodefaultlib), so no MSVC C runtime needs to be installed.

  The toolchain is discovered by tools\winsdk.ps1; every path can be overridden
  with a parameter or an environment variable:

      -KitsRoot / $env:WINDOWSSDKDIR
      -Clang    / $env:MALI_CLANG
      -Linker   / $env:MALI_LINKER

  Output stays inside windrv\kmd\build.
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)   # windrv\kmd
Push-Location $root

try {
    . (Join-Path $root 'tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $clang = $t.ClangCl
    if (-not $clang -or -not (Test-Path -LiteralPath $clang)) {
        throw (("clang-cl.exe not found next to {0}. Install LLVM (or the Visual Studio " +
                "'C++ Clang tools for Windows'), or pass -Clang <path to clang.exe>.") -f $t.Clang)
    }

    $out = Join-Path $root 'build'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $obj = Join-Path $out 'malitest.obj'
    $exe = Join-Path $out 'malitest.exe'

    $cc = @(
        '--target=aarch64-pc-windows-msvc','/nologo','/c','/O2','/W4','/GS-','/Zl','-fno-builtin',
        '/D_ARM64_','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0a00000e',
        '/I', (Join-Path $root 'include'),
        '/imsvc', $t.SdkUm,
        '/imsvc', $t.SdkShared,
        '/imsvc', $t.SdkUcrt,
        "/Fo$obj", (Join-Path $root 'tools\malitest.c')
    )
    & $clang @cc
    if ($LASTEXITCODE -ne 0) { throw 'malitest compile failed' }

    if (-not (Test-Path -LiteralPath $t.SdkUmLibArm64)) {
        throw (("Windows SDK ARM64 user-mode libs not found: {0}. " +
                "Install the ARM64 components of the Windows SDK.") -f $t.SdkUmLibArm64)
    }

    $lc = @(
        '/nologo','/subsystem:console','/machine:arm64','/entry:mainCRTStartup','/nodefaultlib','/debug',
        ("/libpath:{0}" -f $t.SdkUmLibArm64),("/out:{0}" -f $exe),$obj,
        'kernel32.lib','setupapi.lib','advapi32.lib'
    )
    & $t.Linker @lc
    if ($LASTEXITCODE -ne 0) { throw 'malitest link failed' }

    Write-Host ("OK: {0} ({1} bytes)" -f $exe, (Get-Item -LiteralPath $exe).Length) -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
