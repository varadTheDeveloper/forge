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

# Non-ASCII text is built from code points so this file stays ASCII (Windows
# PowerShell 5.1 reads a script without a byte-order mark as ANSI).
$eAcute = [string][char]0x00E9
$check = [string][char]0x2713
$fire = [char]::ConvertFromUtf32(0x1F525)
$uUmlaut = [string][char]0x00FC

# Phase 8.2: a scratch tree for the Kavach tests. "link" is a junction
# from inside the granted folder to a folder outside it; following it must
# not escape the grant. Junctions need no administrator rights.
$kRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-kavach-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$kGranted = Join-Path $kRoot "granted"
$kOutside = Join-Path $kRoot "outside"
$kLink = Join-Path $kGranted "link"
$kAudit = Join-Path $kRoot "audit.jsonl"
New-Item -ItemType Directory -Force -Path $kGranted, $kOutside | Out-Null
Set-Content -NoNewline -Path (Join-Path $kGranted "ok.txt") -Value "ok-content"
Set-Content -NoNewline -Path (Join-Path $kOutside "secret.txt") -Value "secret-content"
cmd /c mklink /J "$kLink" "$kOutside" | Out-Null
$env:FORGE_KAVACH_TEST = "env-ok"

# File, expected exit code, text that must appear (stdout+stderr), text that
# must not appear, optional script arguments (Args, Phase 8), optional
# options placed before the script (Pre, Phase 8.2) and an optional Check
# script block returning a list of problems (Phase 8.2).
$cases = @(
  @{ File = "clean-exit.js";             Exit = 0; Has = @("ok");                                   Not = @() },
  @{ File = "set-timeout-no-delay.js";   Exit = 0; Has = @("fired");                                Not = @() },
  @{ File = "event-order.js";            Exit = 0; Has = @("order: sync,promise,micro1,micro2,timer"); Not = @() },
  @{ File = "handled-rejection.js";      Exit = 0; Has = @("caught: handled");                      Not = @("Uncaught") },
  @{ File = "uncaught-throw.js";         Exit = 1; Has = @("boom from main script", "inner");       Not = @("SHOULD NOT RUN") },
  @{ File = "uncaught-in-timer.js";      Exit = 1; Has = @("boom from timer");                      Not = @("SHOULD NOT RUN") },
  @{ File = "uncaught-in-microtask.js";  Exit = 1; Has = @("boom from microtask");                  Not = @("SHOULD NOT RUN") },
  @{ File = "unhandled-rejection.js";    Exit = 1; Has = @("Uncaught (in promise)", "rejected and never handled"); Not = @() },
  @{ File = "late-handled-rejection.js"; Exit = 1; Has = @("Uncaught (in promise)", "handled too late"); Not = @() },
  @{ File = "uncaught-non-error.js";     Exit = 1; Has = @("{ reason: 'not an Error' }", "thrower"); Not = @() },
  @{ File = "console-output.js";         Exit = 0; Has = @(
        "Forge is 1 years and 2.5%",
        "{ a: 1, list: [ 1, 2, 3 ], nested: { deep: { deeper: [Object] } } }",
        "caf$eAcute $check $fire",
        "info line", "error line", "warn line",
        "group label", "  inside group", "after group",
        "default: 2", "other: 1",
        "Assertion failed: assert message",
        "t: ", "Timer 'missing' does not exist",
        "'quoted'",
        "Map(1) { 'k' => Set(1) { 1 } } [ undefined, null ]",
        "tail 1 two { three: 3 }");                                                                Not = @("SHOULD NOT PRINT") },
  @{ File = "forge-args.js";             Exit = 0; Has = @("args: [""a b"",""--flag"",""$uUmlaut""]", "count: 3"); Not = @();
     Args = @("a b", "--flag", $uUmlaut) },
  @{ File = "forge-exit.js";             Exit = 7; Has = @("before exit");                          Not = @("SHOULD NOT RUN") },
  @{ File = "forge-exit-code.js";        Exit = 5; Has = @("timer ran");                            Not = @() },
  @{ File = "forge-exit-in-timer.js";    Exit = 3; Has = @("first timer");                          Not = @("SHOULD NOT RUN") },
  @{ File = "kavach-read.js";            Exit = 0; Has = @("allowed: ok-content", "denied: PermissionDenied read"); Not = @("SHOULD NOT READ", "secret-content");
     Pre = @("--allow-read=$kGranted"); Args = @((Join-Path $kGranted "ok.txt"), (Join-Path $kOutside "secret.txt")) },
  @{ File = "kavach-read.js";            Exit = 0; Has = @("allowed: ok-content", "denied: PermissionDenied read"); Not = @("SHOULD NOT READ", "secret-content");
     Pre = @("--allow-read=$kGranted"); Args = @((Join-Path $kGranted "ok.txt"), (Join-Path $kLink "secret.txt")) },
  @{ File = "kavach-no-grant.js";        Exit = 1; Has = @("needs read access to", "--allow-read="); Not = @("SHOULD NOT RUN");
     Args = @((Join-Path $kGranted "ok.txt")) },
  @{ File = "kavach-read.js";            Exit = 0; Has = @("allowed: ok-content", "denied: PermissionDenied read"); Not = @();
     Pre = @("--allow-read=$kGranted", "--audit-log=$kAudit"); Args = @((Join-Path $kGranted "ok.txt"), (Join-Path $kLink "secret.txt"));
     Check = {
       $log = Get-Content -Raw -Encoding UTF8 $kAudit
       $p = @()
       foreach ($t in @('"event":"start"', '"decision":"allow"', '"decision":"deny"', '"reason":"not-granted"', '"event":"stop"')) {
         if (-not $log.Contains($t)) { $p += "audit log lacks $t" }
       }
       $p
     } },
  @{ File = "clean-exit.js";             Exit = 2; Has = @("refused", "whole drive"); Not = @("ok");
     Pre = @("--allow-read=C:\\") },  # doubled: a "\" before the closing quote would escape it
  @{ File = "clean-exit.js";             Exit = 2; Has = @("refused", "home folder"); Not = @();
     Pre = @("--allow-write=$env:USERPROFILE") },
  @{ File = "kavach-env.js";             Exit = 0; Has = @("env: env-ok", "env denied: PermissionDenied"); Not = @("SHOULD NOT READ");
     Pre = @("--allow-env=FORGE_KAVACH_TEST") }
)

$failures = 0

foreach ($c in $cases) {
  $script = Join-Path $here $c.File
  $outFile = [System.IO.Path]::GetTempFileName()
  $errFile = [System.IO.Path]::GetTempFileName()

  # Start-Process joins arguments with spaces, so quote each one here.
  $argLine = ""
  if ($c.Pre) { foreach ($a in $c.Pre) { $argLine += "`"$a`" " } }
  $argLine += "`"$script`""
  if ($c.Args) { foreach ($a in $c.Args) { $argLine += " `"$a`"" } }

  $p = Start-Process -FilePath $ForgePath -ArgumentList $argLine -NoNewWindow -Wait -PassThru `
         -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  # Forge writes UTF-8 to pipes and files.
  $output = (Get-Content -Raw -Encoding UTF8 $outFile) + (Get-Content -Raw -Encoding UTF8 $errFile)
  Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
  if ($null -eq $output) { $output = "" }

  # Contains(), not -like: the expected text has [ ] and * characters.
  $problems = @()
  if ($p.ExitCode -ne $c.Exit) { $problems += "exit code $($p.ExitCode), expected $($c.Exit)" }
  foreach ($t in $c.Has) { if (-not $output.Contains($t)) { $problems += "missing '$t'" } }
  foreach ($t in $c.Not) { if ($output.Contains($t)) { $problems += "unexpected '$t'" } }
  if ($c.Check) { $problems += @(& $c.Check) }

  if ($problems.Count -eq 0) {
    Write-Host ("[runtime-test] {0,-30} PASS" -f $c.File)
  } else {
    $failures++
    Write-Host ("[runtime-test] {0,-30} FAIL: {1}" -f $c.File, ($problems -join "; "))
    Write-Host "  output was:"
    Write-Host $output
  }
}

# Remove the junction first (rmdir removes the link, not its target).
cmd /c rmdir "$kLink" 2>$null
Remove-Item -Recurse -Force $kRoot -ErrorAction SilentlyContinue
Remove-Item Env:\FORGE_KAVACH_TEST -ErrorAction SilentlyContinue

if ($failures -eq 0) {
  Write-Host "[runtime-test] ALL PASSED"
  exit 0
}

Write-Host "[runtime-test] $failures FAILED"
exit 1
