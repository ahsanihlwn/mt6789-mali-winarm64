<#
  build-smoke.ps1  -  Configure + build the Meson cross-toolchain smoke project.

  Proves the aarch64-windows-msvc cross environment works end to end for the
  part Mesa shares with us: compile an ARM64 object and archive a static library.
  Run tools\setup.ps1 first (this does it for you if build\cross.ini is missing).

  This does NOT build Mesa.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$mesaRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$meson    = Join-Path $mesaRoot '.venv\Scripts\meson.exe'
$cross    = Join-Path $mesaRoot 'build\cross.ini'
$bdir     = Join-Path $mesaRoot 'build\smoke'

if (-not (Test-Path -LiteralPath $cross)) {
    Write-Host 'build\cross.ini missing; running setup.ps1 ...'
    & (Join-Path $mesaRoot 'tools\setup.ps1')
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (Test-Path -LiteralPath $bdir) { Remove-Item -Recurse -Force $bdir }

Push-Location $mesaRoot
try {
    & $meson setup build\smoke smoke --cross-file build\cross.ini
    if ($LASTEXITCODE -ne 0) { throw 'meson setup failed' }
    & $meson compile -C build\smoke
    if ($LASTEXITCODE -ne 0) { throw 'meson compile failed' }
    Write-Host 'Mesa cross-toolchain smoke: OK (ARM64 object + archive)' -ForegroundColor Green
}
catch {
    Write-Host ("ERROR: {0}" -f $_.Exception.Message) -ForegroundColor Red
    exit 1
}
finally {
    Pop-Location
}
