<#
  patch-mesa.ps1  -  bring the pinned Mesa checkout to a building state.

  Two layers are applied, in order, onto the pinned commit
  (see tools\fetch-mesa.ps1):

    1. patches-v9\fourfectvk\*.patch
       The FourFectVK Valhall-v9 PanVK stack (Noysz/panvk-g99-jm @ 08aa2e62).
       Vendored verbatim: upstream Mesa has no v9 backend at all, and Mali-G57
       (MT6789) is v9. This layer also carries the kbase kmod backend that the
       stack's v9 submission path drives.

    2. patches\*.patch
       This project's Windows/MSVC portability patches (bit-field layout,
       missing POSIX/DRM bits, the pregenerated-shader switch, ...).

  After patching, the pregenerated libpan/libpoly sources are copied from
  pregen\ into the Mesa tree, which is what -Dpanfrost-pregen=true consumes.
  They are the output of Mesa's own host tools (see pregen\README.md); the
  Windows build host cannot run those tools because its LLVM has no SPIR-V
  target.

  The script is deterministic and idempotent: it first restores the checkout to
  the pinned commit (tracked files are reset, and files that only exist because
  a patch added them are removed), then applies both layers.

  Usage:
    .\tools\patch-mesa.ps1                      # external\mesa-v9
    .\tools\patch-mesa.ps1 -MesaDir <path>
    .\tools\patch-mesa.ps1 -NoBase              # layer 2 only
    .\tools\patch-mesa.ps1 -NoPregen            # skip the pregen staging

  It never touches git-ignored directories (Meson's downloaded subprojects).
#>
[CmdletBinding()]
param(
    [string]$MesaDir,
    [switch]$NoBase,
    [switch]$NoPregen
)

$ErrorActionPreference = 'Stop'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # windrv\mesa
$windrv   = Split-Path -Parent $mesaRoot                                          # windrv
$repoRoot = Split-Path -Parent $windrv

if (-not $MesaDir) {
    $MesaDir = Join-Path $repoRoot 'external\mesa-v9'
}

if (-not (Test-Path -LiteralPath (Join-Path $MesaDir '.git'))) {
    throw ("Not a git checkout: {0}. Run tools\fetch-mesa.ps1 first." -f $MesaDir)
}

$baseDir   = Join-Path $mesaRoot 'patches-v9\fourfectvk'
$patchDir  = Join-Path $mesaRoot 'patches'
$pregenDir = Join-Path $mesaRoot 'pregen'

# Every patch that will be applied, base layer first.
$stack = @()
if (-not $NoBase -and (Test-Path -LiteralPath $baseDir)) {
    $stack += Get-ChildItem -LiteralPath $baseDir -Filter '*.patch' | Sort-Object Name
}
$ours = @()
if (Test-Path -LiteralPath $patchDir) {
    $ours = Get-ChildItem -LiteralPath $patchDir -Filter '*.patch' | Sort-Object Name
}
$stack += $ours

# --- deterministic starting point -------------------------------------------
# Reset tracked files to the pinned commit ...
& git -C $MesaDir checkout -- .
if ($LASTEXITCODE -ne 0) {
    throw ("git checkout failed in {0}" -f $MesaDir)
}

# ... and delete files that exist only because a patch added them. They are
# untracked, so `checkout` leaves them behind and the next `git apply` would
# fail with "already exists in working directory".
$added = @()
foreach ($p in $stack) {
    $added += Select-String -LiteralPath $p.FullName -Pattern '^\+\+\+ b/([^\t\s]+)' |
              ForEach-Object { $_.Matches[0].Groups[1].Value }
}
foreach ($rel in ($added | Sort-Object -Unique)) {
    $path = Join-Path $MesaDir ($rel -replace '/', '\')
    if (-not (Test-Path -LiteralPath $path)) { continue }
    # `ls-files -- <path>` prints nothing (and exits 0) for an untracked path.
    # (Do not use --error-unmatch: under $ErrorActionPreference='Stop' PowerShell
    # turns its non-zero exit into a terminating NativeCommandError, which made a
    # second run on an already-patched tree fail instead of cleaning up.)
    $tracked = & git -C $MesaDir ls-files -- $rel
    if (-not ($tracked | Where-Object { $_ })) {
        Remove-Item -LiteralPath $path -Force
    }
}

# --- apply both layers -------------------------------------------------------
foreach ($p in $stack) {
    & git -C $MesaDir apply --whitespace=nowarn $p.FullName
    if ($LASTEXITCODE -ne 0) {
        throw ("git apply failed: {0}" -f $p.Name)
    }
}

# --- stage the pregenerated shader sources ----------------------------------
$pregenCount = 0
if (-not $NoPregen) {
    $libpan = Join-Path $MesaDir 'src\panfrost\libpan'
    $polycl = Join-Path $MesaDir 'src\poly\cl'
    $files = @(Get-ChildItem -LiteralPath $pregenDir -Filter 'libpan_*' -File) +
             @(Get-ChildItem -LiteralPath $pregenDir -Filter 'libpoly.*' -File)
    if ($files.Count -eq 0) {
        Write-Host ("pregen\ is empty - run tools\gen-libpan-wsl.sh on a Linux host" ) -ForegroundColor Yellow
    }
    foreach ($f in $files) {
        $dest = if ($f.Name -like 'libpoly.*') { $polycl } else { $libpan }
        Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
        $pregenCount++
    }
}

$commit = (& git -C $MesaDir rev-parse --short HEAD).Trim()
Write-Host ("MESA PATCH: OK ({0} base + {1} project patch(es), {2} pregen file(s), at {3})" -f `
            $(if ($NoBase) { 0 } else { $stack.Count - $ours.Count }), $ours.Count, $pregenCount, $commit)
if ($stack.Count -ne ($stack | Select-Object -Unique).Count) {
    Write-Host 'warning: duplicate patch entries in the stack' -ForegroundColor Yellow
}