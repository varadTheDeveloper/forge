@echo off
rem Builds Forge against the pinned engine and installs it to C:\Forge\bin.
rem Extra arguments go to tools\build.ps1, for example:  build.bat -Test
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" %*
exit /b %errorlevel%
