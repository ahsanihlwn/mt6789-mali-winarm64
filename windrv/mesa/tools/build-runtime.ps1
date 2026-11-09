<#
  build-runtime.ps1  -  build the small ARM64 MSVC C++ runtime shim used by
                        Jalur B (see ..\runtime\).

  Members (kept separate so the linker pulls only what a target needs):
    mali_cxxrt.cpp     operator new/delete, atexit, std::_Raise_handler
    mali_crt_exe.cpp   mainCRTStartup   (executables)
    mali_crt_dll.cpp   DllMainCRTStartup (shared libraries)
    mali_tls.cpp       C++ thread-safe statics + thread_local TLS guard
                       (the vcruntime.lib pieces clang emits)
    mali_vcrt.cpp      mem*/str* intrinsics, std::mutex (_Mtx_*), the STL
                       [[noreturn]] shims: keeps VCRUNTIME140/MSVCP140 out
    mali_aarch64.s     __chkstk, _setjmpex/longjmp

  It also *proves* the result by linking, against
  ucrt.lib + BufferOverflow.lib + msvcurt.lib + kernel32.lib + ntdll.lib +
  this shim:

    A. a trivial C exe      -> exercises mainCRTStartup (and thus the
                              "subsystem must be defined" wall)
    B. an STL-using DLL     -> <mutex>, <unordered_map>, <vector>, <string>
                              compiled _HAS_EXCEPTIONS=0 /GR-

  Nothing is executed: the only ARM64 machine is the tablet. A build failure
  here means the cross file recipe in tools\setup.ps1 is incomplete.
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Continue'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot

Push-Location $mesaRoot
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $binDir  = Split-Path -Parent $t.ClangCl
    $lld     = Join-Path $binDir 'lld-link.exe'
    $llvmLib = Join-Path $binDir 'llvm-lib.exe'
    if (-not (Test-Path -LiteralPath $llvmLib)) { $llvmLib = Join-Path $binDir 'llvm-ar.exe' }

    $libUm   = Join-Path $t.KitsRoot ("Lib\{0}\um\arm64"   -f $t.SdkVer)
    $libUcrt = Join-Path $t.KitsRoot ("Lib\{0}\ucrt\arm64" -f $t.SdkVer)
    $vcRoot  = Get-ChildItem (Join-Path $t.VsInstall 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
               Sort-Object Name -Descending | Select-Object -First 1
    $vcInc   = if ($vcRoot) { Join-Path $vcRoot.FullName 'include' }   else { $null }
    $vcArm64 = if ($vcRoot) { Join-Path $vcRoot.FullName 'lib\arm64' } else { $null }

    $out = Join-Path $mesaRoot 'build\runtime'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $inc = @('/imsvc', $t.SdkUcrt, '/imsvc', $t.SdkUm, '/imsvc', $t.SdkShared)
    if ($vcInc -and (Test-Path -LiteralPath $vcInc)) { $inc += @('/imsvc', $vcInc) }
    $libpath = @("/libpath:$libUm", "/libpath:$libUcrt")
    if ($vcArm64 -and (Test-Path -LiteralPath $vcArm64)) { $libpath += "/libpath:$vcArm64" }

    $cf = @('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/O2', '/std:c++17',
            '/EHs-c-', '/GR-', '/D_HAS_EXCEPTIONS=0', '/MD', '/clang:-fno-builtin')
    $cfC = @('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/O2', '/MD',
             '/clang:-fno-builtin')
    $cfS = @('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c')
    # /MD matters even though we link /nodefaultlib + ucrt.lib/msvcurt.lib by hand:
    # the UCRT headers emit #pragma detect_mismatch("RuntimeLibrary", ...), and a
    # shim built /MT gets lld-link's /failifmismatch at the first Mesa DLL link
    # (measured: "MT_StaticRelease" vs "MD_DynamicRelease"). We consume the DLL
    # UCRT, so /MD is also the honest tag.

    # Include roots for the libdrm/Win32 shim: its own compat headers, the KMD
    # ABI, the UMD winsys, and Mesa's drm-uapi headers.
    $shimInc = @(
        '-I', (Join-Path $mesaRoot 'cross\compat'),
        '-I', (Join-Path $mesaRoot 'cross'),
        '-I', (Join-Path $windrv 'kmd\include'),
        '-I', (Join-Path $windrv 'umd\include'),
        '-I', (Join-Path (Split-Path -Parent $windrv) 'external\mesa\include')
    )

    $sources  = @('mali_cxxrt.cpp', 'mali_posix.cpp', 'mali_crt_exe.cpp', 'mali_crt_dll.cpp',
                  'mali_drm.cpp', 'mali_win32os.cpp', 'mali_tls.cpp', 'mali_vcrt.cpp',
                  'mali_umd.c', 'mali_aarch64.s')
    $objNames = @()
    foreach ($src in $sources) {
        $obj = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
        $srcPath = Join-Path $mesaRoot "runtime\$src"
        if (-not (Test-Path -LiteralPath $srcPath)) { $srcPath = Join-Path $windrv "umd\src\$src" }
        if ($src.EndsWith('.c'))      { $flags = $cfC }
        elseif ($src.EndsWith('.s'))  { $flags = $cfS }
        else                          { $flags = $cf }
        Write-Host ("compiling {0} (aarch64) ..." -f $src)
        & $t.ClangCl ($flags + $inc + $shimInc + @("/Fo$obj", $srcPath))
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $obj)) { throw ("compile failed: {0}" -f $src) }
        $objNames += $obj
    }

    $lib = Join-Path $out 'mali_cxxrt.lib'
    Write-Host 'archiving mali_cxxrt.lib ...'
    if ((Split-Path -Leaf $llvmLib) -eq 'llvm-lib.exe') {
        & $llvmLib /nologo /machine:arm64 "/out:$lib" @objNames
    } else {
        & $llvmLib rc $lib @objNames
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $lib)) { throw 'shim archive failed' }

    # The shim comes *first* on purpose: for the handful of symbols it defines
    # (memcpy/strchr..., _Mtx_*, _setjmpex/longjmp) msvcurt.lib would otherwise
    # win and redirect them at VCRUNTIME140.dll / MSVCP140.dll, which are not
    # reliably present on Windows 11 ARM64.  See runtime\mali_vcrt.cpp.
    $crtLibs = @($lib, 'ucrt.lib', 'BufferOverflow.lib', 'msvcurt.lib', 'kernel32.lib',
                 'ntdll.lib', 'setupapi.lib', 'advapi32.lib')

    function Link([string]$label, [string[]]$objs, [string]$mode, [string]$name) {
        $outFile = Join-Path $out $name
        if (Test-Path -LiteralPath $outFile) { Remove-Item -LiteralPath $outFile -Force }
        $a = @('/nologo', '/machine:arm64', '/nodefaultlib') + $libpath + @("/out:$outFile") + $objs + $crtLibs
        if ($mode -eq 'dll') { $a += @('/dll') }
        $raw = & $lld @a 2>&1
        $undef = @()
        $other = @()
        foreach ($line in $raw) {
            $s = [string]$line
            if ($s -match 'undefined symbol:\s*(.+)$') { $undef += $Matches[1].Trim() }
            elseif ($s -match 'error') { $other += $s.Trim() }
        }
        $undef = @($undef | Sort-Object -Unique)
        if (Test-Path -LiteralPath $outFile) {
            return [pscustomobject]@{ Label=$label; Ok=$true; Bytes=(Get-Item -LiteralPath $outFile).Length; Undef=@(); Other=@() }
        }
        return [pscustomobject]@{ Label=$label; Ok=$false; Bytes=0; Undef=$undef; Other=@($other | Select-Object -First 4) }
    }

    # A. trivial C exe: the Meson-sanity-check shape.
    $exeSrc = Join-Path $out 'crt_exe_proof.c'
    "int main(void) { return 0; }" | Set-Content -LiteralPath $exeSrc
    $exeObj = Join-Path $out 'crt_exe_proof.obj'
    & $t.ClangCl (@('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/O2', '/MD') + $inc + @("/Fo$exeObj", $exeSrc))
    if ($LASTEXITCODE -ne 0) { throw 'exe proof compile failed' }

    # B. STL DLL: exercises operator new/delete, the STL support bits and DllMainCRTStartup.
    $dllSrc = Join-Path $out 'stl_proof.cpp'
    @'
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <stdint.h>

static std::mutex g_lock;
static std::unordered_map<uint32_t, uint32_t> g_map;

extern "C" size_t stl_proof(void)
{
    std::lock_guard<std::mutex> holder{ g_lock };
    g_map[1] = 2;
    std::vector<uint32_t> v{ 1, 2, 3 };
    std::string s = "hello";
    return v.size() + g_map.size() + s.size();
}
'@ | Set-Content -LiteralPath $dllSrc
    $dllObj = Join-Path $out 'stl_proof.obj'
    & $t.ClangCl ($cf + $inc + @("/Fo$dllObj", $dllSrc))
    if ($LASTEXITCODE -ne 0) { throw 'dll proof compile failed' }

    $results = @()
    $results += Link 'C   exe  mainCRTStartup' @($exeObj) 'exe' 'crt_exe_proof.exe'
    $results += Link 'C++ dll  STL + DllMainCRTStartup' @($dllObj) 'dll' 'stl_proof.dll'

    Write-Host ''
    Write-Host ("shim      {0}" -f $lib)
    $fail = 0
    foreach ($r in $results) {
        if ($r.Ok) {
            Write-Host ("  {0,-38} LINKED   ({1} bytes)" -f $r.Label, $r.Bytes) -ForegroundColor Green
        } else {
            Write-Host ("  {0,-38} FAILED   ({1} undefined)" -f $r.Label, $r.Undef.Count) -ForegroundColor Red
            foreach ($u in $r.Undef) { Write-Host ("        {0}" -f $u) -ForegroundColor DarkGray }
            foreach ($o in $r.Other)  { Write-Host ("        {0}" -f $o) -ForegroundColor DarkGray }
            $fail++
        }
    }
    Write-Host ''
    if ($fail) { Write-Host 'RUNTIME BUILD: FAIL' -ForegroundColor Red; exit 1 }
    Write-Host 'RUNTIME BUILD: PASS' -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
