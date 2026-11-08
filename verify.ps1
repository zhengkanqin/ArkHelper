<# .SYNOPSIS Verifies the integrated runtime and both loading methods without a game. #>
[CmdletBinding()]
param([string]$DistDirectory = 'dist')
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Dist = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($DistDirectory)) {
    $DistDirectory
} else { Join-Path $Root $DistDirectory }))
function Run-Checked([string]$Executable, [string[]]$Arguments) {
    $logDirectory = Join-Path $Dist 'runtime\logs'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $name = ($Arguments -join '-') -replace '[^a-zA-Z0-9-]', ''
    $stdout = Join-Path $logDirectory ("verify-$name.out.txt")
    $stderr = Join-Path $logDirectory ("verify-$name.err.txt")
    # Direct process creation avoids Explorer's attachment warning for our own
    # freshly compiled binaries. It does not change Windows security policy.
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = $Dist
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    $output = $process.StandardOutput.ReadToEndAsync()
    $errors = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(75000)) {
        Stop-Process -Id $process.Id -Force
        throw "Integration check timed out: $Executable $Arguments"
    }
    [IO.File]::WriteAllText($stdout, $output.GetAwaiter().GetResult())
    [IO.File]::WriteAllText($stderr, $errors.GetAwaiter().GetResult())
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $stdout, $stderr | Write-Host
        throw "Integration check failed (exit $($process.ExitCode)): $Executable $Arguments"
    }
    Write-Host "PASS $([IO.Path]::GetFileName($Executable)) $Arguments"
}
Run-Checked (Join-Path $Dist 'runtime\ArkHelperRuntimeHost.exe') @('--self-test')
Run-Checked (Join-Path $Dist 'ArkHelperLauncher.exe') @('--load-runtime', '--self-test', '--method', 'remote-thread', '--timeout-seconds', '30')
Run-Checked (Join-Path $Dist 'ArkHelperLauncher.exe') @('--load-runtime', '--self-test', '--method', 'window-hook', '--timeout-seconds', '30')
