<# .SYNOPSIS Runs development-only checks against the real menu and ball renderers. #>
[CmdletBinding()]
param([string]$OutputDirectory = 'build/ui-review-current', [switch]$SharedMouse)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = if ([IO.Path]::IsPathRooted($OutputDirectory)) { $OutputDirectory } else { Join-Path $root $OutputDirectory }
$zig = Join-Path $root '.tools/zig/ziglang/zig.exe'
if (-not (Test-Path -LiteralPath $zig)) { $zig = (Get-Command zig -ErrorAction Stop).Source }
New-Item -ItemType Directory -Path $output, (Join-Path $output 'tmp') -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $root 'build/zig-cache'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $output 'zig-cache'
$env:TEMP = Join-Path $output 'tmp'
$env:TMP = $env:TEMP
$reviewData = Join-Path $output 'data'
New-Item -ItemType Directory -Path $reviewData -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'data') | Copy-Item -Destination $reviewData -Recurse -Force
$reviews = @('ui_ball_review', 'ui_menu_review')
if ($SharedMouse) { $reviews += 'ui_shared_mouse_review' }
foreach ($name in $reviews) {
    $inputFlags = @()
    if ($name -eq 'ui_shared_mouse_review') { $inputFlags += '-DARKHELP_MOUSE_RELAY_SUFFIX=L".MouseReview"' }
    $executable = Join-Path $output ($name + '.exe')
    & $zig c++ @inputFlags -target x86_64-windows-gnu -std=c++20 -O2 -DNOMINMAX -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN `
        ('-I' + (Join-Path $root 'include')) ('-I' + (Join-Path $root 'src')) `
        (Join-Path $root "tests/$name.cpp") (Join-Path $root 'src/shell/win_util.cpp') `
        (Join-Path $root 'src/ui/shared_mouse_input.cpp') `
        (Join-Path $root 'src/log/log.cpp') -luser32 -lgdi32 -lshell32 -ladvapi32 -lole32 -luuid `
        -lwindowscodecs -lmsimg32 -ldwmapi -lshlwapi -lcomdlg32 -municode -o $executable
    if ($LASTEXITCODE -ne 0) { throw "UI review compile failed: $name" }
    & $executable $output
    if ($LASTEXITCODE -ne 0) { throw "UI review failed: $name" }
}
Add-Type -AssemblyName System.Drawing
foreach ($bitmap in Get-ChildItem -LiteralPath $output -Filter '*.bmp') {
    $source = [Drawing.Image]::FromFile($bitmap.FullName)
    try { $source.Save([IO.Path]::ChangeExtension($bitmap.FullName, '.png'), [Drawing.Imaging.ImageFormat]::Png) }
    finally { $source.Dispose() }
}
Write-Host "UI review images: $output"
