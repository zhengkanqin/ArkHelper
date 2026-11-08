Option Explicit
' Compatibility entry for installations created before the native agent was integrated.
Dim fso, shell, launcher, child
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
launcher = fso.GetParentFolderName(fso.GetParentFolderName(WScript.ScriptFullName)) & "\ArkHelperLauncher.exe"
' Exec uses direct process creation and avoids the Shell attachment warning.
Set child = shell.Exec("""" & launcher & """ --agent")
Do While child.Status = 0
  WScript.Sleep 1000
Loop
