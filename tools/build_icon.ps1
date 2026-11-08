<# .SYNOPSIS Exports the existing ArkHelper logo to a Windows multi-resolution ICO. #>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Add-Type -AssemblyName System.Drawing
$source = [Drawing.Image]::FromFile((Join-Path $root 'data/logo.png'))
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @()
try {
    foreach ($size in $sizes) {
        $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $stream = [IO.MemoryStream]::new()
        try {
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage($source, [Drawing.Rectangle]::new(0, 0, $size, $size))
            $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
            $frames += [PSCustomObject]@{ Size = $size; Bytes = $stream.ToArray() }
        } finally { $graphics.Dispose(); $bitmap.Dispose(); $stream.Dispose() }
    }
} finally { $source.Dispose() }
$directory = Join-Path $root 'resources'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$file = [IO.File]::Create((Join-Path $directory 'arkhelp.ico'))
$writer = [IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = $(if ($frame.Size -eq 256) { 0 } else { $frame.Size })
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length); $writer.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
} finally { $writer.Dispose() }
Write-Host 'Exported resources/arkhelp.ico (16, 24, 32, 48, 64, 128, 256 px)'
