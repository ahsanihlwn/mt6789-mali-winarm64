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
    [string]$Python   = $env:MALI_PYTHON,
    [switch]$SystemCrt
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

    # Arg lists are built here (not in the template) because they depend on the
    # link recipe. Forward slashes keep the INI free of backslash escapes.
    function Q([string[]]$items) {
        return (($items | ForEach-Object { "'" + ($_ -replace '\\', '/') + "'" }) -join ', ')
    }

    $target = '/clang:--target=aarch64-pc-windows-msvc'
    # -fuse-ld=lld is a *driver* flag: Meson appends c_link_args after "/link", so
    # it must live in c_args to reach clang-cl itself (otherwise clang-cl tries
    # the absent link.exe and fails with "program not executable").
    # /FI force-includes the MSVC/ARM64 compatibility layer: lower-case
    # _interlocked* intrinsics, asprintf, ...  See cross\msvc-compat.h.
    $compatHdr = To-Fwd (Join-Path $mesaRoot 'cross\msvc-compat.h')
    # cross\compat holds POSIX headers MSVC/ARM64 lacks but Mesa's Unix-oriented
    # files still include (sys/ioccom.h, sys/mman.h, err.h), plus redirects for
    # the DirectX-Headers include layout (directx/d3d12.h, dxguids/dxguids.h).
    $compatDir = To-Fwd (Join-Path $mesaRoot 'cross\compat')
    $cArgs  = @($target, '-fuse-ld=lld', "/FI$compatHdr", "-I$compatDir", '/imsvc', $t.SdkUcrt, '/imsvc', $t.SdkShared, '/imsvc', $t.SdkUm)

    # No /clang:--target here: Meson passes c_link_args straight to lld-link, which
    # does not understand /clang:. The machine comes from /MACHINE:arm64, which
    # Meson derives from the target it detects (see tools\build-mesa.ps1, which
    # sets CCC_OVERRIDE_OPTIONS so clang-cl reports aarch64 to Meson).
    $linkArgs = @("/libpath:$libUmArm64", "/libpath:$libUcrtArm64", "/libpath:$vcLibArm64")

    if (-not $SystemCrt) {
        # CRT-free recipe: the SDK's dynamic UCRT + GS cookie + msvcurt + our
        # small C++ runtime shim, instead of the missing vcruntime/libcmt/libcpmt.
        $shimLib = Join-Path $build 'runtime\mali_cxxrt.lib'
        if (-not (Test-Path -LiteralPath $shimLib)) {
            Write-Host 'Building the ARM64 C++ runtime shim (build-runtime.ps1) ...'
            & (Join-Path $mesaRoot 'tools\build-runtime.ps1') -KitsRoot $KitsRoot -WdkRoot $WdkRoot -Clang $Clang -Linker $Linker
            if (-not (Test-Path -LiteralPath $shimLib)) { throw 'runtime shim build failed' }
        }
        $linkArgs += @('/nodefaultlib',
                       'ucrt.lib', 'BufferOverflow.lib', 'msvcurt.lib', 'kernel32.lib', 'ntdll.lib',
                       'setupapi.lib', 'advapi32.lib',
                       $shimLib)
    }

    $tpl  = Get-Content -Raw -LiteralPath (Join-Path $mesaRoot 'cross\aarch64-windows-msvc.ini.in')
    $map  = @{
        '@CLANGCL@'        = To-Fwd $t.ClangCl
        '@LLVM_AR@'        = To-Fwd $llvmAr
        '@LLVM_RC@'        = To-Fwd $llvmRc
        '@C_ARGS@'         = Q $cArgs
        '@CPP_ARGS@'       = Q $cArgs
        '@C_LINK_ARGS@'    = Q $linkArgs
        '@CPP_LINK_ARGS@'  = Q $linkArgs
    }
    foreach ($k in $map.Keys) { $tpl = $tpl.Replace($k, $map[$k]) }
    $crossOut = Join-Path $build 'cross.ini'
    # Write without a BOM: Meson rejects a leading U+FEFF in machine files.
    [System.IO.File]::WriteAllText($crossOut, $tpl)

    Write-Host ("Cross file  {0}" -f $crossOut)
    Write-Host ("  clang-cl  {0}" -f $t.ClangCl)
    Write-Host ("  libs um   {0}" -f $libUmArm64)
    Write-Host ("  libs ucrt {0}" -f $libUcrtArm64)
    Write-Host ("  link      {0}" -f $(if ($SystemCrt) { 'system CRT (default libs)' } else { 'CRT-free recipe + mali_cxxrt shim' }))

    # ---- 5. honest readiness check ----------------------------------------
    # The normal MSVC ARM64 CRT is missing on this host, so the default
    # (blocking) recipe is the system one. The CRT-free recipe + shim is what
    # makes Meson's sanity links pass without it.
    $vcruntime = Join-Path $vcLibArm64 'vcruntime.lib'
    $libcmt    = Join-Path $vcLibArm64 'libcmt.lib'
    if ((Test-Path -LiteralPath $vcruntime) -and (Test-Path -LiteralPath $libcmt)) {
        Write-Host 'ARM64 MSVC CRT present.' -ForegroundColor Green
    }
    else {
        Write-Host 'ARM64 MSVC CRT (vcruntime/libcmt/libcpmt) is not installed.' -ForegroundColor Yellow
        Write-Host '  Meson will use the CRT-free recipe + mali_cxxrt shim instead.' -ForegroundColor Yellow
        Write-Host '  Verify it with tools\link-probe.ps1 and tools\build-runtime.ps1.' -ForegroundColor Yellow
    }
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
