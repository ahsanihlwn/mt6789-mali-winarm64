<#
  fetch-mesa.ps1  -  Fetch upstream Mesa into external\mesa (git-ignored).

  Jalur B step 1: obtain the source. It is intentionally shallow so it is cheap
  to re-fetch, and it also installs Mesa's Python build requirements (mako,
  pyyaml, packaging) into the repo-local venv.

  PINNED VERSION
  --------------
  The default ref is the tag `mesa-25.0.7`, not `main`. Reason (measured
  2026-11-09): from Mesa 25.1.0 on, PanVK embeds OpenCL kernels
  (src/panfrost/libpan/*.cl) that are compiled at build time by `mesa_clc`,
  which links LLVM. This host has only `LLVM-C.lib` and no LLVM headers, so any
  25.1+ tree stops at `ERROR: Feature llvm cannot be enabled: CLC requires
  LLVM`. mesa-25.0.7 is the newest release whose PanVK needs no CLC; Valhall
  (Mali-G57) support is present. Use -Ref to override, or -TrackMain for main.

  Upstream: https://gitlab.freedesktop.org/mesa/mesa   (Anubis may rate-limit
  the web UI; git over https works. GitHub is a read-only mirror at
  https://github.com/Mesa3D/mesa but that org is intermittently 404; GitLab is
  canonical.)
#>
[CmdletBinding()]
param(
    [string]$Remote = 'https://gitlab.freedesktop.org/mesa/mesa.git',
    [string]$Ref    = 'mesa-25.0.7',
    [switch]$TrackMain,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

if ($TrackMain) { $Ref = 'main' }

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$windrv   = Split-Path -Parent $mesaRoot
$repoRoot = Split-Path -Parent $windrv
$dest     = Join-Path $repoRoot 'external\mesa'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git not found on PATH.' }

if (Test-Path -LiteralPath $dest) {
    if (-not $Force) {
        Write-Host ("{0} already exists; pass -Force to re-fetch." -f $dest)
        exit 0
    }
    Remove-Item -Recurse -Force $dest
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null

$args = @('clone', '--depth', '1', '--branch', $Ref)
$args += @($Remote, $dest)

Write-Host ("git {0}" -f ($args -join ' '))
& git @args
if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }

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
