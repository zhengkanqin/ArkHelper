Option Explicit
Dim fso, shell, root, destination, link
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
root = fso.GetParentFolderName(WScript.ScriptFullName)
destination = shell.SpecialFolders("Desktop")
If WScript.Arguments.Count > 0 Then destination = fso.GetAbsolutePathName(WScript.Arguments(0))
Set link = shell.CreateShortcut(destination & "\ArkHelper.lnk")
link.TargetPath = root & "\ArkHelper.exe"
link.Arguments = ""
link.WindowStyle = 1
link.WorkingDirectory = root
link.IconLocation = root & "\ArkHelper.exe,0"
link.Description = "ArkHelper"
link.Save
