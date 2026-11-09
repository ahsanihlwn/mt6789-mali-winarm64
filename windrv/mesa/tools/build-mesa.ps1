<#
  build-mesa.ps1  -  configure and build Mesa PanVK (panfrost) for
                     aarch64-windows-msvc, using the pinned Mesa release.

  This wraps everything that has to be true at *build* time, not just at
  configure time:

  * PATH -- `meson setup` activates the VS environment, but `ninja` does not,
    and Meson's generated link rule invokes `lld-link` by name. So the LLVM bin
    directory (and the venv Scripts, for python) are prepended to PATH here.

  * TARGET -- Meson decides the linker `/MACHINE:` from `clang-cl --version`,
    which reports the host default (x86_64) because the cross target is only in
    c_args. clang's supported CCC_OVERRIDE_OPTIONS hook makes every clang-cl
    invocation (including --version) carry `--target=aarch64-pc-windows-msvc`,
    so Meson picks `/MACHINE:arm64`.

  It uses the Mesa version pinned by tools\fetch-mesa.ps1 (mesa-25.0.7). Newer
  Mesa (25.1+) needs `mesa_clc`, which links LLVM and cannot be built here; see
  README.md.

  Nothing is executed; the output is an ARM64 DLL. It is not deployed.
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER,
    [string]$Target   = 'aarch64-pc-windows-msvc',
    # vulkan = PanVK (Jalur B, current); gl = Gallium panfrost OpenGL (the v9
    # render path, since upstream PanVK has no v9 backend). Separate build dirs.
    [ValidateSet('vulkan', 'gl')]
    [string]$Kind = 'vulkan',
    [switch]$Reconfigure,
    [switch]$ConfigureOnly,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Continue'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot
$srcDir   = Join-Path (Split-Path -Parent $windrv) 'external\mesa'
$buildDir = Join-Path $mesaRoot ("build\mesa" + $(if ($Kind -eq 'gl') { '-gl' } else { '' }))
$cross    = Join-Path $mesaRoot 'build\cross.ini'

if (-not (Test-Path -LiteralPath $srcDir)) {
    throw ("Mesa source missing at {0}. Run tools\fetch-mesa.ps1 first." -f $srcDir)
}

# Bring the checkout to the project's known-good state (MSVC portability fixes
# live in windrv\mesa\patches\, applied by this script). Idempotent.
& (Join-Path $mesaRoot 'tools\patch-mesa.ps1') -MesaDir $srcDir
if ($LASTEXITCODE -ne 0) { throw 'patch-mesa.ps1 failed' }

Push-Location $mesaRoot
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $llvmBin     = Split-Path -Parent $t.ClangCl
    $venvScripts = Join-Path $mesaRoot '.venv\Scripts'
    $meson       = Join-Path $venvScripts 'meson.exe'

    if (-not (Test-Path -LiteralPath $meson)) {
        throw 'Meson venv missing. Run tools\setup.ps1 first.'
    }

    # Build-time environment.
    $env:PATH = "$llvmBin;$venvScripts;$env:PATH"
    $env:CCC_OVERRIDE_OPTIONS = "+--target=$Target"

    # The GL build's GLSL front-end (src/compiler/glsl) generates its lexer and
    # parser and Mesa prefers winflexbison on Windows. tools\fetch-flexbison.ps1
    # vendors win_flex/win_bison into external\ (git-ignored); find them by name.
    $flexBison = Join-Path (Split-Path -Parent $windrv) 'external\winflexbison'
    if (Test-Path -LiteralPath (Join-Path $flexBison 'win_flex.exe')) {
        $env:PATH = "$flexBison;$env:PATH"
    } elseif ($Kind -eq 'gl') {
        throw 'GL builds need win_flex/win_bison. Run tools\fetch-flexbison.ps1 first.'
    }

    # win_flex writes a scratch file into %TEMP% and deletes it afterwards. With
    # this host's user TEMP that intermittently fails
    # ("win_flex.exe: error deleting file ...~flex_out_main_2") and ninja then
    # treats the generation as failed. A project-local TEMP is reliable and
    # keeps build byproducts inside the repo.
    $tmpDir = Join-Path $mesaRoot 'build\tmp'
    New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
    $env:TEMP = $tmpDir
    $env:TMP  = $tmpDir

    # Meson's `setup` activates the Visual Studio environment, but neither
    # `ninja` nor Meson's internal `--internal regenerate` step does. Without
    # LIB, feature probes such as cc.find_library('ws2_32') fail on the first
    # regeneration (which our patch step triggers by rewriting file mtimes) even
    # though the same probe succeeded during setup. Publish the ARM64 library
    # and include search paths the same way the cross file does.
    $lp = $t.Linker -split '\\'
    $mi = [Array]::IndexOf($lp, 'MSVC')
    $msvcVer = if ($mi -ge 0 -and ($mi + 1) -lt $lp.Count) { $lp[$mi + 1] } else { '' }
    $libUm   = $t.SdkUmLibArm64
    $libUcrt = Join-Path $t.KitsRoot ("Lib\{0}\ucrt\arm64" -f $t.SdkVer)
    $libVc   = Join-Path $t.VsInstall (Join-Path "VC\Tools\MSVC\$msvcVer" 'lib\arm64')
    $incVc   = Join-Path $t.VsInstall (Join-Path "VC\Tools\MSVC\$msvcVer" 'include')
    $env:LIB = "$libUm;$libUcrt;$libVc" + $(if ($env:LIB) { ";$env:LIB" } else { '' })
    $env:INCLUDE = "$($t.SdkUcrt);$($t.SdkShared);$($t.SdkUm);$incVc" + $(if ($env:INCLUDE) { ";$env:INCLUDE" } else { '' })

    Write-Host ("LLVM bin     {0}" -f $llvmBin)
    Write-Host ("target       {0}  (via CCC_OVERRIDE_OPTIONS)" -f $Target)

    # Cross file (generated by setup.ps1; regenerate if missing).
    if (-not (Test-Path -LiteralPath $cross)) {
        Write-Host 'build\cross.ini missing; running tools\setup.ps1 ...'
        & (Join-Path $mesaRoot 'tools\setup.ps1') -KitsRoot $KitsRoot -WdkRoot $WdkRoot -Clang $Clang -Linker $Linker
        if (-not (Test-Path -LiteralPath $cross)) { throw 'setup.ps1 did not produce build\cross.ini' }
    }

    if ($Reconfigure -and (Test-Path -LiteralPath $buildDir)) {
        Remove-Item -Recurse -Force $buildDir
    }

    # Minimal, no-LLVM panfrost configurations (see README.md).
    #
    #   vulkan : PanVK ICD only (current Jalur B target)
    #   gl     : Gallium panfrost OpenGL (opengl32.dll / libgallium_wgl.dll).
    #            This is the v9 render path: mainline Gallium panfrost supports
    #            Valhall v9, while upstream PanVK does not.
    #
    # Both use Mesa's Windows platform (WSI only): no display driver, no WDDM,
    # display stays on GOP.
    if ($Kind -eq 'gl') {
        $opts = @(
            '--cross-file', 'build\cross.ini',
            '-Dvulkan-drivers=',
            '-Dgallium-drivers=panfrost',
            '-Dplatforms=windows',
            '-Dglx=disabled',
            '-Degl=disabled',
            '-Dgbm=disabled',
            '-Dopengl=true',
            '-Dtools=',
            '-Dbuild-tests=false',
            '-Dllvm=disabled',
            '-Dcpp_rtti=false',
            '-Dxmlconfig=disabled'
        )
    } else {
        $opts = @(
            '--cross-file', 'build\cross.ini',
            '-Dvulkan-drivers=panfrost',
            '-Dgallium-drivers=',
            '-Dplatforms=windows',
            '-Dglx=disabled',
            '-Dgbm=disabled',
            '-Dtools=',
            '-Dbuild-tests=false',
            '-Dopengl=false',
            '-Dllvm=disabled',
            '-Dcpp_rtti=false',
            '-Dxmlconfig=disabled'
        )
    }

    # Meson caches the cross file's built-in options (c_link_args) in coredata
    # and `setup --reconfigure` does not pick up changes to them, which
    # silently links against the old library list. Force a fresh setup when the
    # cross file has been regenerated.
    $ninjaFile = Join-Path $buildDir 'build.ninja'
    if ((Test-Path -LiteralPath $ninjaFile) -and
        ((Get-Item -LiteralPath $cross).LastWriteTime -gt (Get-Item -LiteralPath $ninjaFile).LastWriteTime)) {
        Write-Host 'cross file changed since last setup; rebuilding from scratch'
        Remove-Item -Recurse -Force $buildDir
    }

    if (-not (Test-Path -LiteralPath $ninjaFile)) {
        Write-Host ("meson setup ({0}) ..." -f $Kind)
        & $meson setup $buildDir $srcDir @opts
        if ($LASTEXITCODE -ne 0) { throw 'meson setup failed' }
    }

    if ($ConfigureOnly) {
        Write-Host ''
        Write-Host ("MESA CONFIGURE ({0}): OK" -f $Kind) -ForegroundColor Green
        exit 0
    }

    Write-Host 'meson compile ...'
    $compileArgs = @('compile', '-C', $buildDir)
    if ($Jobs -gt 0) { $compileArgs += @('-j', "$Jobs") }
    & $meson @compileArgs
    $rc = $LASTEXITCODE

    Write-Host ''
    if ($rc -eq 0) {
        Write-Host 'MESA BUILD: PASS' -ForegroundColor Green
        $dll = Get-ChildItem $buildDir -Recurse -Include '*.dll' -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -match 'panfrost|gallium|opengl32' } |
               Sort-Object Name | Select-Object -First 8
        foreach ($d in $dll) { Write-Host ("  {0}  ({1} bytes)" -f $d.FullName, $d.Length) }
    } else {
        Write-Host ("MESA BUILD: FAIL (exit {0})" -f $rc) -ForegroundColor Red
    }
    exit $rc
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
