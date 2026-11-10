<#
  extract-wdk.ps1  -  unpack the WDK NuGet package into external\wdk and report
                      whether the graphics DDIs are complete.

  Why this exists: windrv\kmd\tools\winsdk.ps1 wants a folder that contains
  Include\ and Lib\. A NuGet WDK package (Microsoft.Windows.WDK.ARM64) is a zip
  whose payload lives under a "c/" prefix, so it cannot be pointed at directly.

  The extraction done during bring-up was partial: kernel headers only, a few
  user-mode headers. That is enough for the KMDF accelerator driver but it hides
  two things this script exists to settle:

    * d3dkmddi.h lives in Include\<ver>\shared, not Include\<ver>\km;
    * IddCx lives in Include\<ver>\um\iddcx\<ver> + Lib\...\um\ARM64\iddcx\<ver>.

  It also checks the link side, which is the part people get wrong: DxgkInitialize
  and DxgkInitializeDisplayOnlyDriver are defined in km\ARM64\displib.lib (there
  is no dxgkrnl.lib in the WDK), and an IddCx driver links iddcxstub.lib plus
  WdfDriverStubUm.lib.

  Headers that come from the *SDK* rather than the WDK (d3dkmdt.h, d3dkmthk.h,
  dxgi*.h) are reported separately, because mixing the two roots up is the usual
  reason a WDDM build fails on "cannot open d3dkmdt.h".

  No install, no elevation, no network: the package is read where it already is.

  Usage:
      .\extract-wdk.ps1                       # find the package automatically
      .\extract-wdk.ps1 -Nupkg <path\to.nupkg>
      .\extract-wdk.ps1 -Dest D:\somewhere\wdk -Force

  Environment overrides:
      $env:MT6789_WDK_NUPKG   path to the .nupkg
      $env:MT6789_WDK_DEST    destination folder

  Everything this script writes stays inside the destination folder.
#>
[CmdletBinding()]
param(
    [string]$Nupkg = $env:MT6789_WDK_NUPKG,
    [string]$Dest  = $env:MT6789_WDK_DEST,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$kmdRoot    = Split-Path -Parent $PSScriptRoot        # windrv\kmd
$windrvRoot = Split-Path -Parent $kmdRoot             # windrv
$repoRoot   = Split-Path -Parent $windrvRoot          # repo root
$projRoot   = Split-Path -Parent $repoRoot            # parent of the repo

if (-not $Dest) { $Dest = Join-Path $repoRoot 'external\wdk' }

if (-not $Nupkg) {
    $candidates = @(
        (Join-Path $projRoot 'edk2-UEFI-mtk\.cache\p13001l-port\windows-wdk'),
        (Join-Path $repoRoot 'external')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    $Nupkg = Get-ChildItem -LiteralPath $candidates -Filter 'microsoft.windows.wdk.*.nupkg' -File -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}

if (-not $Nupkg -or -not (Test-Path -LiteralPath $Nupkg)) {
    throw ("WDK NuGet package not found. Pass -Nupkg <path> or set MT6789_WDK_NUPKG. " +
           "Get it with:  winget download Microsoft.WindowsWDK.10.0.26100 --architecture arm64")
}

$Dest  = [IO.Path]::GetFullPath($Dest)
$stamp = Join-Path $Dest '.extracted-from.txt'
$skip  = $false

if ((Test-Path -LiteralPath $stamp) -and -not $Force) {
    if ((Get-Content -LiteralPath $stamp -TotalCount 1) -eq $Nupkg) { $skip = $true }
}

Write-Host 'extract-wdk' -ForegroundColor Cyan
Write-Host ("  package : {0} ({1:N0} bytes)" -f $Nupkg, (Get-Item -LiteralPath $Nupkg).Length)
Write-Host ("  dest    : {0}" -f $Dest)

if ($skip) {
    Write-Host '  already extracted from this package (-Force to redo)' -ForegroundColor Yellow
}
else {
    $zip = [IO.Compression.ZipFile]::OpenRead($Nupkg)
    try {
        $prefixes = $zip.Entries | ForEach-Object { ($_.FullName -split '/')[0] } | Sort-Object -Unique
        if ($prefixes -notcontains 'c') {
            throw ("Unexpected package layout (top level: {0}). Expected a 'c/' payload root." -f ($prefixes -join ', '))
        }
        if ((Test-Path -LiteralPath $Dest) -and (Get-ChildItem -LiteralPath $Dest -Force | Select-Object -First 1)) {
            if (-not $Force) {
                throw ("Destination is not empty and was not extracted from this package: {0}. Use -Force." -f $Dest)
            }
        }
        New-Item -ItemType Directory -Path $Dest -Force | Out-Null

        $n = 0
        foreach ($e in $zip.Entries) {
            if ($e.FullName -notlike 'c/*') { continue }
            $rel = $e.FullName.Substring(2)
            if ([string]::IsNullOrEmpty($rel) -or $rel.EndsWith('/')) { continue }
            $out = Join-Path $Dest ($rel -replace '/', '\')
            $dir = Split-Path -Parent $out
            if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
            $n++
        }
        Set-Content -LiteralPath $stamp -Value $Nupkg -Encoding ascii
        Write-Host ("  extracted {0:N0} files" -f $n)
    }
    finally {
        $zip.Dispose()
    }
}

# ---- verification -----------------------------------------------------------
# Resolve the newest KMDF/UMDF/IddCx version folder the way the WDK does.
function Get-NewestVersionDir {
    param([string]$Parent)
    if (-not (Test-Path -LiteralPath $Parent)) { return $null }
    Get-ChildItem -LiteralPath $Parent -Directory |
        Sort-Object -Property @{ Expression = { try { [version]$_.Name } catch { [version]'0.0.0.0' } } } -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

$inc  = Join-Path $Dest 'Include'
$lib  = Join-Path $Dest 'Lib'
$kmdf = Get-NewestVersionDir (Join-Path $inc 'wdf\kmdf')
$umdf = Get-NewestVersionDir (Join-Path $lib 'wdf\umdf\ARM64')
$iddc = Get-NewestVersionDir (Join-Path $inc '10.0.26100.0\um\iddcx')
$iddcLib = if ($iddc) { Join-Path (Join-Path $lib '10.0.26100.0\um\ARM64\iddcx') (Split-Path -Leaf $iddc) } else { $null }
$kmdfLib = Get-NewestVersionDir (Join-Path $lib 'wdf\kmdf\ARM64')

# SDK root, for the headers the WDK does not ship.
$sdkRoot = $env:WINDOWSSDKDIR
if (-not $sdkRoot) { $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
$sdkShared = $null
if (Test-Path -LiteralPath (Join-Path $sdkRoot 'Include')) {
    $sdkVer = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
              Sort-Object -Property @{ Expression = { try { [version]$_.Name } catch { [version]'0.0.0.0' } } } -Descending |
              Select-Object -First 1
    if ($sdkVer) { $sdkShared = Join-Path $sdkVer.FullName 'shared' }
}

$checks = @(
    @{ Group = 'kernel';  Name = 'kmdf\wdf.h';                       Path = if ($kmdf) { Join-Path $kmdf 'wdf.h' } }
    @{ Group = 'kernel';  Name = 'kmdf\wdfldr.lib';                  Path = if ($kmdfLib) { Join-Path $kmdfLib 'wdfldr.lib' } }
    @{ Group = 'WDDM';    Name = 'shared\d3dkmddi.h';                Path = Join-Path $inc '10.0.26100.0\shared\d3dkmddi.h' }
    @{ Group = 'WDDM';    Name = 'km\dispmprt.h';                    Path = Join-Path $inc '10.0.26100.0\km\dispmprt.h' }
    @{ Group = 'WDDM';    Name = 'km\ARM64\displib.lib (DxgkInitialize)'; Path = Join-Path $lib '10.0.26100.0\km\ARM64\displib.lib' }
    @{ Group = 'IddCx';   Name = 'um\iddcx\<ver>\IddCx.h';           Path = if ($iddc) { Join-Path $iddc 'IddCx.h' } }
    @{ Group = 'IddCx';   Name = 'um\ARM64\iddcx\<ver>\iddcxstub.lib'; Path = if ($iddcLib) { Join-Path $iddcLib 'iddcxstub.lib' } }
    @{ Group = 'IddCx';   Name = 'umdf\WdfDriverStubUm.lib';         Path = if ($umdf) { Join-Path $umdf 'WdfDriverStubUm.lib' } }
    @{ Group = 'SDK';     Name = 'shared\d3dkmdt.h';                 Path = if ($sdkShared) { Join-Path $sdkShared 'd3dkmdt.h' } }
    @{ Group = 'SDK';     Name = 'shared\d3dkmthk.h';                Path = if ($sdkShared) { Join-Path $sdkShared 'd3dkmthk.h' } }
    @{ Group = 'SDK';     Name = 'shared\dxgi1_3.h';                 Path = if ($sdkShared) { Join-Path $sdkShared 'dxgi1_3.h' } }
)

$missing = @{}
foreach ($g in @('kernel', 'WDDM', 'IddCx', 'SDK')) { $missing[$g] = 0 }

Write-Host '  graphics toolchain' -ForegroundColor Cyan
foreach ($c in $checks) {
    $ok = $c.Path -and (Test-Path -LiteralPath $c.Path)
    if (-not $ok) { $missing[$c.Group]++ }
    $mark  = if ($ok) { 'ok  ' } else { 'MISS' }
    $color = if ($ok) { 'Gray' } else { 'Yellow' }
    Write-Host ("    {0}  [{1,-6}] {2}" -f $mark, $c.Group, $c.Name) -ForegroundColor $color
}

Write-Host '  verdict' -ForegroundColor Cyan
$verdicts = @(
    @{ Group = 'kernel'; Label = 'KMDF accelerator driver (current work)' },
    @{ Group = 'WDDM';   Label = 'WDDM display miniport (L3 / L5a)' },
    @{ Group = 'IddCx';  Label = 'IddCx indirect display (L5b)' },
    @{ Group = 'SDK';    Label = 'SDK-side shared display headers' }
)
foreach ($v in $verdicts) {
    $n = $missing[$v.Group]
    if ($n -eq 0) { Write-Host ("    ready    {0}" -f $v.Label) -ForegroundColor Green }
    else           { Write-Host ("    blocked  {0}  ({1} missing)" -f $v.Label, $n) -ForegroundColor Yellow }
}
Write-Host '  done' -ForegroundColor Green
