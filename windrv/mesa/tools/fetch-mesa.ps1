<#
  fetch-mesa.ps1  -  Fetch upstream Mesa into external\mesa-v9 (git-ignored).

  Jalur B step 1: obtain the source. It is intentionally shallow so it is cheap
  to re-fetch, and it also installs Mesa's Python build requirements (mako,
  pyyaml, packaging) into the repo-local venv.

  PINNED COMMIT
  -------------
  feeadb5f55f4ba14a5c5ddc84ba20177e419cffb ("26.3.0-devel") is the base the
  FourFectVK Valhall-v9 stack (Noysz/panvk-g99-jm @ 08aa2e62) was measured
  against; see windrv\V9-PANVK-PLAN.md. Upstream Mesa has no v9 backend
  (src/panfrost/vulkan/valhall is empty there), and Mali-G57 is v9, so this
  stack is the backend - not an optimisation on top of one.

  Do not go to Mesa 27.0 yet: its default compiler for the v9 path is the
  Rust "Kraid" compiler, which brings a Rust toolchain into the Windows/ARM64
  cross build for no benefit.

  The tree is *not* usable until tools\patch-mesa.ps1 has applied the vendored
  stack and this project's portability patches. Building also needs the
  pregenerated libpan/libpoly sources in windrv\mesa\pregen (see its README):
  Mesa 25.1+ compiles the panfrost OpenCL kernels at build time with mesa_clc,
  which needs a SPIR-V capable LLVM that the Windows build host does not have.
  Regenerate them with windrv\mesa\tools\gen-libpan-wsl.sh after updating the
  stack's .cl sources.

  Upstream: https://gitlab.freedesktop.org/mesa/mesa   (Anubis may rate-limit
  the web UI; git over https works. GitHub is a read-only mirror at
  https://github.com/Mesa3D/mesa but that org is intermittently 404; GitLab is
  canonical.)
#>
[CmdletBinding()]
param(
    [string]$Remote = 'https://gitlab.freedesktop.org/mesa/mesa.git',
    [string]$Ref    = 'feeadb5f55f4ba14a5c5ddc84ba20177e419cffb',
    [switch]$TrackMain,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if ($TrackMain) { $Ref = 'main' }

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$windrv   = Split-Path -Parent $mesaRoot
$repoRoot = Split-Path -Parent $windrv
$dest     = Join-Path $repoRoot 'external\mesa-v9'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git not found on PATH.' }

if (Test-Path -LiteralPath $dest) {
    if (-not $Force) {
        Write-Host ("{0} already exists; pass -Force to re-fetch." -f $dest)
        exit 0
    }
    Remove-Item -Recurse -Force $dest
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null

# A commit hash cannot be cloned directly, so fetch it explicitly.
if ($Ref -match '^[0-9a-f]{40}$') {
    & git init -q $dest
    & git -C $dest remote add origin $Remote
    & git -C $dest fetch --depth 1 origin $Ref
    if ($LASTEXITCODE -ne 0) { throw 'git fetch of the pinned commit failed' }
    & git -C $dest checkout -q FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw 'git checkout of the pinned commit failed' }
}
else {
    $args = @('clone', '--depth', '1', '--branch', $Ref)
    $args += @($Remote, $dest)
    Write-Host ("git {0}" -f ($args -join ' '))
    & git @args
    if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
}

# The vendored patches and the pregen sources are LF; do not let the working
# copy be rewritten to CRLF behind git's back.
& git -C $dest config core.autocrlf false | Out-Null

# Mesa's Meson needs a few Python modules; install them into the venv.
$venvPy = Join-Path $mesaRoot '.venv\Scripts\python.exe'
if (Test-Path -LiteralPath $venvPy) {
    Write-Host 'Installing Mesa Python build requirements (mako, pyyaml, packaging) ...'
    & $venvPy -m pip install --quiet --disable-pip-version-check mako pyyaml packaging
    if ($LASTEXITCODE -ne 0) { throw 'pip install of Mesa requirements failed' }
}
else {
    Write-Host 'venv not found; run tools\setup.ps1 to create it.' -ForegroundColor Yellow
}

Write-Host ("Mesa source at {0}  (ref {1})" -f $dest, $Ref) -ForegroundColor Green