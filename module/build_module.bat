@echo off
rem Builds gmcl_rcx_win64.dll into module\builds\win64\ (see tools\build_module.ps1).
rem Arguments pass through, e.g. build_module.bat -Install, -Rollback, -Clean (see README.md).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_module.ps1" %*
set result=%errorlevel%
pause
exit /b %result%
