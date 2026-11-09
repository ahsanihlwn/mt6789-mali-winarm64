<#
  patch-mesa.ps1  -  apply this project's portability patches to the pinned Mesa
                      checkout.

  Mesa is written for the Itanium/GCC ABI. Building it for
  aarch64-pc-windows-msvc exposes a small number of places where the MSVC ABI
  differs (notably bit-field layout). The fixes live here as ordinary `git`
  patches under windrv\mesa\patches\, not as ad-hoc edits, so a fresh
  `tools\fetch-mesa.ps1` checkout can be brought to a building state again by
  one command.

  The script is deterministic: it first discards tracked edits in the checkout,
  then applies every patches\*.patch in filename order. Running it twice is
  harmless.

  Usage:
    .\tools\patch-mesa.ps1                 # default: ..\..\external\mesa
    .\tools\patch-mesa.ps1 -MesaDir <path>

  It never touches untracked directories (Meson's downloaded subprojects).
#>
[CmdletBinding()]
param(
    [string]$MesaDir
)

$ErrorActionPreference = 'Stop'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot                                          # windrv

if (-not $MesaDir) {
    $MesaDir = Join-Path (Split-Path -Parent $windrv) 'external\mesa'
}

if (-not (Test-Path -LiteralPath (Join-Path $MesaDir '.git'))) {
    throw ("Not a git checkout: {0}. Run tools\fetch-mesa.ps1 first." -f $MesaDir)
}

$patchDir = Join-Path $mesaRoot 'patches'
$patches  = @()
if (Test-Path -LiteralPath $patchDir) {
    $patches = Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' | Sort-Object Name
}

# Deterministic starting point: drop tracked modifications (our patches).
& git -C $MesaDir checkout -- .
if ($LASTEXITCODE -ne 0) {
    throw ("git checkout failed in {0}" -f $MesaDir)
}

foreach ($p in $patches) {
    & git -C $MesaDir apply --whitespace=nowarn $p.FullName
    if ($LASTEXITCODE -ne 0) {
        throw ("git apply failed: {0}" -f $p.Name)
    }
}

Write-Host ("MESA PATCH: OK ({0} patch(es))" -f $patches.Count)