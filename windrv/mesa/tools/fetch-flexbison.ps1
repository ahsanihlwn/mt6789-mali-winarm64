<#
  fetch-flexbison.ps1  -  Fetch win_flex/win_bison into external\winflexbison
                           (git-ignored), for the OpenGL/Gallium Mesa build.

  WHY THIS EXISTS
  ---------------
  Mesa's Vulkan-only build (tools\build-mesa.ps1 -Kind vulkan) needs neither
  flex nor bison. The OpenGL build (-Kind gl) pulls in the GLSL front-end
  (src/compiler/glsl), whose lexer/parser are generated, and Mesa's own
  meson.build prefers winflexbison on Windows:

      if build_machine.system() == 'windows'
        # Prefer the winflexbison versions, they're much easier to install ...
        prog_flex = find_program('win_flex', required : false)
        ...
        prog_bison = find_program('win_bison', required : false)

  The host has no flex/bison (measured 2026-11-09), so without this the GL
  configure stops at `ERROR: Program 'flex lex' not found or not executable`.

  This is not a host install: the tools are a portable zip unpacked inside the
  repository (external\, git-ignored), found by name on PATH. tools\
  build-mesa.ps1 prepends the directory for the GL build.

  Upstream: https://github.com/lexxmark/winflexbison/releases  (GPLv3+ bison /
  BSD flex; only the unmodified release archive is downloaded).
#>
[CmdletBinding()]
param(
    [string]$Version = '2.5.25',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$windrv   = Split-Path -Parent $mesaRoot
$repoRoot = Split-Path -Parent $windrv
$dest     = Join-Path $repoRoot 'external\winflexbison'
$flex     = Join-Path $dest 'win_flex.exe'
$bison    = Join-Path $dest 'win_bison.exe'

if ((Test-Path -LiteralPath $flex) -and (Test-Path -LiteralPath $bison) -and -not $Force) {
    Write-Host ("winflexbison already present at {0}; pass -Force to re-fetch." -f $dest)
    exit 0
}

$url = "https://github.com/lexxmark/winflexbison/releases/download/v$Version/win_flex_bison-$Version.zip"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("winflexbison-$Version.zip")

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null

Write-Host ("downloading {0}" -f $url)
Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing

if (Test-Path -LiteralPath $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Write-Host ("extracting to {0}" -f $dest)
Expand-Archive -LiteralPath $tmp -DestinationPath $dest -Force
Remove-Item -LiteralPath $tmp -Force

if (-not (Test-Path -LiteralPath $flex))  { throw ("win_flex.exe not found after extract: {0}" -f $dest) }
if (-not (Test-Path -LiteralPath $bison)) { throw ("win_bison.exe not found after extract: {0}" -f $dest) }

Write-Host ("winflexbison {0} at {1}" -f $Version, $dest) -ForegroundColor Green
