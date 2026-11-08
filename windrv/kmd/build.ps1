<#
  build.ps1  -  Build MaliKmd.sys (ARM64, KMDF) without MSBuild project files.

  Toolchain is discovered automatically (see tools\winsdk.ps1):
    * clang (LLVM) with --target=aarch64-pc-windows-msvc   (no MSVC cl.exe needed)
    * WDK headers/libs from an installed WDK, a repo-local external\wdk,
      or the sibling edk2-UEFI-mtk extracted WDK used during bring-up
    * MSVC x64 link.exe (lld-link also works)

  Nothing is hard-coded to one machine. Every path can be overridden with a
  parameter or an environment variable:

      -WdkRoot   / $env:WDK_ROOT          folder containing Include\ and Lib\
      -KitsRoot  / $env:WINDOWSSDKDIR     Windows SDK root
      -Clang     / $env:MALI_CLANG        clang.exe
      -Linker    / $env:MALI_LINKER       link.exe or lld-link.exe
      -TestPfx   / $env:MALI_TEST_PFX     signing certificate (.pfx)
      -PfxPassword / $env:MALI_TEST_PFX_PASSWORD

  Everything this script writes stays inside windrv\kmd\build.

  Examples:
      .\build.ps1                       # build + sign with an auto-found test cert
      .\build.ps1 -SkipSign             # build only (no certificate needed)
      .\build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string]$Configuration = 'Release',
    [string]$WdkRoot     = $env:WDK_ROOT,
    [string]$KitsRoot    = $env:WINDOWSSDKDIR,
    [string]$Clang       = $env:MALI_CLANG,
    [string]$Linker      = $env:MALI_LINKER,
    [string]$TestPfx     = $env:MALI_TEST_PFX,
    [string]$PfxPassword = $env:MALI_TEST_PFX_PASSWORD,
    [switch]$SkipSign
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Push-Location $root

try {
    . (Join-Path $root 'tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    Write-Host 'toolchain' -ForegroundColor Cyan
    Write-Host ("  clang   : {0}" -f $t.Clang)
    Write-Host ("  linker  : {0}" -f $t.Linker)
    Write-Host ("  wdk     : {0}  (km {1}, kmdf {2})" -f $t.WdkRoot, $t.KmVer, $t.KmdfVersion)
    Write-Host ("  sdk     : {0}  ({1})" -f $t.KitsRoot, $t.SdkVer)

    $out     = Join-Path $root 'build'
    $objDir  = Join-Path $out 'obj'
    $package = Join-Path $out 'package'
    New-Item -ItemType Directory -Force -Path $objDir  | Out-Null
    New-Item -ItemType Directory -Force -Path $package | Out-Null
    # Never ship a stale catalog/signature from a previous build.
    Get-ChildItem -LiteralPath $package -File -ErrorAction SilentlyContinue | Remove-Item -Force

    # Keep the WDF version the driver is compiled against in sync with the
    # KMDF that is actually installed on this machine.
    $kmdfParts = $t.KmdfVersion -split '\.'
    $kmdfMajor = $kmdfParts[0]
    $kmdfMinor = if ($kmdfParts.Count -gt 1) { $kmdfParts[1] } else { '0' }

    $sources = @(
        'Driver.c','MaliIo.c','MaliHw.c','MaliMmu.c','MaliMem.c','MaliJob.c','MaliIrq.c','MaliSoc.c'
    )

    $opt = if ($Configuration -eq 'Debug') { @('-O0','-g','-gcodeview') } else { @('-O2','-gcodeview') }

    $cc = @(
        '--target=aarch64-pc-windows-msvc',
        '-fms-extensions','-fms-compatibility','-fms-compatibility-version=19.40',
        '-ffreestanding','-fno-stack-protector',
        '-D_KERNEL_MODE','-D_ARM64_','-D_WIN64','-D_WIN32_WINNT=0x0a00',
        '-DNTDDI_VERSION=0x0a00000e',
        ('-DKMDF_VERSION_MAJOR={0}' -f $kmdfMajor),
        ('-DKMDF_VERSION_MINOR={0}' -f $kmdfMinor),
        '-Wno-pragma-pack','-Wno-ignored-attributes','-Wno-microsoft-anon-tag',
        '-Wno-unknown-pragmas','-Wno-ignored-pragma-intrinsic',
        '-Iinclude','-Isrc'
    ) + $opt
    $cc += @('-isystem', $t.KmInc, '-isystem', $t.KmCrt, '-isystem', $t.WdfInc)
    if (Test-Path -LiteralPath $t.SdkShared) { $cc += @('-isystem', $t.SdkShared) }

    $objects = @()
    Write-Host ("Building {0} ARM64 (clang) ..." -f $Configuration) -ForegroundColor Cyan
    foreach ($src in $sources) {
        $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
        $objects += $obj
        $cargs = $cc + @('-c', (Join-Path 'src' $src), '-o', $obj)
        & $t.Clang @cargs
        if ($LASTEXITCODE -ne 0) { throw "Compile failed: $src" }
    }

    $sys = Join-Path $out 'MaliKmd.sys'
    $link = @(
        '/nologo','/driver','/kernel','/subsystem:native','/machine:ARM64',
        '/entry:FxDriverEntry','/nodefaultlib','/ignore:4257',
        ("/libpath:{0}" -f $t.KmLib),("/libpath:{0}" -f $t.WdfLib),
        ("/out:{0}" -f $sys),'/debug'
    )
    if ($Configuration -eq 'Release') { $link += '/release' }
    $link += $objects
    $link += @('wdfdriverentry.lib','wdfldr.lib','ntoskrnl.lib','hal.lib','aux_klib.lib','libcntpr.lib','bufferoverflowfastfailk.lib')
    & $t.Linker @link
    if ($LASTEXITCODE -ne 0) { throw 'Link failed' }

    Write-Host ("OK: {0} ({1} bytes)" -f $sys, (Get-Item -LiteralPath $sys).Length) -ForegroundColor Green

    # --- locate an optional test-signing certificate -------------------------
    if (-not $TestPfx -and -not $SkipSign) {
        # Look for a cert without forcing any machine-specific location.
        $windrvRoot = Split-Path -Parent $root            # windrv
        $repoRoot   = Split-Path -Parent $windrvRoot      # MT6789__WINDRIVER
        $projRoot   = Split-Path -Parent $repoRoot        # parent of the repo
        foreach ($c in @(
            (Join-Path $root     'test-signing\test.pfx'),
            (Join-Path $repoRoot 'test-signing\test.pfx'),
            (Join-Path $projRoot 'edk2-UEFI-mtk\.cache\p13001l-port\windows-pogo-cert\test.pfx'))) {
            if ($c -and (Test-Path -LiteralPath $c)) { $TestPfx = $c; break }
        }
    }
    if (-not $PfxPassword -and $TestPfx -and (Test-Path -LiteralPath $TestPfx)) {
        $PfxPassword = 'p13001l-local-test'   # password of the bundled bring-up cert
    }

    # --- package (INF + SYS + CAT), optionally signed with a local test cert ---
    $infText = Get-Content -LiteralPath (Join-Path $root 'MaliKmd.inf') -Raw
    $infText = [regex]::Replace($infText, '(?m)^KmdfLibraryVersion\s*=.*$',
                                ("KmdfLibraryVersion = {0}" -f $t.KmdfVersion))
    Set-Content -LiteralPath (Join-Path $package 'MaliKmd.inf') -Value $infText -Encoding ASCII
    Copy-Item -LiteralPath $sys -Destination $package -Force

    # Ship the public half of the test cert next to the package: a test-signed
    # driver only loads once that cert is trusted on the target (see INSTALL.CMD).
    $testCer = $env:MALI_TEST_CER
    if (-not $testCer -and $TestPfx) { $testCer = Join-Path (Split-Path -Parent $TestPfx) 'P13Test.cer' }
    if (-not $testCer) {
        $local = Join-Path $root 'test-signing\P13Test.cer'
        if (Test-Path -LiteralPath $local) { $testCer = $local }
    }
    if ($testCer -and (Test-Path -LiteralPath $testCer)) {
        Copy-Item -LiteralPath $testCer -Destination $package -Force
    }

    if (-not $SkipSign -and $TestPfx -and (Test-Path -LiteralPath $TestPfx)) {
        if ($t.Inf2Cat) {
            & $t.Inf2Cat "/driver:$package" /os:10_GE_ARM64
            if ($LASTEXITCODE -ne 0) { Write-Host 'WARN: Inf2Cat failed; catalog not generated' -ForegroundColor Yellow }
            # Inf2Cat lowercases the catalog name; the INF refers to MaliKmd.cat.
            $lowerCat = Join-Path $package 'malikmd.cat'
            if (Test-Path -LiteralPath $lowerCat) {
                Move-Item -LiteralPath $lowerCat -Destination (Join-Path $package 'MaliKmd.cat') -Force
            }
        } else {
            Write-Host 'WARN: Inf2Cat.exe not found; catalog not generated' -ForegroundColor Yellow
        }
        if ($t.SignTool) {
            & $t.SignTool sign /fd SHA256 /f $TestPfx /p $PfxPassword (Join-Path $package 'MaliKmd.sys')
            if (Test-Path -LiteralPath (Join-Path $package 'MaliKmd.cat')) {
                & $t.SignTool sign /fd SHA256 /f $TestPfx /p $PfxPassword (Join-Path $package 'MaliKmd.cat')
            }
        } else {
            Write-Host 'WARN: signtool.exe not found; package left unsigned' -ForegroundColor Yellow
        }
    } else {
        Write-Host 'Signing skipped (no test cert found, or -SkipSign).' -ForegroundColor Yellow
        Write-Host '  The driver can still be built and inspected; installing it requires' -ForegroundColor Yellow
        Write-Host '  test-signing mode plus a certificate trusted on the target machine.' -ForegroundColor Yellow
    }

    Write-Host 'package' -ForegroundColor Cyan
    Get-ChildItem -LiteralPath $package | ForEach-Object {
        Write-Host ('  {0,10} {1}' -f $_.Length, $_.Name)
    }

    # Device-side scripts are kept in the repo (tools\device) and copied into
    # the build output so one folder can be uploaded to the target.
    $deviceSrc = Join-Path $root 'tools\device'
    if (Test-Path -LiteralPath $deviceSrc) {
        $deviceOut = Join-Path $out 'device'
        New-Item -ItemType Directory -Force -Path $deviceOut | Out-Null
        foreach ($f in (Get-ChildItem -LiteralPath $deviceSrc -File)) {
            Copy-Item -LiteralPath $f.FullName -Destination $deviceOut -Force
        }
        Write-Host 'device scripts' -ForegroundColor Cyan
        Get-ChildItem -LiteralPath $deviceOut | ForEach-Object {
            Write-Host ('  {0,10} {1}' -f $_.Length, $_.Name)
        }
    }
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
