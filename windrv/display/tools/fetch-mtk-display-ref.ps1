<#
  fetch-mtk-display-ref.ps1  -  fetch the MediaTek display driver sources we
                                use as the reference for our own display path.

  Why: our Windows display work is a port of the ideas in MTK's DRM/DSI
  driver, not of any Windows driver. The vendor sources are the ground truth
  for block registers, the DDP wiring, the DSI PHY programming and the panel
  configuration format.

  Nothing is installed and nothing outside the repository is touched: the
  files land in external\mtk-display-ref\ (git-ignored) and a manifest with
  SHA-256 hashes is written next to them, so the exact revision we read is
  recorded and re-checkable.

  Usage:
      .\fetch-mtk-display-ref.ps1
      .\fetch-mtk-display-ref.ps1 -Ref <git sha> -Owner adomerle -Repo android_kernel_mediatek_mt6789

  The default Ref is the revision that the edk2 workspace already pinned for
  the same kernel tree (file-manifest.json in that workspace).
#>
[CmdletBinding()]
param(
    [string]$Owner = 'adomerle',
    [string]$Repo  = 'android_kernel_mediatek_mt6789',
    [string]$Ref   = 'f0a85c3c19d808b86dbad9070a267a30709f85d0',
    [string]$Dest
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path      # windrv\display\tools
$dispRoot = Split-Path -Parent $here                         # windrv\display
$windrv   = Split-Path -Parent $dispRoot                     # windrv
$repoRoot = Split-Path -Parent $windrv                       # repo root

if (-not $Dest) { $Dest = Join-Path $repoRoot 'external\mtk-display-ref' }

# Paths are relative to the kernel repository root.
$files = @(
    'drivers/gpu/drm/mediatek/mtk_dsi.c',
    'drivers/gpu/drm/mediatek/mtk_disp_rdma.c',
    'drivers/gpu/drm/mediatek/mtk_disp_color.c',
    'drivers/gpu/drm/mediatek/mtk_disp_ovl.c',
    'drivers/gpu/drm/mediatek/mtk_drm_drv.c',
    'drivers/gpu/drm/mediatek/mtk_drm_drv.h',
    'drivers/gpu/drm/mediatek/mtk_drm_ddp.c',
    'drivers/gpu/drm/mediatek/mtk_drm_ddp.h',
    'drivers/gpu/drm/mediatek/mtk_drm_ddp_comp.c',
    'drivers/gpu/drm/mediatek/mtk_drm_ddp_comp.h',
    'drivers/gpu/drm/mediatek/mtk_drm_crtc.c',
    'drivers/gpu/drm/mediatek/mtk_drm_plane.c',
    'drivers/gpu/drm/mediatek/mtk_drm_gem.c',
    'drivers/gpu/drm/mediatek/mtk_mipi_tx.c',
    'drivers/gpu/drm/mediatek/mtk_mipi_tx.h',
    'drivers/gpu/drm/mediatek/mediatek_v2/mtk_dsi.h',
    'drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_drv.h',
    'drivers/gpu/drm/mediatek/mediatek_v2/mtk_panel_ext.h',
    'drivers/gpu/drm/mediatek/mediatek_v2/mtk_drm_crtc.h',
    'drivers/gpu/drm/mediatek/mediatek_v2/mtk_disp_ovl.c'
)

$base = "https://raw.githubusercontent.com/$Owner/$Repo/$Ref"
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

Write-Host 'fetch-mtk-display-ref' -ForegroundColor Cyan
Write-Host ("  repo : {0}/{1}" -f $Owner, $Repo)
Write-Host ("  ref  : {0}" -f $Ref)
Write-Host ("  dest : {0}" -f $Dest)

$ok = 0
$miss = @()
$manifest = @()

foreach ($rel in $files) {
    $url = "$base/$rel"
    $out = Join-Path $Dest ($rel -replace '/', '\')
    $dir = Split-Path -Parent $out
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

    try {
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -ErrorAction Stop
        $hash = (Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash
        $manifest += [pscustomobject]@{ path = $rel; sha256 = $hash; bytes = (Get-Item -LiteralPath $out).Length }
        Write-Host ("  ok    {0}" -f $rel) -ForegroundColor DarkGray
        $ok++
    }
    catch {
        $miss += $rel
        Write-Host ("  miss  {0}" -f $rel) -ForegroundColor Yellow
        if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
    }
}

$manifestFile = Join-Path $Dest 'manifest.json'
$manifest | ConvertTo-Json -Depth 3 | ForEach-Object { [IO.File]::WriteAllText($manifestFile, $_, (New-Object System.Text.UTF8Encoding($false))) }

Write-Host ("  fetched {0} file(s), {1} missing" -f $ok, $miss.Count) -ForegroundColor Cyan
if ($miss.Count) {
    Write-Host '  missing (they may not exist in this revision):' -ForegroundColor Yellow
    $miss | ForEach-Object { Write-Host ("    {0}" -f $_) -ForegroundColor DarkYellow }
}
Write-Host '  done' -ForegroundColor Green
