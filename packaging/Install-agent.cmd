@echo off
"%~dp0ArkHelperLauncher.exe" --install-agent
if errorlevel 1 (
  echo ArkHelper background agent installation failed.
  pause
  exit /b 1
)
echo ArkHelper background agent installed.
pause
