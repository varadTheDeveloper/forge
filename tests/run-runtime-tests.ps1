<#
.SYNOPSIS
  Runs forge.exe on each script in this folder and checks its exit code and
  output (Phase 7.9). These cover behaviour that `forge --self-test` cannot
  check from inside its own process: uncaught exceptions and unhandled
  Promise rejections must print the error and exit with code 1.

.EXAMPLE
  .\tests\run-runtime-tests.ps1 -ForgePath C:\Forge\bin\forge.exe
#>
param(
  [string]$ForgePath = "C:\Forge\bin\forge.exe"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path $ForgePath)) {
  Write-Error "forge.exe not found at $ForgePath (pass -ForgePath)"
  exit 2
}

# Name, expected exit code, text that must appear (stdout+stderr), text that must not appear.
$cases = @(
  @{ File = "clean-exit.js";             Exit = 0; Has = @("ok");                                   Not = @() },
  @{ File = "set-timeout-no-delay.js";   Exit = 0; Has = @("fired");                                Not = @() },
  @{ File = "event-order.js";            Exit = 0; Has = @("order: sync,promise,micro1,micro2,timer"); Not = @() },
  @{ File = "handled-rejection.js";      Exit = 0; Has = @("caught: handled");                      Not = @("Uncaught") },
  @{ File = "uncaught-throw.js";         Exit = 1; Has = @("boom from main script", "inner");       Not = @("SHOULD NOT RUN") },
  @{ File = "uncaught-in-timer.js";      Exit = 1; Has = @("boom from timer");                      Not = @("SHOULD NOT RUN") },
  @{ File = "uncaught-in-microtask.js";  Exit = 1; Has = @("boom from microtask");                  Not = @("SHOULD NOT RUN") },
  @{ File = "unhandled-rejection.js";    Exit = 1; Has = @("Uncaught (in promise)", "rejected and never handled"); Not = @() },
  @{ File = "late-handled-rejection.js"; Exit = 1; Has = @("Uncaught (in promise)", "handled too late"); Not = @() }
)

$failures = 0

foreach ($c in $cases) {
  $script = Join-Path $here $c.File
  $outFile = [System.IO.Path]::GetTempFileName()
  $errFile = [System.IO.Path]::GetTempFileName()

  $p = Start-Process -FilePath $ForgePath -ArgumentList "`"$script`"" -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $output = (Get-Content -Raw $outFile) + (Get-Content -Raw $errFile)
  Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue

  $problems = @()
  if ($p.ExitCode -ne $c.Exit) { $problems += "exit code $($p.ExitCode), expected $($c.Exit)" }
  foreach ($t in $c.Has) { if ($output -notlike "*$t*") { $problems += "missing '$t'" } }
  foreach ($t in $c.Not) { if ($output -like "*$t*") { $problems += "unexpected '$t'" } }

  if ($problems.Count -eq 0) {
    Write-Host ("[runtime-test] {0,-30} PASS" -f $c.File)
  } else {
    $failures++
    Write-Host ("[runtime-test] {0,-30} FAIL: {1}" -f $c.File, ($problems -join "; "))
    Write-Host "  output was:"
    Write-Host $output
  }
}

if ($failures -eq 0) {
  Write-Host "[runtime-test] ALL PASSED"
  exit 0
}

Write-Host "[runtime-test] $failures FAILED"
exit 1
