<#
  link-probe.ps1  -  prove what ARM64 linking works with the toolchain already
                     installed, and pin down the contributions of each library.

  This is the measured counterpart of README.md "What the missing CRT actually
  blocks". It builds real ARM64 objects and links them with /nodefaultlib against
  only the Windows SDK, then reports which configurations succeed and what is
  still undefined in the ones that fail.

  Cases:
    1. C   exe   ucrt.lib + BufferOverflow.lib + kernel32 + ntdll   -> expected OK
    2. C   exe   ucrt.lib + kernel32 + ntdll (no BufferOverflow)    -> expect the
                                                                      GS cookie
                                                                      symbols only
    3. C++ DLL   plain C++ (no STL), same libs as case 1            -> expected OK
    4. C++ DLL   STL/thread, same libs as case 1                    -> expected to
                                                                      fail, and
                                                                      that is the
                                                                      real CRT gap

  Nothing is executed: the only ARM64 machine is the tablet, so "linking" is
  what is proven. Exit code 0 when the expected outcomes hold.

  Requires the LLVM toolchain + Windows SDK (resolved via
  windrv\kmd\tools\winsdk.ps1). Output goes to windrv\mesa\build\linkprobe\.
#>
[CmdletBinding()]
param(
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Continue'   # native link failures are data, not exceptions

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot

Push-Location $mesaRoot
try {
    . (Join-Path $windrv 'kmd\tools\winsdk.ps1')
    $t = Resolve-MaliToolchain -WdkRoot $WdkRoot -KitsRoot $KitsRoot -Clang $Clang -Linker $Linker

    $binDir = Split-Path -Parent $t.ClangCl
    $lld    = Join-Path $binDir 'lld-link.exe'
    if (-not (Test-Path -LiteralPath $lld)) { $lld = $t.Linker }

    $libUm   = Join-Path $t.KitsRoot ("Lib\{0}\um\arm64"   -f $t.SdkVer)
    $libUcrt = Join-Path $t.KitsRoot ("Lib\{0}\ucrt\arm64" -f $t.SdkVer)
    foreach ($p in @($libUm, $libUcrt)) {
        if (-not (Test-Path -LiteralPath $p)) { throw ("ARM64 SDK libs not found: {0}" -f $p) }
    }

    $out = Join-Path $mesaRoot 'build\linkprobe'
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $inc = @(
        '/imsvc', $t.SdkUcrt,
        '/imsvc', $t.SdkUm,
        '/imsvc', $t.SdkShared
    )
    $libpath = @(
        "/libpath:$libUm",
        "/libpath:$libUcrt"
    )

    function Compile([string]$src, [string]$obj, [string[]]$extra) {
        $a = @('/clang:--target=aarch64-pc-windows-msvc', '/nologo', '/c', '/O2', '/GS') + $extra + $inc + @("/Fo$obj", $src)
        & $t.ClangCl @a
        if ($LASTEXITCODE -ne 0) { throw ("compile failed: {0}" -f $src) }
    }

    function TryLink([string]$label, [string[]]$objs, [string[]]$libs, [string]$mode, [string]$name) {
        $outFile = Join-Path $out $name
        $a = @('/nologo', '/machine:arm64', '/nodefaultlib') + $libpath + @("/out:$outFile") + $objs + $libs
        if ($mode -eq 'exe') { $a += @('/subsystem:console', '/entry:mainCRTStartup') }
        else                 { $a += @('/dll', '/noentry') }
        $raw = & $lld @a 2>&1
        $undef = @()
        foreach ($line in $raw) {
            if ($line -match 'undefined symbol:\s*(.+)$') { $undef += $Matches[1].Trim() }
        }
        $undef = @($undef | Sort-Object -Unique)
        $ok = Test-Path -LiteralPath $outFile
        $bytes = if ($ok) { (Get-Item -LiteralPath $outFile).Length } else { 0 }
        return [pscustomobject]@{ Label = $label; Ok = $ok; Bytes = $bytes; Undef = $undef; Raw = $raw }
    }

    Write-Host ("clang-cl  {0}" -f $t.ClangCl)
    Write-Host ("lld-link  {0}" -f $lld)
    Write-Host ("libs um   {0}" -f $libUm)
    Write-Host ("libs ucrt {0}" -f $libUcrt)
    Write-Host ''

    $cObj   = Join-Path $out 'link_probe_c.obj'
    $cppObj = Join-Path $out 'link_probe_cpp.obj'
    $stlSrc = Join-Path $out 'stl_probe.cpp'

    Compile (Join-Path $mesaRoot 'smoke\link_probe.c')   $cObj   @()
    Compile (Join-Path $mesaRoot 'smoke\link_probe.cpp') $cppObj @('/EHs-c-', '/GR-')

    # Case 4 needs STL; write it next to the objects.
    $stlText = @'
#include <mutex>
#include <unordered_map>
#include <vector>
#include <stdint.h>
static std::mutex g_lock;
static std::unordered_map<uint32_t, uint32_t> g_map;
extern "C" size_t stl_probe(void)
{
    std::lock_guard<std::mutex> holder{ g_lock };
    g_map[1] = 2;
    std::vector<uint32_t> v{ 1, 2, 3 };
    return v.size() + g_map.size();
}
'@
    [System.IO.File]::WriteAllText($stlSrc, $stlText)
    $stlObj = Join-Path $out 'stl_probe.obj'
    Compile $stlSrc $stlObj @('/EHsc', '/GR')

    $crtLibs  = @('ucrt.lib', 'BufferOverflow.lib', 'kernel32.lib', 'ntdll.lib')
    $noBow    = @('ucrt.lib', 'kernel32.lib', 'ntdll.lib')

    $results = @()
    $results += TryLink 'C   exe  ucrt + BufferOverflow'         @($cObj, $cppObj) $crtLibs 'exe' 'probe_crtfree.exe'
    $results += TryLink 'C   exe  ucrt only (no BufferOverflow)' @($cObj, $cppObj) $noBow  'exe' 'probe_no_bow.exe'
    $results += TryLink 'C++ dll  plain C++'                    @($cppObj)        $crtLibs 'dll' 'probe_cpp.dll'
    $results += TryLink 'C++ dll  STL/thread (expect fail)'      @($stlObj)        $crtLibs 'dll' 'probe_stl.dll'

    $fail = 0
    foreach ($r in $results) {
        if ($r.Ok) {
            Write-Host ("  {0,-42} LINKED   ({1} bytes)" -f $r.Label, $r.Bytes) -ForegroundColor Green
        }
        else {
            Write-Host ("  {0,-42} FAILED   ({1} undefined)" -f $r.Label, $r.Undef.Count) -ForegroundColor Yellow
            foreach ($u in $r.Undef) { Write-Host ("        {0}" -f $u) -ForegroundColor DarkGray }
        }
    }

    # Expectations: cases 1 and 3 must link; case 4 must not (until libcpmt exists).
    $r1 = $results[0]; $r3 = $results[2]; $r4 = $results[3]
    if (-not $r1.Ok) { Write-Host 'UNEXPECTED: CRT-free C exe did not link.' -ForegroundColor Red; $fail++ }
    if (-not $r3.Ok) { Write-Host 'UNEXPECTED: plain C++ DLL did not link.' -ForegroundColor Red; $fail++ }
    if ($r4.Ok)      { Write-Host 'NOTE: STL DLL linked - the ARM64 C++ runtime is now present.' -ForegroundColor Cyan }
    if ($results[1].Ok) { Write-Host 'NOTE: even without BufferOverflow it linked.' -ForegroundColor Cyan }

    Write-Host ''
    if ($fail) { Write-Host 'LINK PROBE: FAIL' -ForegroundColor Red; exit 1 }
    Write-Host 'LINK PROBE: PASS (recipe confirmed)' -ForegroundColor Green
    if (-not $r4.Ok) {
        Write-Host ('Last wall for full Mesa: {0}' -f ($r4.Undef -join ', ')) -ForegroundColor Yellow
    }
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
