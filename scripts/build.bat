@echo off
rem Runs build_all.ps1 with the execution policy bypassed for this call only
rem (DEVELOPMENT_RULES 1). Pass-through args, e.g.  scripts\build.bat -Install
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_all.ps1" %*
