<#
.SYNOPSIS
  Builds Forge against the pinned engine (Phase 7.95).

.DESCRIPTION
  1. Copies this checkout into the engine tree (setup-engine.ps1 -SyncOnly).
  2. Runs `mach build` there.
  3. Copies forge.exe and mozglue.dll to -InstallDir (forge.exe needs
     mozglue.dll next to it).
  4. With -Test, runs `forge --self-test` and tests\run-runtime-tests.ps1
     against the freshly installed binary.

  Run tools\setup-engine.ps1 and `mach bootstrap` once before the first
  build. mach needs Python 3.12 or older (3.14 breaks it): either run this
  from an environment where `python` is such a version, or pass -Python.

.EXAMPLE
  .\tools\build.ps1 -Test
  .\tools\build.ps1 -Python C:\mozilla-build\python3\python3.exe -Test
#>
param(
  [string]$EngineDir = "C:\forge-engine\esr153",
  [string]$InstallDir = "C:\Forge\bin",
  [string]$Python = "python",
  [switch]$Test
)

$ErrorActionPreference = "Stop"
$forgeDir = Resolve-Path (Join-Path $PSScriptRoot "..")

# --- 1. Copy this checkout into the engine tree ------------------------------
& (Join-Path $PSScriptRoot "setup-engine.ps1") -EngineDir $EngineDir -SyncOnly
if ($LASTEXITCODE -ne 0) { throw "setup-engine.ps1 -SyncOnly failed" }

# --- 2. Build ------------------------------------------------------------------
Push-Location $EngineDir
try {
  & $Python mach build
  if ($LASTEXITCODE -ne 0) { throw "mach build failed (exit code $LASTEXITCODE)" }
} finally {
  Pop-Location
}

# --- 3. Install ----------------------------------------------------------------
$bin = Join-Path $EngineDir "obj-forge\dist\bin"
foreach ($name in @("forge.exe", "mozglue.dll")) {
  if (-not (Test-Path (Join-Path $bin $name))) { throw "The build did not produce $bin\$name" }
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item (Join-Path $bin "forge.exe"), (Join-Path $bin "mozglue.dll") -Destination $InstallDir -Force
$forge = Join-Path $InstallDir "forge.exe"
Write-Host "Installed $forge"

# --- 4. Test -------------------------------------------------------------------
if ($Test) {
  # The self-test writes a scratch folder into the current directory.
  $scratch = Join-Path ([System.IO.Path]::GetTempPath()) "forge-selftest"
  New-Item -ItemType Directory -Force -Path $scratch | Out-Null
  Push-Location $scratch
  try {
    & $forge --self-test
    if ($LASTEXITCODE -ne 0) { throw "forge --self-test failed (exit code $LASTEXITCODE)" }
  } finally {
    Pop-Location
  }

  & (Join-Path $forgeDir "tests\run-runtime-tests.ps1") -ForgePath $forge
  if ($LASTEXITCODE -ne 0) { throw "The runtime tests failed" }
}

exit 0
