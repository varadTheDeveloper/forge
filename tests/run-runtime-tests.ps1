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
# Phase 8.3: a hard link inside the granted folder to the outside file.
$kHardLink = Join-Path $kGranted "hardlink.txt"
cmd /c mklink /H "$kHardLink" (Join-Path $kOutside "secret.txt") | Out-Null
$env:FORGE_KAVACH_TEST = "env-ok"

# Phase 8.4: a tree for the bypass replays (tests/bypass-replays.js, see
# BYPASS-REPLAYS.md). Everything in "granted" is granted for reading and
# writing; the protected files, the links and the audit log are inside it.
$rRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-replay-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$rGranted = Join-Path $rRoot "granted"
$rOutside = Join-Path $rRoot "outside"
$rAudit = Join-Path $rGranted "audit.jsonl"
$rOutLink = Join-Path $rGranted "out-link"
$rGitLink = Join-Path $rGranted "git-link"
New-Item -ItemType Directory -Force -Path $rOutside, (Join-Path $rGranted ".git\hooks"), (Join-Path $rGranted "victim") | Out-Null
Set-Content -NoNewline -Path (Join-Path $rGranted ".git\config") -Value "[core]"
Set-Content -NoNewline -Path (Join-Path $rGranted "forge.json") -Value "{}"
Set-Content -NoNewline -Path (Join-Path $rGranted "ok.txt") -Value "ok-content"
Set-Content -NoNewline -Path (Join-Path $rGranted "victim\keep.txt") -Value "keep"
Set-Content -NoNewline -Path (Join-Path $rOutside "secret.txt") -Value "secret-content"
cmd /c mklink /J "$rOutLink" "$rOutside" | Out-Null
cmd /c mklink /J "$rGitLink" (Join-Path $rGranted ".git") | Out-Null
cmd /c mklink /H (Join-Path $rGranted "hard.txt") (Join-Path $rOutside "secret.txt") | Out-Null
$env:FORGE_REPLAY_GRANTED = "granted-value"

# Phase 8.4: a scratch folder for the native fuzzer (tests/fuzz-natives.js).
# It runs with this folder as its current folder and the only grant.
$fRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-fuzz-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$fScratch = Join-Path $fRoot "scratch"
$fOutside = Join-Path $fRoot "outside"
New-Item -ItemType Directory -Force -Path $fScratch, $fOutside | Out-Null
Set-Content -NoNewline -Path (Join-Path $fOutside "secret.txt") -Value "secret-content"

# Phase 9.1: a copy of tests\modules\app in Temp, plus what cannot live in
# the repository: a junction out of the module root, a .git folder with a
# module in it, and modules next to the root that must never load.
$mRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-module-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$mApp = Join-Path $mRoot "app"
$mOutside = Join-Path $mRoot "outside"
$mOutLink = Join-Path $mApp "out-link"
$mAudit = Join-Path $mRoot "audit.jsonl"
New-Item -ItemType Directory -Force -Path $mRoot, $mOutside | Out-Null
Copy-Item -Recurse -Path (Join-Path $here "modules\app") -Destination $mApp
New-Item -ItemType Directory -Force -Path (Join-Path $mApp ".git") | Out-Null
Set-Content -NoNewline -Path (Join-Path $mApp ".git\hook.js") -Value "export default 'SHOULD NOT LOAD hook';"
Set-Content -NoNewline -Path (Join-Path $mOutside "evil.js") -Value "export default 'SHOULD NOT LOAD evil';"
Set-Content -NoNewline -Path (Join-Path $mRoot "outside.js") -Value "export default 'SHOULD NOT LOAD outside';"
cmd /c mklink /J "$mOutLink" "$mOutside" | Out-Null

# Phase 9.2: a copy of tests\packages\app in Temp, a package outside it that
# only the program itself may use, and a program with an invalid forge.json.
$pRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-package-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$pApp = Join-Path $pRoot "app"
$pOutside = Join-Path $pRoot "outside-pkg"
$pBad = Join-Path $pRoot "bad-app"
New-Item -ItemType Directory -Force -Path $pRoot, $pOutside, $pBad | Out-Null
Copy-Item -Recurse -Path (Join-Path $here "packages\app") -Destination $pApp
Set-Content -NoNewline -Path (Join-Path $pOutside "forge.json") -Value '{ "name": "outside", "exports": "./index.js" }'
Set-Content -NoNewline -Path (Join-Path $pOutside "index.js") -Value 'export default "outside-ok";'
Set-Content -NoNewline -Path (Join-Path $pBad "forge.json") -Value '{ "dependancies": {} }'
Set-Content -NoNewline -Path (Join-Path $pBad "main.js") -Value 'console.log("SHOULD NOT RUN");'

# Phase 9.3: a copy of tests\isolation\app in Temp (a program with packages
# that run in the shared, frozen "pure" compartment).
$iRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-isolation-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$iApp = Join-Path $iRoot "app"
New-Item -ItemType Directory -Force -Path $iRoot | Out-Null
Copy-Item -Recurse -Path (Join-Path $here "isolation\app") -Destination $iApp

# Phase 9.4: a copy of tests\permissions\app in Temp with the files its
# packages try to read, an audit log, and a program whose grant names a
# package that two folders hold.
$gRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-permission-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$gApp = Join-Path $gRoot "app"
$gAudit = Join-Path $gRoot "audit.jsonl"
$gDup = Join-Path $gRoot "dup-app"
New-Item -ItemType Directory -Force -Path $gRoot | Out-Null
Copy-Item -Recurse -Path (Join-Path $here "permissions\app") -Destination $gApp
New-Item -ItemType Directory -Force -Path (Join-Path $gApp "data"), (Join-Path $gApp "private"), (Join-Path $gApp "other") | Out-Null
Set-Content -NoNewline -Path (Join-Path $gApp "data\note.txt") -Value "note-content"
Set-Content -NoNewline -Path (Join-Path $gApp "private\secret.txt") -Value "secret-content"
Set-Content -NoNewline -Path (Join-Path $gApp "other\x.txt") -Value "other-content"
$env:FORGE_PERM_TEST = "perm-value"
$env:FORGE_PERM_OTHER = "other-value"
foreach ($d in @("a", "b", "c")) { New-Item -ItemType Directory -Force -Path (Join-Path $gDup $d) | Out-Null }
Set-Content -NoNewline -Path (Join-Path $gDup "forge.json") -Value '{ "dependencies": { "dup": "file:./a", "other": "file:./b" }, "grants": { "dup": { "read": ["./a"] } } }'
Set-Content -NoNewline -Path (Join-Path $gDup "main.js") -Value 'import a from "dup"; import o from "other"; console.log("SHOULD NOT RUN " + a + o);'
Set-Content -NoNewline -Path (Join-Path $gDup "a\forge.json") -Value '{ "name": "dup", "exports": "./i.js", "permissions": { "read": ["./a"] } }'
Set-Content -NoNewline -Path (Join-Path $gDup "a\i.js") -Value 'export default "a";'
Set-Content -NoNewline -Path (Join-Path $gDup "b\forge.json") -Value '{ "name": "other", "exports": "./i.js", "dependencies": { "dup": "file:../c" } }'
Set-Content -NoNewline -Path (Join-Path $gDup "b\i.js") -Value 'import d from "dup"; export default d;'
Set-Content -NoNewline -Path (Join-Path $gDup "c\forge.json") -Value '{ "name": "dup", "exports": "./i.js" }'
Set-Content -NoNewline -Path (Join-Path $gDup "c\i.js") -Value 'export default "c";'

# What a folder holds, for "nothing changed" checks.
function Get-TreeListing([string]$Path) {
  (Get-ChildItem -LiteralPath $Path -Recurse -Force -Attributes !ReparsePoint |
     ForEach-Object { $_.FullName.Substring($Path.Length) + "|" + $(if ($_.PSIsContainer) { "d" } else { $_.Length }) }) -join "`n"
}
$rOutsideBefore = Get-TreeListing $rOutside

# File, expected exit code, text that must appear (stdout+stderr), text that
# must not appear, optional script arguments (Args, Phase 8), optional
# options placed before the script (Pre, Phase 8.2) and an optional Check
# script block returning a list of problems (Phase 8.2), and an optional
# current folder for Forge (Cwd, Phase 8.4).
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
     Pre = @("--allow-env=FORGE_KAVACH_TEST") },
  @{ File = "fs-async.js";               Exit = 0; Has = @("names: e,f.txt", "typed: e:directory,f.txt:file", "stat: 3 file",
                                                           "lstat: directory", "realpath ends: true", "gone: true",
                                                           "async denied: PermissionDenied"); Not = @("SHOULD NOT READ");
     Pre = @("--allow-read=$kGranted", "--allow-write=$kGranted");
     Args = @((Join-Path $kGranted "async"), (Join-Path $kOutside "secret.txt")) },
  @{ File = "bypass-replays.js";         Exit = 0; Has = @("replays done:", " 0 bypassed", "replay R16 async functions refuse the same way: blocked",
                                                           "replay R11b no module loading: blocked");
     Not = @("BYPASSED", "Uncaught");
     Pre = @("--allow-read=$rGranted", "--allow-write=$rGranted", "--allow-env=FORGE_REPLAY_GRANTED", "--audit-log=$rAudit");
     Args = @($rGranted, $rOutside, $rAudit); Cwd = $rGranted;
     Check = {
       $p = @()
       if ((Get-TreeListing $rOutside) -ne $rOutsideBefore) { $p += "the outside folder changed" }
       if ((Get-Content -Raw (Join-Path $rOutside "secret.txt")) -ne "secret-content") { $p += "secret.txt changed" }
       if ((Get-Content -Raw (Join-Path $rGranted ".git\config")) -ne "[core]") { $p += ".git\config changed" }
       if ((Get-ChildItem -Force (Join-Path $rGranted ".git\hooks")).Count -ne 0) { $p += "a git hook was written" }
       if ((Get-Content -Raw (Join-Path $rGranted "forge.json")) -ne "{}") { $p += "forge.json changed" }
       if (-not (Test-Path (Join-Path $rGranted "victim\keep.txt"))) { $p += "victim\keep.txt was deleted" }
       foreach ($n in @("forge.lock", "stolen.txt", "moved.jsonl", "not-git")) {
         if (Test-Path -LiteralPath (Join-Path $rGranted $n)) { $p += "$n was created" }
       }
       $log = Get-Content -Raw -Encoding UTF8 $rAudit
       foreach ($t in @('"event":"start"', '"reason":"protected"', '"reason":"not-granted"', '"reason":"invalid-path"', '"event":"stop"')) {
         if (-not $log.Contains($t)) { $p += "audit log lacks $t" }
       }
       $p
     } },
  @{ File = "fuzz-natives.js";           Exit = 0; Has = @("fuzz: seed", "natives called", "fuzz done"); Not = @("FUZZ VIOLATION", "Uncaught");
     Pre = @("--allow-read=$fScratch", "--allow-write=$fScratch", "--allow-env=FORGE_KAVACH_TEST");
     Args = @($fScratch); Cwd = $fScratch;
     Check = {
       $p = @()
       if ((Get-Content -Raw (Join-Path $fOutside "secret.txt")) -ne "secret-content") { $p += "the outside file changed" }
       $top = @(Get-ChildItem -Force -LiteralPath $fRoot | ForEach-Object { $_.Name } | Sort-Object) -join ","
       if ($top -ne "outside,scratch") { $p += "the fuzzer created $top next to its scratch folder" }
       $p
     } },
  @{ Name = "modules (9.1)"; File = (Join-Path $mApp "src\main.js"); Exit = 0;
     Has = @("add: 5", "live binding: 1", "same module: true", "side effect runs: 1", "json: forge 3",
             "shared: ROOT", "bom: bom-ok", "meta url: true true", "meta main: true", "resolve: true",
             "strict: true", "tla: tla-ok", "dynamic: dyn-ok main=false", "import is async: sync,after,import",
             "refused node: NotSupported", "refused bare: NotFound", "refused https: NotSupported",
             "refused query: InvalidArgument", "refused windows path: InvalidArgument",
             "refused json without type: InvalidArgument", "refused css type: ",
             "refused missing: NotFound", "refused escape: PermissionDenied", "refused file url: PermissionDenied",
             "refused junction: PermissionDenied", "refused protected: PermissionDenied",
             "refused syntax: SyntaxError", "throws: module threw", "resolve number: InvalidArgument",
             "modules done");
     Not = @("SHOULD NOT"); Pre = @("--audit-log=$mAudit");
     Check = {
       $log = Get-Content -Raw -Encoding UTF8 $mAudit
       $p = @()
       foreach ($t in @('"op":"import"', '"permission":"module"', '"decision":"allow"', '"decision":"deny"', '"reason":"protected"', '"reason":"not-granted"')) {
         if (-not $log.Contains($t)) { $p += "audit log lacks $t" }
       }
       $p
     } },
  @{ File = "tla-reject.js";             Exit = 1; Has = @("before await", "late boom from top-level await", "the main module"); Not = @() },
  @{ File = "import-missing.js";         Exit = 1; Has = @("Cannot find module", "does-not-exist.js"); Not = @("SHOULD NOT RUN") },
  @{ Name = "packages (9.2)"; File = (Join-Path $pApp "src\main.js"); Exit = 0;
     Has = @("greet: HELLO FROM GREET", "condition: forge self: forge", "pattern: alpha",
             "one util instance: true", "outside: outside-ok", "resolve: true",
             "refused not exported: PermissionDenied", "refused private: PermissionDenied",
             "refused into a package by path: PermissionDenied", "refused package escaping by path: PermissionDenied",
             "refused undeclared: NotFound", "refused dependency outside the program: PermissionDenied",
             "refused duplicate key: InvalidData", "refused misnamed: InvalidData", "refused phantom: NotFound",
             "refused no exports: PermissionDenied", "refused invalid name: InvalidArgument",
             "refused unsafe subpath: InvalidArgument", "refused self without exports: PermissionDenied",
             "resolve not exported: PermissionDenied", "packages done");
     Not = @("SHOULD NOT") },
  @{ Name = "bad forge.json (9.2)"; File = (Join-Path $pBad "main.js"); Exit = 1;
     Has = @("forge.json is not valid", "unknown field ""dependancies"""); Not = @("SHOULD NOT RUN") },
  @{ Name = "isolation (9.3)"; File = (Join-Path $iApp "src\main.js"); Exit = 0;
     Has = @("probe globals: undefined undefined undefined", "probe safe globals: function function function",
             "probe eval: PermissionDenied", "probe indirect eval: PermissionDenied",
             "probe new Function: PermissionDenied", "probe frozen: true true true true true true",
             "probe hidden frozen: true true true true true true true true true",
             "probe pollute Object.prototype: TypeError", "probe then on Promise.prototype: TypeError",
             "probe global variable: TypeError", "probe replace console.log: TypeError",
             "probe override: MyError mine custom-text true", "probe app lexical: undefined",
             "probe new realms: undefined undefined",
             "meta in a package: true", "json across packages: probe-data 3",
             "app not polluted: true true", "closure crosses: app-closure", "data crosses: 3 string",
             "pollute through a wrapper: TypeError", "app not polluted after: true",
             "prototypes map: true", "intrinsics map back: true true",
             "Function through constructor: PermissionDenied",
             "refused fs: PermissionDenied", "refused fs function: PermissionDenied",
             "refused bound fs function: PermissionDenied", "refused console: PermissionDenied",
             "refused console.log: PermissionDenied", "refused Forge: PermissionDenied",
             "refused Forge.env.get: PermissionDenied", "refused global object: PermissionDenied",
             "refused import.meta.resolve: PermissionDenied", "fs inside an object: PermissionDenied",
             "Forge.args crosses: true", "error crosses: true probe failed",
             "regexp statics separate: true true", "live binding: bump=1 seen=0",
             "{ id: 7, list: [ 1, 2 ] }", "clear from a package: cleared",
             "package timer: later-ok", "package microtask: micro-ok", "wrapped microtask ran",
             "app timer survived: true", "package dynamic import: helper-ok", "spread import: NotFound",
             "thrower: true thrower failed at its top level", "cycle: InvalidArgument true",
             "isolation done");
     Not = @("SHOULD NOT", "[Proxy]") },
  @{ Name = "permissions (9.4)"; File = (Join-Path $gApp "src\main.js"); Exit = 0;
     Pre = @("--allow-read=$gApp\data", "--allow-read=$gApp\private",
             "--allow-env=FORGE_PERM_TEST,FORGE_PERM_OTHER", "--audit-log=$gAudit");
     Args = @($gApp);
     Has = @("app reads secret: ok secret-content",
             "reader globals: object object object undefined undefined",
             "reader data: ok note-content", "reader secret: PermissionDenied reader not-declared",
             "reader write: PermissionDenied reader not-declared", "reader env: ok perm-value",
             "reader other env: PermissionDenied reader not-declared",
             "reader env all: PermissionDenied reader all-variables",
             "reader async data: ok note-content", "reader async secret: PermissionDenied reader not-declared",
             "reader leaks fs: PermissionDenied - -",
             "reader wasm data: ok note-content", "reader wasm secret: PermissionDenied reader not-declared",
             "greedy data: ok note-content", "greedy secret: PermissionDenied greedy not-granted",
             "greedy steals from reader: PermissionDenied",
             "capped other: PermissionDenied capped command-line",
             "wanting globals: undefined undefined",
             "the package ""wanting"" runs without the permissions it declares",
             "permissions done");
     Not = @("SHOULD NOT", "written");
     Check = {
       $p = @()
       if (Test-Path (Join-Path $gApp "data\out.txt")) { $p += "reader wrote a file" }
       $log = if (Test-Path $gAudit) { Get-Content -Raw $gAudit } else { "" }
       foreach ($t in @('"event":"package","package":"reader"', '"package":"reader","permission":"read","target"',
                        '"reason":"not-declared"', '"reason":"not-granted-by-program"',
                        '"reason":"not-allowed-by-command-line"', '"package":"reader","permission":"env"')) {
         if (-not $log.Contains($t)) { $p += "audit log has no $t" }
       }
       $p
     } },
  @{ Name = "ambiguous grant (9.4)"; File = (Join-Path $gDup "main.js"); Exit = 1;
     Has = @("two folders hold a package of that name"); Not = @("SHOULD NOT RUN") },
  @{ File = "frozen-intrinsics.js"; Exit = 0; Pre = @("--frozen-intrinsics");
     Has = @("frozen: true true", "extend: TypeError", "override: Fancy ok", "app globals: object object",
             "eval in the program: 2", "frozen done");
     Not = @("SHOULD NOT") },
  # Last: it removes the junction the earlier cases use.
  @{ File = "kavach-links.js";           Exit = 0; Has = @("hardlink: PermissionDenied explained", "junction removed: true", "plain: plain-ok");
     Not = @("SHOULD NOT WRITE");
     Pre = @("--allow-read=$kGranted", "--allow-write=$kGranted");
     Args = @($kHardLink, $kLink, (Join-Path $kGranted "plain.txt"));
     Check = {
       $p = @()
       $secret = Join-Path $kOutside "secret.txt"
       if (-not (Test-Path $secret)) { $p += "the file behind the junction was deleted" }
       elseif ((Get-Content -Raw $secret) -ne "secret-content") { $p += "the outside file was changed" }
       $p
     } }
)

$failures = 0

foreach ($c in $cases) {
  # Phase 9.1: File may be a full path (the module tree is copied to Temp).
  $script = if ([System.IO.Path]::IsPathRooted($c.File)) { $c.File } else { Join-Path $here $c.File }
  $label = if ($c.Name) { $c.Name } else { $c.File }
  $outFile = [System.IO.Path]::GetTempFileName()
  $errFile = [System.IO.Path]::GetTempFileName()

  # Start-Process joins arguments with spaces, so quote each one here.
  $argLine = ""
  if ($c.Pre) { foreach ($a in $c.Pre) { $argLine += "`"$a`" " } }
  $argLine += "`"$script`""
  if ($c.Args) { foreach ($a in $c.Args) { $argLine += " `"$a`"" } }

  $startArgs = @{
    FilePath = $ForgePath; ArgumentList = $argLine; NoNewWindow = $true; Wait = $true; PassThru = $true
    RedirectStandardOutput = $outFile; RedirectStandardError = $errFile
  }
  if ($c.Cwd) { $startArgs.WorkingDirectory = $c.Cwd }
  $p = Start-Process @startArgs
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
    Write-Host ("[runtime-test] {0,-30} PASS" -f $label)
  } else {
    $failures++
    Write-Host ("[runtime-test] {0,-30} FAIL: {1}" -f $label, ($problems -join "; "))
    Write-Host "  output was:"
    Write-Host $output
  }
}

# Remove the junction first, if a test has not already removed it.
# Directory.Delete on a junction removes the link, not its target. (A plain
# `cmd /c rmdir` here wrote "cannot find the file" to stderr once the
# kavach-links test had removed it, and with $ErrorActionPreference = "Stop"
# that ended the script before it reported success.)
# Windows PowerShell 5.1's Remove-Item -Recurse can follow a junction, so
# every junction goes before the trees are removed.
foreach ($j in @($kLink, $rOutLink, $rGitLink, $mOutLink)) {
  try {
    if (Test-Path -LiteralPath $j) { [System.IO.Directory]::Delete($j) }
  } catch {
    Write-Host "[runtime-test] note: could not remove the junction $j ($($_.Exception.Message))"
  }
}

# Phase 9.1: the native fuzzer can create folders nested deeper than 260
# characters (Forge supports long paths), which Windows PowerShell 5.1's
# Remove-Item cannot delete ("The system cannot find the file specified",
# a terminating error that -ErrorAction does not catch). rd with the \\?\
# prefix handles long paths. Folders left by earlier runs are removed too.
# A cleanup problem is reported but never fails the run, so from here on
# errors do not stop the script (rd writes to stderr for files in use).
$ErrorActionPreference = "Continue"
$roots = @($kRoot, $rRoot, $fRoot, $mRoot, $pRoot, $iRoot, $gRoot)
$roots += @(Get-ChildItem -LiteralPath ([System.IO.Path]::GetTempPath()) -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^forge-(kavach|replay|fuzz|module|package|isolation|permission)-test-[0-9a-f]{8}$' } |
            ForEach-Object { $_.FullName })
foreach ($root in ($roots | Select-Object -Unique)) {
  try {
    if (Test-Path -LiteralPath $root) {
      cmd /c rd /s /q "\\?\$root" 2>&1 | Out-Null
      if (Test-Path -LiteralPath $root) { Write-Host "[runtime-test] note: could not remove $root" }
    }
  } catch {
    Write-Host "[runtime-test] note: could not remove $root ($($_.Exception.Message))"
  }
}
Remove-Item Env:\FORGE_KAVACH_TEST, Env:\FORGE_REPLAY_GRANTED, Env:\FORGE_PERM_TEST, Env:\FORGE_PERM_OTHER -ErrorAction SilentlyContinue

if ($failures -eq 0) {
  Write-Host "[runtime-test] ALL PASSED"
  exit 0
}

Write-Host "[runtime-test] $failures FAILED"
exit 1
