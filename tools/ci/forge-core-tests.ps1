<#
.SYNOPSIS
  Builds and runs forge-core's unit and smoke tests with MSVC (cl.exe),
  under the project's real constraints: C++17, exceptions disabled
  (Phase 7.9). Includes the Windows-only smoke tests (File, Socket,
  threads, timers, IOCP cross-thread wake-ups).

  Run from a plain PowerShell; the script finds Visual Studio itself.
#>
$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$core = Join-Path $root "forge-core"
$out = Join-Path ([System.IO.Path]::GetTempPath()) "forge-core-tests"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Write-Error "Visual Studio with the C++ tools was not found"; exit 2 }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$mem  = "memory\DefaultAllocator.cpp memory\detail\AllocationBackend.cpp"
$sync = "Mutex.cpp ConditionVariable.cpp Thread.cpp platform\Win32Error.cpp"

# Name, sources (relative to forge-core\), extra link input.
$tests = @(
  @{ Name = "ArrayTest";          Src = "ArrayTest.cpp $mem" },
  @{ Name = "ErasedCallableTest"; Src = "ErasedCallableTest.cpp $mem" },
  @{ Name = "HashMapTest";        Src = "HashMapTest.cpp $mem" },
  @{ Name = "HashSetTest";        Src = "HashSetTest.cpp $mem" },
  @{ Name = "IpAddressTest";      Src = "IpAddressTest.cpp $mem" },
  @{ Name = "OverflowTest";       Src = "OverflowTest.cpp $mem" },
  @{ Name = "PathTest";           Src = "PathTest.cpp $mem" },
  @{ Name = "QueueTest";          Src = "QueueTest.cpp $mem" },
  @{ Name = "StackTest";          Src = "StackTest.cpp $mem" },
  @{ Name = "StringTest";         Src = "StringTest.cpp $mem" },
  @{ Name = "VectorSmokeTest";    Src = "..\VectorSmokeTest.cpp $mem" },
  @{ Name = "FileSmokeTest";      Src = "FileSmokeTest.cpp File.cpp platform\Win32Error.cpp $mem" },
  @{ Name = "ThreadingSmokeTest"; Src = "ThreadingSmokeTest.cpp $sync ThreadPool.cpp $mem" },
  @{ Name = "LockGuardTest";      Src = "LockGuardTest.cpp $sync $mem" },
  @{ Name = "TimerSchedulerTest"; Src = "platform\TimerSchedulerTest.cpp platform\TimerScheduler.cpp $sync $mem" },
  @{ Name = "IocpCrossThreadSmokeTest"; Src = "..\IocpCrossThreadSmokeTest.cpp platform\IocpLoop.cpp platform\TimerScheduler.cpp $sync ThreadPool.cpp $mem" },
  @{ Name = "SocketSmokeTest";    Src = "SocketSmokeTest.cpp Socket.cpp $sync $mem"; Link = "ws2_32.lib" },
  @{ Name = "PathRulesTest";      Src = "..\kavach\PathRulesTest.cpp ..\kavach\PathRules.cpp $mem"; Inc = ".." },
  @{ Name = "HostTest";           Src = "platform\HostTest.cpp platform\Unicode.cpp platform\Environment.cpp platform\Stdio.cpp platform\Win32Error.cpp $mem" }
)

$failures = 0
Push-Location $core
try {
  foreach ($t in $tests) {
    $exe = Join-Path $out "$($t.Name).exe"
    $link = if ($t.Link) { "/link $($t.Link)" } else { "" }
    # Inc: an extra include folder, relative to forge-core\ (Phase 8.2:
    # the Kavach tests include "kavach/..." and "forge-core/..." from the root).
    $inc = if ($t.Inc) { "/I`"$($t.Inc)`"" } else { "" }
    $cmd = "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHs- /D_HAS_EXCEPTIONS=0 /W4 $inc /Fo`"$out\\`" /Fe`"$exe`" $($t.Src) $link"
    cmd /c $cmd > (Join-Path $out "$($t.Name).build.log") 2>&1

    if ($LASTEXITCODE -ne 0) {
      $failures++
      Write-Host ("[forge-core] {0,-26} BUILD FAIL" -f $t.Name)
      Get-Content (Join-Path $out "$($t.Name).build.log")
      continue
    }

    & $exe > (Join-Path $out "$($t.Name).log") 2>&1
    if ($LASTEXITCODE -ne 0) {
      $failures++
      Write-Host ("[forge-core] {0,-26} FAIL" -f $t.Name)
      Get-Content (Join-Path $out "$($t.Name).log")
    } else {
      Write-Host ("[forge-core] {0,-26} PASS" -f $t.Name)
    }
  }
} finally {
  Pop-Location
}

if ($failures -ne 0) { Write-Host "[forge-core] $failures FAILED"; exit 1 }
Write-Host "[forge-core] ALL PASSED"
