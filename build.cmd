@echo off
REM Double-click this, or run it from a terminal.
REM
REM PowerShell refuses to run unsigned scripts that came from the internet, and
REM build.ps1 will have, since you downloaded it. Invoking it this way scopes
REM the exemption to this one script instead of asking you to weaken a
REM machine-wide security setting for a hobby project.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
if errorlevel 1 (
  echo.
  echo Build failed. If the error mentions 'zig', install it from
  echo https://ziglang.org/download/ and put zig.exe on your PATH.
)
pause
