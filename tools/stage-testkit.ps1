<#
  stage-testkit.ps1  -  Collect everything that has to reach the tablet into one
                        folder, with a SHA-256 manifest.

  WHY THIS EXISTS
  ---------------
  The deployable set is spread over three build trees (kmd\, umd\, mesa\) and is
  easy to get wrong by hand: a stale copy of one DLL, or the missing z-1.dll,
  turns a correct build into a mysterious failure on the tablet. This script
  gathers the exact files, records their hashes, and leaves a checklist next to
  them.

  WHAT IT COLLECTS
  ----------------
    kmd\build\package\MaliKmd.sys / .inf / .cat     the driver package
    kmd\build\malitest.exe                          KMD bring-up tool
    umd\build\mali_vk_icd.dll + mali_g57_icd.json   Jalur A Vulkan ICD
    umd\build\mali_vk_smoke.exe                     Jalur A smoke test
    mesa\...\vulkan_panfrost.dll                    Jalur B PanVK ICD (v9, DEPLOY §4)
    mesa\build\vk-smoke\panvk_smoke.exe             PanVK first-run validation (DEPLOY §4)
    mesa\...\libgallium_wgl.dll + opengl32.dll      Mesa GL artefacts (see DEPLOY §3)
    mesa\build\gl-smoke\mali_gl_smoke.exe           offscreen GL probe (DEPLOY §3)
    mesa\...\z-1.dll                                zlib, required by libgallium_wgl

  It only copies what already exists; a missing file is reported, not created.
  Nothing here touches the tablet or USB. Output: out\testkit\ (git-ignored).
#>
[CmdletBinding()]
param(
    [string]$Destination
)

$ErrorActionPreference = 'Stop'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $here
$kmd      = Join-Path $repoRoot 'windrv\kmd'
$umd      = Join-Path $repoRoot 'windrv\umd'
$mesa     = Join-Path $repoRoot 'windrv\mesa'

if (-not $Destination) { $Destination = Join-Path $repoRoot 'out\testkit' }

# Logical name -> candidate source paths (first that exists wins).
$items = [ordered]@{
    'MaliKmd.sys'           = @(Join-Path $kmd 'build\package\MaliKmd.sys')
    'MaliKmd.inf'           = @(Join-Path $kmd 'build\package\MaliKmd.inf')
    'MaliKmd.cat'           = @(Join-Path $kmd 'build\package\MaliKmd.cat')
    'malitest.exe'          = @(Join-Path $kmd 'build\malitest.exe')
    'mali_vk_icd.dll'       = @(Join-Path $umd 'build\mali_vk_icd.dll')
    'mali_g57_icd.json'     = @(Join-Path $umd 'build\mali_g57_icd.json')
    'mali_vk_smoke.exe'     = @(Join-Path $umd 'build\mali_vk_smoke.exe')
    'vulkan_panfrost.dll'   = @(Join-Path $mesa 'build\mesa\src\panfrost\vulkan\vulkan_panfrost.dll')
    'panvk_smoke.exe'       = @(Join-Path $mesa 'build\vk-smoke\panvk_smoke.exe')
    'libgallium_wgl.dll'    = @(Join-Path $mesa 'build\mesa-gl\src\gallium\targets\wgl\libgallium_wgl.dll')
    'opengl32.dll'          = @(Join-Path $mesa 'build\mesa-gl\src\gallium\targets\libgl-gdi\opengl32.dll')
    'mali_gl_smoke.exe'     = @(Join-Path $mesa 'build\gl-smoke\mali_gl_smoke.exe')
    'z-1.dll'               = @(
        (Join-Path $mesa 'build\mesa-gl\subprojects\zlib-1.3.1\z-1.dll'),
        (Join-Path $mesa 'build\mesa\subprojects\zlib-1.3.1\z-1.dll')
    )
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$manifest = New-Object System.Collections.Generic.List[string]
$manifest.Add(('# Mali MT6789 Windows ARM64 test kit'))
$manifest.Add(('# staged {0:yyyy-MM-dd HH:mm:ss}' -f (Get-Date)))
$manifest.Add('#')
$manifest.Add('# sha256                                                           bytes      name')

$missing = @()
foreach ($name in $items.Keys) {
    $src = $items[$name] | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $src) {
        $missing += $name
        Write-Host ("  MISSING  {0}" -f $name) -ForegroundColor Yellow
        continue
    }
    $dst = Join-Path $Destination $name
    Copy-Item -LiteralPath $src -Destination $dst -Force
    $h = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash.ToLower()
    $len = (Get-Item -LiteralPath $dst).Length
    $manifest.Add(('{0}  {1,10}  {2}' -f $h, $len, $name))
    Write-Host ("  ok       {0}  ({1} bytes)" -f $name, $len)
}

$manifestPath = Join-Path $Destination 'MANIFEST.txt'
$manifest | Set-Content -LiteralPath $manifestPath -Encoding ASCII

if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host ("TESTKIT: staged with {0} missing ({1})" -f $missing.Count, ($missing -join ', ')) -ForegroundColor Yellow
} else {
    Write-Host ''
    Write-Host 'TESTKIT: OK (all components staged)' -ForegroundColor Green
}
Write-Host ("  {0}" -f $Destination)
Write-Host ("  {0}" -f $manifestPath)
exit 0
