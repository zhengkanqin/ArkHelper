<# .SYNOPSIS Builds and packages the complete portable ArkHelper application. #>
[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string]$Version = '1.3.0',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = $PSScriptRoot
$InputDirectory = Join-Path $Root 'build\package-input'
& (Join-Path $Root 'build.ps1') -Target All -Release -DistDirectory $InputDirectory -SkipTests:$SkipTests
if (-not $SkipTests) {
    & (Join-Path $Root 'verify.ps1') -DistDirectory $InputDirectory
}
$PackageRoot = Join-Path $Root 'package'
$Payload = Join-Path $PackageRoot 'ArkHelper'
if (Test-Path -LiteralPath $Payload) {
    $resolved = [IO.Path]::GetFullPath($Payload)
    if ($resolved -ne [IO.Path]::GetFullPath((Join-Path $Root 'package\ArkHelper'))) {
        throw 'Refusing to remove an unexpected package directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
foreach ($directory in @($Payload, (Join-Path $Payload 'runtime'), (Join-Path $Payload 'licenses'))) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$files = [ordered]@{
    'ArkHelper.exe' = Join-Path $InputDirectory 'ArkHelper.exe'
    'ArkHelperLauncher.exe' = Join-Path $InputDirectory 'ArkHelperLauncher.exe'
    'runtime\ArkHelperRuntime.dll' = Join-Path $InputDirectory 'runtime\ArkHelperRuntime.dll'
    'runtime\ArkHelperBootstrap.dll' = Join-Path $InputDirectory 'runtime\ArkHelperBootstrap.dll'
    'LICENSE' = Join-Path $Root 'LICENSE'
    'README.md' = Join-Path $Root 'README.md'
    'licenses\MinHook.txt' = Join-Path $Root 'third_party\minhook-1.3.4\LICENSE.txt'
    'licenses\LLVM-libcxx.txt' = Join-Path $Root 'third_party\toolchain-licenses\LLVM-libcxx.txt'
    'licenses\LLVM-libcxxabi.txt' = Join-Path $Root 'third_party\toolchain-licenses\LLVM-libcxxabi.txt'
    'licenses\LLVM-libunwind.txt' = Join-Path $Root 'third_party\toolchain-licenses\LLVM-libunwind.txt'
    'licenses\MinGW.txt' = Join-Path $Root 'third_party\toolchain-licenses\MinGW.txt'
    'licenses\Zig.txt' = Join-Path $Root 'third_party\toolchain-licenses\Zig.txt'
}
foreach ($entry in $files.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value)) { throw "Missing package input: $($entry.Value)" }
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $Payload $entry.Key)
}
Copy-Item -LiteralPath (Join-Path $InputDirectory 'data') -Destination (Join-Path $Payload 'data') -Recurse
Get-ChildItem -LiteralPath (Join-Path $Root 'packaging') | Where-Object Name -ne 'README.txt' |
    Copy-Item -Destination $Payload -Recurse -Force
$manifest = [ordered]@{
    product = 'ArkHelper'
    version = $Version
    architecture = 'windows-x64'
    ipc_version = 9
    stage_overview_version = 1
    files = @(Get-ChildItem -LiteralPath $Payload -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($Payload.Length + 1).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Payload 'manifest.json') -Encoding utf8
$Archive = Join-Path $PackageRoot "ArkHelper-$Version-windows-x64.zip"
Compress-Archive -LiteralPath $Payload -DestinationPath $Archive -Force
if (-not $SkipTests) {
    & (Join-Path $Root 'tools\verify_portable.ps1') -PackageArchive $Archive -TestHost (Join-Path $InputDirectory 'runtime\ArkHelperRuntimeHost.exe')
}
$SourceRoot = Join-Path $Root 'build\package-source'
$SourcePayload = Join-Path $SourceRoot 'ArkHelper'
if (Test-Path -LiteralPath $SourceRoot) {
    $resolved = [IO.Path]::GetFullPath($SourceRoot)
    if ($resolved -ne [IO.Path]::GetFullPath((Join-Path $Root 'build\package-source'))) {
        throw 'Refusing to clean an unexpected source staging directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Path $SourcePayload -Force | Out-Null
foreach ($entry in @('src', 'native', 'include', 'tests', 'third_party', 'data', 'resources', 'tools',
    'packaging', 'build.ps1', 'verify.ps1', 'package.ps1', 'README.md', 'LICENSE', '.clang-format', '.gitignore', '.gitattributes')) {
    Copy-Item -LiteralPath (Join-Path $Root $entry) -Destination $SourcePayload -Recurse -Force
}
foreach ($relative in @('native/modules/provider/README.md', 'tests/prts/prts/README.md', 'packaging/README.txt')) {
    $localNote = [IO.Path]::GetFullPath((Join-Path $SourcePayload $relative))
    if (-not $localNote.StartsWith([IO.Path]::GetFullPath($SourcePayload).TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unexpected source note path.'
    }
    if (Test-Path -LiteralPath $localNote) { Remove-Item -LiteralPath $localNote -Force }
}
$SourceArchive = Join-Path $PackageRoot "ArkHelper-$Version-source.zip"
Compress-Archive -LiteralPath $SourcePayload -DestinationPath $SourceArchive -Force
Write-Host "Portable application: $Payload"
Write-Host "Distribution archive: $Archive"
Write-Host "Corresponding source: $SourceArchive"
