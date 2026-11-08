<#
  setup.ps1  -  Bootstrap the Meson build environment for Jalur B (Mesa/PanVK).

  This does not build Mesa. It puts the *environment* in place so a later
  `meson setup` can be attempted, and it materialises the cross file:

    1. create a repo-local Python venv   (windrv\mesa\.venv, git-ignored)
    2. install Meson into it              (no system-wide changes)
    3. resolve the same LLVM + Windows SDK the KMD/UMD scripts use
    4. write build\cross.ini from cross\aarch64-windows-msvc.ini.in

  Everything it writes stays inside this repository. Nothing is installed
  system-wide. Re-runnable: existing venv/Meson are reused.
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER,
    [string]$Python   = $env:MALI_PYTHON
)

$ErrorActionPreference = 'Stop'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot                                          # windrv

function To-Fwd([string]$p) { return ($p -replace '\\', '/') }

Push-Location $mesaRoot
try {
    # ---- 1. Python ---------------------------------------------------------
    if (-not $Python) {
        $cand = @()
        foreach ($n in @('python', 'py')) {
            $c = Get-Command $n -ErrorAction SilentlyContinue
            if ($c) { $cand += $c.Source }
        }
        if ($cand.Count -eq 0) { throw 'Python not found; install Python 3.8+ or pass -Python <path>.' }
        $Python = $cand[0]
    }

    # ---- 2. venv + Meson ---------------------------------------------------
    $venv     = Join-Path $mesaRoot '.venv'
    $venvPy   = Join-Path $venv 'Scripts\python.exe'
    $mesonExe = Join-Path $venv 'Scripts\meson.exe'

    if (-not (Test-Path -LiteralPath $venvPy)) {
        Write-Host 'Creating venv (windrv\mesa\.venv) ...'
        & $Python -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw 'venv creation failed' }
    }
    if (-not (Test-Path -LiteralPath $mesonExe)) {
        Write-Host 'Installing Meson into the venv ...'
        & $venvPy -m pip install --quiet --disable-pip-version-check meson
        if ($LASTEXITCODE -ne 0) { throw 'Meson install failed' }
    }
    $mesonVer = (& $mesonExe --version) -join ''
    Write-Host ("Meson {0}  ({1})" -f $mesonVer, $mesonExe)

    # ---- 3. toolchain ------------------------------------------------------
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $clangDir = Split-Path -Parent $t.ClangCl
    $llvmAr   = Join-Path $clangDir 'llvm-ar.exe'
    $llvmRc   = Join-Path $clangDir 'llvm-rc.exe'
    foreach ($p in @($t.ClangCl, $llvmAr, $llvmRc)) {
        if (-not (Test-Path -LiteralPath $p)) { throw ("LLVM tool not found: {0}" -f $p) }
    }

    # MSVC version comes from the linker path (...\VC\Tools\MSVC\<ver>\bin\...).
    $lp = $t.Linker -split '\\'
    $mi = [Array]::IndexOf($lp, 'MSVC')
    $msvcVer = if ($mi -ge 0 -and ($mi + 1) -lt $lp.Count) { $lp[$mi + 1] } else { '' }
    $libUmArm64   = $t.SdkUmLibArm64
    $libUcrtArm64 = Join-Path $t.KitsRoot ("Lib\{0}\ucrt\arm64" -f $t.SdkVer)
    $vcLibArm64   = Join-Path $t.VsInstall (Join-Path "VC\Tools\MSVC\$msvcVer" 'lib\arm64')

    # ---- 4. materialise cross.ini -----------------------------------------
    $build = Join-Path $mesaRoot 'build'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $tpl  = Get-Content -Raw -LiteralPath (Join-Path $mesaRoot 'cross\aarch64-windows-msvc.ini.in')
    $map  = @{
        '@CLANGCL@'        = To-Fwd $t.ClangCl
        '@LLVM_AR@'        = To-Fwd $llvmAr
        '@LLVM_RC@'        = To-Fwd $llvmRc
        '@SDK_UCRT@'       = To-Fwd $t.SdkUcrt
        '@SDK_SHARED@'     = To-Fwd $t.SdkShared
        '@SDK_UM@'         = To-Fwd $t.SdkUm
        '@LIB_UM_ARM64@'   = To-Fwd $libUmArm64
        '@LIB_UCRT_ARM64@' = To-Fwd $libUcrtArm64
        '@VC_LIB_ARM64@'   = To-Fwd $vcLibArm64
    }
    foreach ($k in $map.Keys) { $tpl = $tpl.Replace($k, $map[$k]) }
    $crossOut = Join-Path $build 'cross.ini'
    # Write without a BOM: Meson rejects a leading U+FEFF in machine files.
    [System.IO.File]::WriteAllText($crossOut, $tpl)

    Write-Host ("Cross file  {0}" -f $crossOut)
    Write-Host ("  clang-cl  {0}" -f $t.ClangCl)
    Write-Host ("  libs um   {0}" -f $libUmArm64)
    Write-Host ("  libs ucrt {0}" -f $libUcrtArm64)

    # ---- 5. honest readiness check ----------------------------------------
    # Mesa links a normal C/C++ runtime. Our own tools are CRT-free, so the
    # question "can we even link an ARM64 CRT exe?" decides whether Messon's
    # sanity link can succeed.
    $vcruntime = Join-Path $vcLibArm64 'vcruntime.lib'
    $libcmt    = Join-Path $vcLibArm64 'libcmt.lib'
    if ((Test-Path -LiteralPath $vcruntime) -and (Test-Path -LiteralPath $libcmt)) {
        Write-Host 'ARM64 MSVC CRT present: Meson sanity link has a chance.' -ForegroundColor Green
    }
    else {
        Write-Host 'ARM64 MSVC CRT (vcruntime/libcmt) is MISSING.' -ForegroundColor Yellow
        Write-Host '  Install the VS Build Tools workload for ARM64 (e.g. "MSVC v143/v145 -' -ForegroundColor Yellow
        Write-Host '  ARM64 build tools") to let Meson/Mesa link. See README.md.' -ForegroundColor Yellow
    }
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
