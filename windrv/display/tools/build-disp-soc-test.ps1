<#
  build-disp-soc-test.ps1  -  Build *and run* the display SoC descriptor test.

  Host (x64) test: the descriptor is pure data, so its internal consistency
  (alignment, no overlapping regions, closed GSI mapping, complete scan-out
  chain) can be checked on the development machine with no device involved.
  CRT-free, like the other host tools.

  Output: windrv\display\build\disp_soc_test.exe  (built, then executed;
  exit code 0 = pass)
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Stop'

$dispRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\display
$windrv  = Split-Path -Parent $dispRoot                                          # windrv

Push-Location $dispRoot
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $clang = $t.ClangCl
    if (-not $clang -or -not (Test-Path -LiteralPath $clang)) {
        throw "clang-cl.exe not found; install LLVM or the VS 'C++ Clang tools for Windows'."
    }

    $libX64 = Join-Path $t.KitsRoot ("Lib\{0}\um\x64" -f $t.SdkVer)
    if (-not (Test-Path -LiteralPath $libX64)) {
        throw ("Windows SDK x64 user-mode libs not found: {0}. Install the x64 " +
               "components of the Windows SDK.") -f $libX64
    }

    $out = Join-Path $dispRoot 'build'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $common = @(
        '--target=x86_64-pc-windows-msvc','/nologo','/c','/O2','/W4','/GS-','/Zl','-fno-builtin',
        '/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0a00000e',
        '/I', (Join-Path $dispRoot 'include'),
        '/imsvc', $t.SdkUm,
        '/imsvc', $t.SdkShared,
        '/imsvc', $t.SdkUcrt
    )

    $objs = @()
    foreach ($src in @('src\MtkDispSoc.c', 'src\MtkDispPanel.c', 'tools\disp_soc_test.c')) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
        $obj  = Join-Path $out ('disp_' + $name + '.obj')
        Write-Host ("CC  {0}  (x64)" -f $src)
        & $clang @common "/Fo$obj" (Join-Path $dispRoot $src)
        if ($LASTEXITCODE -ne 0) { throw ("compile failed: {0}" -f $src) }
        $objs += $obj
    }

    $exe = Join-Path $out 'disp_soc_test.exe'
    $lc = @(
        '/nologo','/subsystem:console','/machine:x64','/entry:mainCRTStartup','/nodefaultlib',
        ("/libpath:{0}" -f $libX64),
        ("/out:{0}" -f $exe)
    ) + $objs + @('kernel32.lib')
    & $t.Linker @lc
    if ($LASTEXITCODE -ne 0) { throw 'link failed' }

    Write-Host ("RUN {0}" -f $exe)
    & $exe
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Host ("display SoC descriptor test FAILED (exit {0})" -f $rc) -ForegroundColor Red
        exit $rc
    }
    Write-Host "display SoC descriptor test passed" -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
