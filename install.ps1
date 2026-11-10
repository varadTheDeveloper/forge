# Copies the last build (forge.exe and mozglue.dll) to C:\Forge\bin without
# rebuilding. tools\build.ps1 already does this after every build.
param(
  [string]$EngineDir = "C:\forge-engine\esr153",
  [string]$InstallDir = "C:\Forge\bin"
)

$ErrorActionPreference = "Stop"
$bin = Join-Path $EngineDir "obj-forge\dist\bin"

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item (Join-Path $bin "forge.exe"), (Join-Path $bin "mozglue.dll") -Destination $InstallDir -Force

Write-Host "Forge installed to $InstallDir"
