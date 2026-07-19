[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPng,

    [Parameter(Mandatory = $true)]
    [string]$OutputIco,

    [string]$PreviewPng
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$sourcePath = (Resolve-Path -LiteralPath $InputPng).Path
$source = [System.Drawing.Bitmap]::FromFile($sourcePath)

function New-IconBitmap {
    param([int]$Size)

    $bitmap = New-Object System.Drawing.Bitmap(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.DrawImage($source, 0, 0, $Size, $Size)
    $graphics.Dispose()
    return $bitmap
}

function Convert-BitmapToPngBytes {
    param([System.Drawing.Bitmap]$Bitmap)

    $stream = New-Object System.IO.MemoryStream
    $Bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $stream.ToArray()
    $stream.Dispose()
    return ,$bytes
}

try {
    $sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    $frames = @()
    foreach ($size in $sizes) {
        $bitmap = New-IconBitmap -Size $size
        $frames += [PSCustomObject]@{
            Size = $size
            Bytes = [byte[]](Convert-BitmapToPngBytes -Bitmap $bitmap)
        }
        if ($PreviewPng -and $size -eq 256) {
            $bitmap.Save($PreviewPng, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        $bitmap.Dispose()
    }

    $output = New-Object System.IO.FileStream(
        $OutputIco,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write)
    $writer = New-Object System.IO.BinaryWriter($output)
    try {
        $writer.Write([UInt16]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]$frames.Count)

        $offset = 6 + ($frames.Count * 16)
        foreach ($frame in $frames) {
            $sizeByte = if ($frame.Size -eq 256) { 0 } else { [byte]$frame.Size }
            $writer.Write([byte]$sizeByte)
            $writer.Write([byte]$sizeByte)
            $writer.Write([byte]0)
            $writer.Write([byte]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]32)
            $writer.Write([UInt32]$frame.Bytes.Length)
            $writer.Write([UInt32]$offset)
            $offset += $frame.Bytes.Length
        }

        foreach ($frame in $frames) {
            $writer.Write([byte[]]$frame.Bytes)
        }
    } finally {
        $writer.Dispose()
        $output.Dispose()
    }
} finally {
    $source.Dispose()
}
