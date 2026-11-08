@echo off
cscript.exe //B //Nologo "%~dp0Create-shortcut.vbs"
if errorlevel 1 exit /b 1
echo ArkHelper desktop shortcut created.
