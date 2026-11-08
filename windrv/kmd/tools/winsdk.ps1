<#
  winsdk.ps1  -  locate the Windows build toolchain without hard-coded paths.

  Dot-source this file, then call:

      $t = Resolve-MaliToolchain [-WdkRoot <path>] [-KitsRoot <path>] `
                                 [-Clang <path>] [-Linker <path>]

  It returns an object with the paths the build scripts need. Every location is
  discovered, so a fresh clone builds on any machine that has the documented
  prerequisites; each value can still be forced with a parameter or an
  environment variable.

  Discovery order
  ----------------
  Windows SDK : -KitsRoot, $env:WINDOWSSDKDIR, $env:WindowsSdkDir,
                HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots\KitsRoot10,
                %ProgramFiles(x86)%\Windows Kits\10
  WDK headers : -WdkRoot, $env:WDK_ROOT, the installed WDK (shares the SDK
                root), a repo-local external\wdk copy, or a sibling
                edk2-UEFI-mtk extracted WDK (convenience for this workspace)
  clang       : -Clang, $env:MALI_CLANG, VS LLVM dir, clang.exe on PATH
  linker      : -Linker, $env:MALI_LINKER, newest MSVC link.exe, lld-link.exe
#>

function Get-MaliKitsRoot {
    param([string]$Override)

    $candidates = @()
    if ($Override) { $candidates += $Override }
    if ($env:WINDOWSSDKDIR) { $candidates += $env:WINDOWSSDKDIR }
    if ($env:WindowsSdkDir) { $candidates += $env:WindowsSdkDir }

    foreach ($key in @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots')) {
        try {
            $v = (Get-ItemProperty -LiteralPath $key -ErrorAction Stop).KitsRoot10
            if ($v) { $candidates += $v }
        } catch { }
    }

    if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10') }
    if ($env:ProgramFiles)        { $candidates += (Join-Path $env:ProgramFiles 'Windows Kits\10') }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        $c = $c.TrimEnd('\')
        if (Test-Path -LiteralPath (Join-Path $c 'Include')) { return $c }
    }
    return $null
}

function Get-MaliNewestDir {
    param([string]$Parent, [string]$Filter = '*')

    if (-not $Parent -or -not (Test-Path -LiteralPath $Parent)) { return $null }
    Get-ChildItem -LiteralPath $Parent -Directory -Filter $Filter -ErrorAction SilentlyContinue |
        Sort-Object -Property @{
            Expression = { try { [version]$_.Name } catch { [version]'0.0.0.0' } }
        } -Descending |
        Select-Object -First 1
}

function Get-MaliVsInstall {
    $vswhere = $null
    if (${env:ProgramFiles(x86)}) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if (-not $vswhere -or -not (Test-Path -LiteralPath $vswhere)) { return $null }

    $path = & $vswhere -latest -products * -property installationPath 2>$null
    if ($path) { return (($path | Select-Object -First 1) -as [string]).Trim() }
    return $null
}

function Resolve-MaliToolchain {
    param(
        [string]$WdkRoot,
        [string]$KitsRoot,
        [string]$Clang,
        [string]$Linker
    )

    $t = [ordered]@{}

    # ---- Windows SDK -------------------------------------------------------
    $kits = Get-MaliKitsRoot -Override $KitsRoot
    if (-not $kits) {
        throw ("Windows SDK not found. Install the Windows 11 SDK, or pass -KitsRoot <path>, " +
               "or set WINDOWSSDKDIR.")
    }
    $t.KitsRoot = $kits

    $sdkIncDir = $null
    $sdkVer = $null
    foreach ($d in (Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory -ErrorAction SilentlyContinue |
                    Sort-Object -Property @{ Expression = { try { [version]$_.Name } catch { [version]'0.0.0.0' } } } -Descending)) {
        if (Test-Path -LiteralPath (Join-Path $d.FullName 'um\windows.h')) {
            $sdkIncDir = $d.FullName
            $sdkVer = $d.Name
            break
        }
    }
    if (-not $sdkIncDir) { throw "No SDK headers (Include\<ver>\um\windows.h) under $kits" }

    $t.SdkVer    = $sdkVer
    $t.SdkInc    = $sdkIncDir
    $t.SdkUm     = Join-Path $sdkIncDir 'um'
    $t.SdkShared = Join-Path $sdkIncDir 'shared'
    $t.SdkUcrt   = Join-Path $sdkIncDir 'ucrt'
    $t.SdkUmLibArm64 = Join-Path $kits "Lib\$sdkVer\um\arm64"

    # ---- WDK ---------------------------------------------------------------
    $wdk = $null
    if ($WdkRoot) { $wdk = $WdkRoot }
    if (-not $wdk -and $env:WDK_ROOT) { $wdk = $env:WDK_ROOT }
    if (-not $wdk) {
        # An installed WDK drops its km headers next to the SDK ones.
        if (Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory -ErrorAction SilentlyContinue |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'km') } | Select-Object -First 1) {
            $wdk = $kits
        }
    }
    if (-not $wdk) {
        # Repo-local copy, then the sibling edk2 workspace used during bring-up.
        $kmdRoot    = Split-Path -Parent $PSScriptRoot        # windrv\kmd
        $windrvRoot = Split-Path -Parent $kmdRoot             # windrv
        $repoRoot   = Split-Path -Parent $windrvRoot          # MT6789__WINDRIVER
        $projRoot   = Split-Path -Parent $repoRoot            # parent of the repo
        foreach ($c in @(
            (Join-Path $repoRoot 'external\wdk'),
            (Join-Path $projRoot 'edk2-UEFI-mtk\.cache\p13001l-port\windows-wdk\extracted\c'))) {
            if ($c -and (Test-Path -LiteralPath (Join-Path $c 'Include'))) { $wdk = $c; break }
        }
    }
    if (-not $wdk) {
        throw ("Windows Driver Kit not found. Install the WDK for Windows 11, or set WDK_ROOT, " +
               "or pass -WdkRoot <path> (the folder that contains Include\ and Lib\).")
    }
    $wdk = (Resolve-Path -LiteralPath $wdk).Path.TrimEnd('\')
    $t.WdkRoot = $wdk

    $kmDir = $null
    foreach ($d in (Get-ChildItem -LiteralPath (Join-Path $wdk 'Include') -Directory -ErrorAction SilentlyContinue |
                    Sort-Object -Property @{ Expression = { try { [version]$_.Name } catch { [version]'0.0.0.0' } } } -Descending)) {
        if (Test-Path -LiteralPath (Join-Path $d.FullName 'km')) { $kmDir = $d; break }
    }
    if (-not $kmDir) { throw "No kernel-mode headers under $wdk\Include\<ver>\km" }

    $t.KmVer = $kmDir.Name
    $t.KmInc = Join-Path $kmDir.FullName 'km'
    $t.KmCrt = Join-Path $t.KmInc 'crt'
    $t.KmLib = Join-Path $wdk "Lib\$($kmDir.Name)\km\ARM64"

    $kmdfDir = Get-MaliNewestDir -Parent (Join-Path $wdk 'Include\wdf\kmdf')
    if (-not $kmdfDir) { throw "No KMDF headers under $wdk\Include\wdf\kmdf\<version>" }

    $t.KmdfVersion = $kmdfDir.Name
    $t.WdfInc = $kmdfDir.FullName
    $t.WdfLib = Join-Path $wdk "Lib\wdf\kmdf\ARM64\$($kmdfDir.Name)"

    # ---- compilers ---------------------------------------------------------
    $vs = Get-MaliVsInstall
    $t.VsInstall = $vs

    if (-not $Clang) { $Clang = $env:MALI_CLANG }
    if (-not $Clang) {
        $candidates = @()
        if ($vs) {
            $candidates += (Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang.exe')
            $candidates += (Join-Path $vs 'VC\Tools\Llvm\ARM64\bin\clang.exe')
            $candidates += (Join-Path $vs 'VC\Tools\Llvm\bin\clang.exe')
        }
        $onPath = Get-Command clang.exe -ErrorAction SilentlyContinue
        if ($onPath) { $candidates += $onPath.Source }
        foreach ($c in $candidates) {
            if ($c -and (Test-Path -LiteralPath $c)) { $Clang = $c; break }
        }
    }
    if (-not $Clang -or -not (Test-Path -LiteralPath $Clang)) {
        throw ("clang.exe not found. Install LLVM (or the Visual Studio 'C++ Clang tools for Windows'), " +
               "or pass -Clang <path>.")
    }
    $t.Clang = (Resolve-Path -LiteralPath $Clang).Path
    $llvmBin = Split-Path -Parent $t.Clang

    $clangCl = Join-Path $llvmBin 'clang-cl.exe'
    if (-not (Test-Path -LiteralPath $clangCl)) {
        $onPath = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
        if ($onPath) { $clangCl = $onPath.Source } else { $clangCl = $null }
    }
    $t.ClangCl = $clangCl

    if (-not $Linker) { $Linker = $env:MALI_LINKER }
    if (-not $Linker) {
        $candidates = @()
        if ($vs) {
            foreach ($m in (Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
                            Sort-Object -Property @{ Expression = { try { [version]$_.Name } catch { [version]'0.0.0.0' } } } -Descending)) {
                $candidates += (Join-Path $m.FullName 'bin\Hostx64\x64\link.exe')
                $candidates += (Join-Path $m.FullName 'bin\HostARM64\ARM64\link.exe')
            }
        }
        $candidates += (Join-Path $llvmBin 'lld-link.exe')
        $onPath = Get-Command link.exe -ErrorAction SilentlyContinue
        if ($onPath) { $candidates += $onPath.Source }
        foreach ($c in $candidates) {
            if ($c -and (Test-Path -LiteralPath $c)) { $Linker = $c; break }
        }
    }
    if (-not $Linker -or -not (Test-Path -LiteralPath $Linker)) {
        throw ("No linker found. Install the MSVC build tools (link.exe) or LLVM (lld-link.exe), " +
               "or pass -Linker <path>.")
    }
    $t.Linker = (Resolve-Path -LiteralPath $Linker).Path
    $t.IsLldLink = ((Split-Path -Leaf $t.Linker) -ieq 'lld-link.exe')

    # ---- optional signing / packaging tools --------------------------------
    # Inf2Cat ships with the WDK, signtool with the SDK; check both roots.
    $t.SignTool = $null
    $t.Inf2Cat = $null
    foreach ($binRootOwner in @($kits, $wdk)) {
        $binDir = Get-MaliNewestDir -Parent (Join-Path $binRootOwner 'bin')
        if (-not $binDir) { continue }
        if (-not $t.SignTool) {
            $st = Join-Path $binDir.FullName 'x64\signtool.exe'
            if (Test-Path -LiteralPath $st) { $t.SignTool = $st }
        }
        if (-not $t.Inf2Cat) {
            $ic = Join-Path $binDir.FullName 'x86\Inf2Cat.exe'
            if (Test-Path -LiteralPath $ic) { $t.Inf2Cat = $ic }
        }
    }

    return [pscustomobject]$t
}
