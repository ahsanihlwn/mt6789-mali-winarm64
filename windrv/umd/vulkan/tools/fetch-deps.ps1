<#
  fetch-deps.ps1  -  fetch the external dependencies of the Vulkan ICD.

  The ICD builds against the official Khronos Vulkan-Headers (vk_icd.h +
  vulkan.h). They are not vendored; like the WDK, they are fetched into the
  git-ignored external\ tree at the repository root.

      external\Vulkan-Headers\include\vulkan\...

  Re-running is safe: an existing checkout is left in place unless -Force.
#>
[CmdletBinding()]
param(
    [string]$Repo = 'https://github.com/KhronosGroup/Vulkan-Headers.git',
    [string]$Ref = 'main',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path        # windrv\umd\vulkan\tools
$vulkan   = Split-Path -Parent $here                               # windrv\umd\vulkan
$umd      = Split-Path -Parent $vulkan                             # windrv\umd
$windrv   = Split-Path -Parent $umd                                # windrv
$repoRoot = Split-Path -Parent $windrv                             # repo root

$external = Join-Path $repoRoot 'external'
$dest     = Join-Path $external 'Vulkan-Headers'

New-Item -ItemType Directory -Force -Path $external | Out-Null

if ((Test-Path -LiteralPath $dest) -and -not $Force) {
    Write-Host ("Vulkan-Headers already present: {0}" -f $dest) -ForegroundColor Green
} else {
    if (Test-Path -LiteralPath $dest) { Remove-Item -Recurse -Force -LiteralPath $dest }
    Write-Host ("Fetching Vulkan-Headers ({0} @ {1})" -f $Repo, $Ref)
    & git clone --depth 1 --branch $Ref $Repo $dest
    if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
}

$hdr = Join-Path $dest 'include\vulkan\vulkan.h'
if (-not (Test-Path -LiteralPath $hdr)) { throw ("Vulkan headers missing after fetch: {0}" -f $hdr) }

$commit = (& git -C $dest rev-parse HEAD 2>$null)
Write-Host ("OK: {0}" -f $hdr) -ForegroundColor Green
if ($commit) { Write-Host ("    pinned at {0}" -f $commit.Trim()) }
