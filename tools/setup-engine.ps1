<#
.SYNOPSIS
  Sets up the pinned SpiderMonkey engine for building Forge (Phase 7.95).
  Forge stays in its own repository; the engine is a separate, pinned
  Firefox ESR checkout with a copy of Forge placed at js/src/forge.

.DESCRIPTION
  1. Fetches exactly the commit in engine/engine.toml (shallow: one commit,
     no history) into -EngineDir, and verifies it.
  2. Copies this Forge checkout to <EngineDir>\js\src\forge (robocopy,
     mirror; -SyncOnly repeats just this step after you edit Forge).
  3. Makes the only two changes Forge needs inside the engine tree:
     adds "forge" to the DIRS list in js/src/moz.build, and adds
     js/src/forge/ to the ignore list of SpiderMonkey's include-style checker
     (config/check_spidermonkey_style.py). Forge is an embedder with its own
     include rules, like the other ignored directories there.
  4. Writes a .mozconfig for a SpiderMonkey-only build.

  Then, once, in a Python 3.12 (or older) environment:
     cd <EngineDir>
     python mach bootstrap     (choose "SpiderMonkey JavaScript engine")

  After that, tools\build.ps1 does the whole edit-build-test loop.

  mach needs MozillaBuild installed at C:\mozilla-build. The download is
  large (a full Firefox source tree, one commit deep: plan for several GB
  of disk).

.EXAMPLE
  .\tools\setup-engine.ps1 -EngineDir C:\forge-engine\esr153
  .\tools\setup-engine.ps1 -EngineDir C:\forge-engine\esr153 -SyncOnly
#>
param(
  [string]$EngineDir = "C:\forge-engine\esr153",
  [switch]$SyncOnly
)

$ErrorActionPreference = "Stop"
$forgeDir = Resolve-Path (Join-Path $PSScriptRoot "..")
$pinFile = Join-Path $forgeDir "engine\engine.toml"

function Get-PinValue([string]$key) {
  $line = Select-String -Path $pinFile -Pattern "^\s*$key\s*=\s*`"([^`"]+)`"" | Select-Object -First 1
  if (-not $line) { throw "engine.toml has no '$key'" }
  return $line.Matches[0].Groups[1].Value
}

$repo = Get-PinValue "repository"
$commit = Get-PinValue "commit"
$version = Get-PinValue "version"

if ($commit -notmatch '^[0-9a-f]{40}$') { throw "engine.toml commit must be a full 40-character SHA" }

# --- 1. Fetch the pinned engine commit ---------------------------------------
if (-not $SyncOnly) {
  if (-not (Test-Path (Join-Path $EngineDir ".git"))) {
    New-Item -ItemType Directory -Force -Path $EngineDir | Out-Null
    git -C $EngineDir init --quiet
    git -C $EngineDir remote add origin $repo
    # The Firefox tree has paths longer than 260 characters.
    git -C $EngineDir config core.longpaths true
  }

  # Skip the (large) download when the checkout is already at the pin.
  $current = (git -C $EngineDir rev-parse --verify --quiet HEAD 2>$null)
  if ($current -and $current.Trim() -eq $commit) {
    Write-Host "Engine already at $commit; skipping download."
  } else {
    Write-Host "Fetching SpiderMonkey $version ($commit)..."
    git -C $EngineDir fetch --depth 1 origin $commit
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed" }
    git -C $EngineDir checkout --quiet --force FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed" }
  }
}

$head = (git -C $EngineDir rev-parse HEAD).Trim()
if ($head -ne $commit) {
  throw "Engine checkout is at $head, but engine.toml pins $commit. Re-run without -SyncOnly."
}
Write-Host "Engine verified at pinned commit $commit ($version)."

# --- 2. Copy Forge into the engine tree --------------------------------------
$target = Join-Path $EngineDir "js\src\forge"
robocopy $forgeDir $target /MIR /NFL /NDL /NJH /NJS /NP `
  /XD .git obj-forge forge-selftest-fs-scratch `
  /XF *.obj *.exe *.pdb *.ilk | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with exit code $LASTEXITCODE" }
Write-Host "Copied Forge to $target"

# Keep the engine's own git status clean.
$exclude = Join-Path $EngineDir ".git\info\exclude"
if (-not (Select-String -Path $exclude -SimpleMatch "/js/src/forge/" -Quiet)) {
  Add-Content -Path $exclude -Value "/js/src/forge/"
}

if ($SyncOnly) { exit 0 }

# --- 3. Register Forge in js/src/moz.build -----------------------------------
$mozBuild = Join-Path $EngineDir "js\src\moz.build"
$text = Get-Content -Raw $mozBuild
if ($text -notmatch '"forge",') {
  $patched = $text -replace '(DIRS \+= \[\s*\r?\n(\s*)"rust",\s*\r?\n\s*"shell",)', "`$1`n`$2`"forge`","
  if ($patched -eq $text) {
    throw "Could not find the DIRS list with `"rust`" and `"shell`" in js/src/moz.build; add `"forge`", by hand."
  }
  Set-Content -Path $mozBuild -Value $patched -NoNewline
  Write-Host 'Added "forge" to js/src/moz.build'
}

# --- 3b. Exclude Forge from SpiderMonkey's include-style check --------------
$styleCheck = Join-Path $EngineDir "config\check_spidermonkey_style.py"
$styleText = Get-Content -Raw $styleCheck
if ($styleText -notmatch '"js/src/forge/"') {
  $patchedStyle = $styleText -replace '(ignored_js_src_dirs = \[\r?\n)', "`$1    `"js/src/forge/`",  # Forge: an embedder with its own include rules`n"
  if ($patchedStyle -eq $styleText) {
    throw "Could not find ignored_js_src_dirs in config/check_spidermonkey_style.py; add `"js/src/forge/`", to it by hand."
  }
  Set-Content -Path $styleCheck -Value $patchedStyle -NoNewline
  Write-Host "Excluded js/src/forge from check_spidermonkey_style.py"
}

# --- 4. .mozconfig -------------------------------------------------------------
$mozconfig = Join-Path $EngineDir ".mozconfig"
if (-not (Test-Path $mozconfig)) {
  @(
    "mk_add_options MOZ_OBJDIR=@TOPSRCDIR@/obj-forge",
    "ac_add_options --enable-project=js"
  ) | Set-Content -Path $mozconfig
  Write-Host "Wrote $mozconfig"
}

Write-Host ""
Write-Host "Next, once (in a Python 3.12 or older environment):"
Write-Host "  cd $EngineDir"
Write-Host "  python mach bootstrap     # choose: SpiderMonkey JavaScript engine"
Write-Host "Then build and test with:"
Write-Host "  $forgeDir\tools\build.ps1 -Test"

# robocopy leaves a non-zero (success) exit code behind; do not let it
# become this script's exit code.
exit 0
