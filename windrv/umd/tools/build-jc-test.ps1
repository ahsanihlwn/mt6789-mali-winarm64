<#
  build-jc-test.ps1  -  Build *and run* the job-chain builder unit test.

  Unlike every other script here this targets the HOST (x64), not the tablet: the
  builder is pure C and its output is a byte layout, so the encoding can be
  verified on the development machine with no GPU and no KMD. The test is
  CRT-free, like the target tools.

  Output: windrv\umd\build\jc_test.exe  (built, then executed; exit code 0 = pass)
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

Push-Location $umdRoot
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

    $out = Join-Path $umdRoot 'build'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $common = @(
        '--target=x86_64-pc-windows-msvc','/nologo','/c','/O2','/W4','/GS-','/Zl','-fno-builtin',
        '/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0a00000e',
        '/I', (Join-Path $umdRoot 'include'),
        '/imsvc', $t.SdkUm,
        '/imsvc', $t.SdkShared,
        '/imsvc', $t.SdkUcrt
    )

    $objs = @()
    foreach ($src in @('src\mali_jc.c', 'tools\jc_test.c')) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
        $obj  = Join-Path $out ('jc_' + $name + '.obj')
        Write-Host ("CC  {0}  (x64)" -f $src)
        & $clang @common "/Fo$obj" (Join-Path $umdRoot $src)
        if ($LASTEXITCODE -ne 0) { throw ("compile failed: {0}" -f $src) }
        $objs += $obj
    }

    $exe = Join-Path $out 'jc_test.exe'
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
        Write-Host ("job-chain builder test FAILED (exit {0})" -f $rc) -ForegroundColor Red
        exit $rc
    }
    Write-Host "job-chain builder test passed" -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
