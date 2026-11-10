<#
  run-host-tests.ps1  -  run every check that does not need the tablet.

  These are the tests you can run on any Windows x64 development machine with
  the Windows SDK, the WDK headers and LLVM installed. They are the regression
  gate for the parts that are testable off-hardware:

    1. mali_jc byte layout        the job-descriptor encoder the ICD submits
    2. Vulkan ICD entry points    every application entry point resolves,
                                  loader interface v2, *Properties2 chaining,
                                  and the Win32 WSI surface/query set
    3. display SoC descriptor     the measured MMSYS/DSI block map stays
                                  consistent (aligned, non-overlapping, closed
                                  GSI mapping, complete scan-out chain)

  Test 2 needs external\Vulkan-Headers; if it is absent the step is skipped with
  a pointer to the fetch script rather than failing the whole run. Pass -SkipIcd
  to skip it explicitly.

  Exit code 0 only when every executed step passed.

      .\tools\run-host-tests.ps1
      .\tools\run-host-tests.ps1 -SkipIcd
#>
[CmdletBinding()]
param(
    [switch]$SkipIcd,
    [string]$KitsRoot = $env:WINDOWSSDKDIR,
    [string]$WdkRoot  = $env:WDK_ROOT,
    [string]$Clang    = $env:MALI_CLANG,
    [string]$Linker   = $env:MALI_LINKER
)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here

# Each test script calls exit on failure, so it must run in its own process.
# Child output goes to the host, not the pipeline, so the function's only
# return value stays the exit code.
function Invoke-Step {
    param([string]$Name, [string]$Script, [string[]]$StepArgs = @())

    Write-Host ''
    Write-Host ("=== {0} ===" -f $Name) -ForegroundColor Cyan
    Write-Host ("    {0}" -f $Script) -ForegroundColor DarkGray

    & powershell -NoProfile -ExecutionPolicy Bypass -File $Script @StepArgs | Out-Host
    return $LASTEXITCODE
}

$toolArgs = @()
if ($KitsRoot) { $toolArgs += @('-KitsRoot', $KitsRoot) }
if ($WdkRoot)  { $toolArgs += @('-WdkRoot',  $WdkRoot)  }
if ($Clang)    { $toolArgs += @('-Clang',    $Clang)    }
if ($Linker)   { $toolArgs += @('-Linker',   $Linker)   }

$rows = @()

$jc = Join-Path $repo 'windrv\umd\tools\build-jc-test.ps1'
$rc = Invoke-Step 'job-chain builder unit test (x64)' $jc $toolArgs
$rows += [pscustomobject]@{
    Test   = 'mali_jc descriptor byte layout'
    Result = if ($rc -eq 0) { 'PASS' } else { "FAIL (exit $rc)" }
}

$disp = Join-Path $repo 'windrv\display\tools\build-disp-soc-test.ps1'
$rc = Invoke-Step 'display SoC descriptor test (x64)' $disp $toolArgs
$rows += [pscustomobject]@{
    Test   = 'display SoC descriptor (MMSYS/DSI map)'
    Result = if ($rc -eq 0) { 'PASS' } else { "FAIL (exit $rc)" }
}

$icdResult = 'SKIP'
if (-not $SkipIcd) {
    $headers = Join-Path $repo 'external\Vulkan-Headers\include\vulkan\vulkan.h'
    if (-not (Test-Path -LiteralPath $headers)) {
        Write-Host ''
        Write-Host '=== Vulkan ICD entry-point self-test (x64) ===' -ForegroundColor Cyan
        Write-Host ('    skipped: external\Vulkan-Headers is absent.' +
                    ' Run windrv\umd\vulkan\tools\fetch-deps.ps1 first.') -ForegroundColor Yellow
        $icdResult = 'SKIP (no Vulkan-Headers)'
    } else {
        $icd = Join-Path $repo 'windrv\umd\vulkan\tools\build-icd-host.ps1'
        $rc = Invoke-Step 'Vulkan ICD entry-point self-test (x64)' $icd $toolArgs
        $icdResult = if ($rc -eq 0) { 'PASS' } else { "FAIL (exit $rc)" }
    }
}

$rows += [pscustomobject]@{ Test = 'Vulkan ICD entry points + Win32 WSI'; Result = $icdResult }

Write-Host ''
Write-Host '=== summary ===' -ForegroundColor Cyan
foreach ($r in $rows) {
    $colour = if ($r.Result.StartsWith('PASS')) { 'Green' }
              elseif ($r.Result.StartsWith('SKIP')) { 'Yellow' }
              else { 'Red' }
    Write-Host ('  {0,-34} {1}' -f $r.Test, $r.Result) -ForegroundColor $colour
}
Write-Host ''

$failed = $rows | Where-Object { -not ($_.Result.StartsWith('PASS') -or $_.Result.StartsWith('SKIP')) }
if ($failed) {
    Write-Host 'HOST TESTS: FAIL' -ForegroundColor Red
    exit 1
}
Write-Host 'HOST TESTS: PASS' -ForegroundColor Green
exit 0