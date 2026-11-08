@echo off
"%~dp0ArkHelperLauncher.exe" --uninstall-agent
if errorlevel 1 (
  echo ArkHelper background agent removal failed.
  pause
  exit /b 1
)
echo ArkHelper background agent removed.
pause
