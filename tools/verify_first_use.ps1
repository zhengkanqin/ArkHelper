<# .SYNOPSIS Exercises the real first-use window in an extracted test distribution only. #>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Payload)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$payloadRoot = [IO.Path]::GetFullPath($Payload)
$allowed = [IO.Path]::GetFullPath((Join-Path $root 'build/portable-check')).TrimEnd('\') + '\'
if (-not $payloadRoot.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'First-use automation is restricted to extracted portable-check fixtures; never acknowledge a real installation.'
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ArkHelperNoticeTest {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr window, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr window, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] private static extern bool AttachThreadInput(uint from, uint to, bool attach);
    [DllImport("user32.dll")] private static extern bool BringWindowToTop(IntPtr window);
    public static void FocusFixture(IntPtr window) {
        uint ignored;
        uint current = GetCurrentThreadId();
        uint foreground = GetWindowThreadProcessId(GetForegroundWindow(), out ignored);
        uint target = GetWindowThreadProcessId(window, out ignored);
        bool attachedForeground = foreground != 0 && foreground != current && AttachThreadInput(current, foreground, true);
        bool attachedTarget = target != 0 && target != current && target != foreground && AttachThreadInput(current, target, true);
        try { BringWindowToTop(window); SetForegroundWindow(window); }
        finally {
            if (attachedTarget) AttachThreadInput(current, target, false);
            if (attachedForeground) AttachThreadInput(current, foreground, false);
        }
    }
}
'@
function Start-TestApp {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $payloadRoot 'ArkHelper.exe'
    $start.WorkingDirectory = $payloadRoot
    $start.UseShellExecute = $false
    $start.Environment['ARKHELP_NO_GAME_LAUNCH'] = '1'
    $start.Environment['ARKHELP_PANEL_DEMO'] = '1'
    $start.Environment['ARKHELP_KEEP_RUNTIME'] = '1'
    return [Diagnostics.Process]::Start($start)
}
function Find-Owned([string]$Class, [int]$OwnerId) {
    $window = [ArkHelperNoticeTest]::FindWindowW($Class, [NullString]::Value)
    [uint32]$owner = 0
    [ArkHelperNoticeTest]::GetWindowThreadProcessId($window, [ref]$owner) | Out-Null
    if ($owner -eq $OwnerId) { return $window }
    return [IntPtr]::Zero
}
function Wait-Window([string]$Class, [Diagnostics.Process]$App) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $window = Find-Owned $Class $App.Id
        if ($window -ne [IntPtr]::Zero) { return $window }
        Start-Sleep -Milliseconds 50
    } while (-not $App.HasExited -and [DateTime]::UtcNow -lt $deadline)
    throw "Missing $Class for test PID $($App.Id)"
}
function Check-Blocked([Diagnostics.Process]$App) {
    if ((Find-Owned 'ArkHelp.Shell' $App.Id) -ne [IntPtr]::Zero) { throw 'Shell became usable before acknowledgement.' }
    try {
        $ready = [Threading.Mutex]::OpenExisting('Local\ArkHelp.App')
        $ready.Dispose()
        throw 'Agent connection marker exists before acknowledgement.'
    } catch [Threading.WaitHandleCannotBeOpenedException] { }
}
function Close-App([Diagnostics.Process]$App, [IntPtr]$Window, [uint32]$Message) {
    [ArkHelperNoticeTest]::PostMessageW($Window, $Message, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $App.WaitForExit(10000) -or $App.ExitCode -ne 0) { throw 'Test app did not exit cleanly.' }
}
$configPath = Join-Path $payloadRoot 'config/arkhelp.json'
if (Test-Path -LiteralPath $configPath) { throw 'First-use fixture must start without config.' }
$app = Start-TestApp
try {
    $window = Wait-Window 'ArkHelper.RiskNotice' $app
    Check-Blocked $app
    $accept = [ArkHelperNoticeTest]::GetDlgItem($window, 1)
    $check = [ArkHelperNoticeTest]::GetDlgItem($window, 1001)
    if ([ArkHelperNoticeTest]::IsWindowEnabled($accept)) { throw 'Accept initially enabled.' }
    if ([ArkHelperNoticeTest]::SendMessageW($check, 0xF0, [IntPtr]::Zero, [IntPtr]::Zero) -ne [IntPtr]::Zero) {
        throw 'Risk checkbox must be unchecked by default.'
    }
    # Deliberate early command injection must not bypass the actual time guard.
    [ArkHelperNoticeTest]::SendMessageW($check, 0xF1, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    [ArkHelperNoticeTest]::SendMessageW($window, 0x111, [IntPtr]1, $accept) | Out-Null
    Check-Blocked $app
    Close-App $app $window 0x10
    if ((Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json).risk_notice_version -ne 0) {
        throw 'Declining or early click persisted acknowledgement.'
    }
    Write-Host 'PASS first-use rejection: default unchecked, early command denied, no connection marker, clean exit'
} finally { if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() } }
$app = Start-TestApp
try {
    $window = Wait-Window 'ArkHelper.RiskNotice' $app
    $accept = [ArkHelperNoticeTest]::GetDlgItem($window, 1)
    $check = [ArkHelperNoticeTest]::GetDlgItem($window, 1001)
    $focusDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        [ArkHelperNoticeTest]::FocusFixture($window)
        Start-Sleep -Milliseconds 100
    } while ([ArkHelperNoticeTest]::GetForegroundWindow() -ne $window -and [DateTime]::UtcNow -lt $focusDeadline)
    if ([ArkHelperNoticeTest]::GetForegroundWindow() -ne $window) { throw 'Cannot foreground test notice for continuous-reading check.' }
    $reading = [Diagnostics.Stopwatch]::StartNew()
    $deadline = [DateTime]::UtcNow.AddSeconds(18)
    do {
        Check-Blocked $app
        if ([ArkHelperNoticeTest]::GetForegroundWindow() -ne $window) { throw 'First-use test lost foreground; reading interval interrupted.' }
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Reading timer did not finish.' }
        Start-Sleep -Milliseconds 100
    } while ($reading.ElapsedMilliseconds -le 10500)
    if ([ArkHelperNoticeTest]::IsWindowEnabled($accept)) { throw 'Reading without checkbox enabled acceptance.' }
    [ArkHelperNoticeTest]::SendMessageW($check, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not [ArkHelperNoticeTest]::IsWindowEnabled($accept)) { throw 'Reading plus checkbox did not unlock acceptance.' }
    [ArkHelperNoticeTest]::SendMessageW($window, 0x111, [IntPtr]1, $accept) | Out-Null
    $shell = Wait-Window 'ArkHelp.Shell' $app
    if ((Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json).risk_notice_version -ne 1) { throw 'Acknowledgement did not persist.' }
    Close-App $app $shell 0x8006
    Write-Host 'PASS first-use acceptance: over 10 seconds, explicit checkbox, saved acknowledgement, shell enabled'
} finally { if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() } }
$app = Start-TestApp
try {
    $shell = Wait-Window 'ArkHelp.Shell' $app
    if ((Find-Owned 'ArkHelper.RiskNotice' $app.Id) -ne [IntPtr]::Zero) { throw 'Accepted notice repeated on restart.' }
    Close-App $app $shell 0x8006
    Write-Host 'PASS acknowledgement survives restart'
} finally { if (-not $app.HasExited) { $app.Kill(); $app.WaitForExit() } }
