<# .SYNOPSIS Checks an extracted distribution in a path containing spaces and Chinese text. #>
[CmdletBinding()]
param(
    [string]$PackageArchive = 'package/ArkHelper-1.2.1-windows-x64.zip',
    [string]$TestHost = 'build/package-input/runtime/ArkHelperRuntimeHost.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
function Absolute([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $root $Path))
}
$destination = Join-Path $root ('build/portable-check/中文 空格-' + [Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath (Absolute $PackageArchive) -DestinationPath $destination
$payload = Join-Path $destination 'ArkHelper'
$manifest = Get-Content -LiteralPath (Join-Path $payload 'manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $path = Join-Path $payload $entry.path
    if ((Get-Item -LiteralPath $path).Length -ne $entry.bytes -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) {
        throw "Distribution file mismatch: $($entry.path)"
    }
}
$binaries = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object { $_.Extension -in '.exe', '.dll' })
$expected = @('ArkHelper.exe', 'ArkHelperLauncher.exe', 'ArkHelperRuntime.dll', 'ArkHelperBootstrap.dll')
if ($binaries.Count -ne 4 -or @(Compare-Object ($binaries.Name | Sort-Object) ($expected | Sort-Object)).Count) {
    throw 'Unexpected distribution binaries.'
}
foreach ($name in @('ArkHelper.exe', 'ArkHelperLauncher.exe')) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $payload $name))
    $pe = [BitConverter]::ToInt32($bytes, 60)
    if ([BitConverter]::ToUInt16($bytes, $pe + 24 + 68) -ne 2) {
        throw "Product executable must use the Windows GUI subsystem: $name"
    }
}
Write-Host 'PASS Windows GUI subsystem: desktop and background launcher do not allocate consoles'
Write-Host "PASS portable manifest: $($manifest.files.Count) files, exactly four product binaries"
if (Test-Path -LiteralPath (Join-Path $payload 'runtime/data/arkhelper-pack.json')) {
    throw 'Distribution must not include the retired all-stage data pack.'
}
# The real distribution remains untouched; the development host is added only to this test copy.
Copy-Item -LiteralPath (Absolute $TestHost) -Destination (Join-Path $payload 'runtime/ArkHelperRuntimeHost.exe')
& (Join-Path $root 'verify.ps1') -DistDirectory $payload

function Start-Direct([string]$Executable, [string[]]$Arguments = @(), [switch]$Demo) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = $destination # Deliberately different from the EXE directory.
    $start.UseShellExecute = $false
    $start.Environment['ARKHELP_NO_GAME_LAUNCH'] = '1'
    # Launch product EXEs normally so the test cannot hide a console regression.
    $start.CreateNoWindow = [IO.Path]::GetFileName($Executable) -notin @('ArkHelper.exe', 'ArkHelperLauncher.exe')
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    if ($Demo) {
        $start.Environment['ARKHELP_PANEL_DEMO'] = '1'
        $start.Environment['ARKHELP_KEEP_RUNTIME'] = '1'
    }
    return [Diagnostics.Process]::Start($start)
}

# Exercise the native agent while the desktop app is closed: no game connection occurs.
$agent = Start-Direct (Join-Path $payload 'ArkHelperLauncher.exe') @('--agent')
try {
    $stop = $null
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        try { $stop = [Threading.EventWaitHandle]::OpenExisting('Local\ArkHelp.Agent.Stop') } catch [Threading.WaitHandleCannotBeOpenedException] { }
        if (-not $stop) { Start-Sleep -Milliseconds 50 }
    } while (-not $stop -and -not $agent.HasExited -and [DateTime]::UtcNow -lt $deadline)
    if (-not $stop -or $agent.HasExited) { throw 'Native agent did not start.' }
    $second = Start-Direct (Join-Path $payload 'ArkHelperLauncher.exe') @('--agent')
    if (-not $second.WaitForExit(5000) -or $second.ExitCode -ne 0) { throw 'Agent singleton check failed.' }
    $stop.Set() | Out-Null
    $stop.Dispose()
    if (-not $agent.WaitForExit(5000) -or $agent.ExitCode -ne 0) { throw 'Native agent did not stop cleanly.' }
    Write-Host 'PASS native agent: starts without elevation, rejects duplicate, stops cleanly'
} finally {
    if (-not $agent.HasExited) { $agent.Kill(); $agent.WaitForExit() }
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ArkHelperPortableWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint msg, IntPtr w, IntPtr l);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] public static extern uint ExtractIconExW(string file, int index, IntPtr large, IntPtr small, uint count);
    public static IntPtr Shell() { return FindWindowW("ArkHelp.Shell", null); }
}
'@
foreach ($name in @('ArkHelper.exe', 'ArkHelperLauncher.exe')) {
    if ([ArkHelperPortableWindow]::ExtractIconExW((Join-Path $payload $name), -1, [IntPtr]::Zero, [IntPtr]::Zero, 0) -lt 1) {
        throw "Missing embedded application icon: $name"
    }
}
$shortcutDirectory = Join-Path $destination 'test shortcuts'
New-Item -ItemType Directory -Path $shortcutDirectory -Force | Out-Null
$shell = New-Object -ComObject WScript.Shell
$legacyShortcut = $shell.CreateShortcut((Join-Path $shortcutDirectory 'ArkHelper.lnk'))
$legacyShortcut.TargetPath = Join-Path $env:WINDIR 'System32/wscript.exe'
$legacyShortcut.Arguments = '//B legacy-start.vbs'
$legacyShortcut.Save()
$creator = Start-Direct (Join-Path $env:WINDIR 'System32/cscript.exe') @('//B', '//Nologo', (Join-Path $payload 'Create-shortcut.vbs'), $shortcutDirectory)
if (-not $creator.WaitForExit(5000) -or $creator.ExitCode -ne 0) { throw 'Shortcut creation failed.' }
$shortcut = $shell.CreateShortcut((Join-Path $shortcutDirectory 'ArkHelper.lnk'))
if ($shortcut.TargetPath -ne (Join-Path $payload 'ArkHelper.exe') -or
    $shortcut.IconLocation -ne ((Join-Path $payload 'ArkHelper.exe') + ',0') -or
    $shortcut.WorkingDirectory -ne $payload -or $shortcut.Arguments -ne '') { throw 'Shortcut path or icon did not relocate.' }
Write-Host 'PASS embedded EXE icons and portable shortcut creation'
& (Join-Path $root 'tools/verify_first_use.ps1') -Payload $payload
$app = Start-Direct (Join-Path $payload 'ArkHelper.exe') -Demo
$window = [IntPtr]::Zero
try {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $candidate = [ArkHelperPortableWindow]::Shell()
        [uint32]$ownerPid = 0
        [ArkHelperPortableWindow]::GetWindowThreadProcessId($candidate, [ref]$ownerPid) | Out-Null
        if ($ownerPid -eq $app.Id) { $window = $candidate; break }
        Start-Sleep -Milliseconds 100
    } while (-not $app.HasExited -and [DateTime]::UtcNow -lt $deadline)
    if ($window -eq [IntPtr]::Zero -or $app.HasExited) { throw "Portable desktop window did not initialize (PID $($app.Id), exited=$($app.HasExited))." }
    Start-Sleep -Seconds 3
    [ArkHelperPortableWindow]::PostMessageW($window, 0x8006, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $app.WaitForExit(15000) -or $app.ExitCode -ne 0) { throw 'Portable desktop did not close cleanly.' }
    $config = Get-Content -LiteralPath (Join-Path $payload 'config/arkhelp.json') -Raw | ConvertFrom-Json
    if ($config.loader_path -ne (Join-Path $payload 'ArkHelperLauncher.exe')) { throw 'Launcher path did not relocate.' }
    $log = Get-Content -LiteralPath (Join-Path $payload "logs/ArkHelper-$($app.Id).log") -Raw
    if ($log -notmatch 'shutdown complete') { throw 'Desktop shutdown was incomplete.' }
    Write-Host 'PASS portable desktop: offline panels, relocated configuration, clean shutdown, no startup prompts'
} finally {
    if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() }
}
Write-Host "Portable check artifacts: $destination"
