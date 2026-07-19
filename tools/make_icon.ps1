param(
    [string]$OutputIco = "assets\LanSpeak.ico",
    [string]$PreviewPng = "assets\LanSpeakIconPreview.png",
    [string]$SizesPreviewPng = "assets\LanSpeakIconSizesPreview.png"
)

Add-Type -AssemblyName System.Drawing

function New-IconBitmap {
    param([int]$Size)

    $bitmap = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.Clear([System.Drawing.Color]::Transparent)

    $scale = $Size / 256.0
    function S([double]$Value) { return [single]($Value * $scale) }

    $bubbleBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.RectangleF((S 8), (S 20), (S 240), (S 218))),
        [System.Drawing.Color]::FromArgb(255, 55, 222, 214),
        [System.Drawing.Color]::FromArgb(255, 25, 154, 235),
        [System.Drawing.Drawing2D.LinearGradientMode]::ForwardDiagonal)

    $bubblePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bubbleRect = New-Object System.Drawing.RectangleF((S 8), (S 20), (S 240), (S 172))
    $bubbleRadius = S 50
    $bubbleDiameter = $bubbleRadius * 2
    $bubblePath.AddArc($bubbleRect.X, $bubbleRect.Y, $bubbleDiameter, $bubbleDiameter, 180, 90)
    $bubblePath.AddArc($bubbleRect.Right - $bubbleDiameter, $bubbleRect.Y, $bubbleDiameter, $bubbleDiameter, 270, 90)
    $bubblePath.AddArc($bubbleRect.Right - $bubbleDiameter, $bubbleRect.Bottom - $bubbleDiameter, $bubbleDiameter, $bubbleDiameter, 0, 90)
    $bubblePath.AddLine((S 198), (S 192), (S 112), (S 192))
    $bubblePath.AddLine((S 112), (S 192), (S 66), (S 238))
    $bubblePath.AddLine((S 66), (S 238), (S 80), (S 192))
    $bubblePath.AddLine((S 80), (S 192), (S 58), (S 192))
    $bubblePath.AddArc($bubbleRect.X, $bubbleRect.Bottom - $bubbleDiameter, $bubbleDiameter, $bubbleDiameter, 90, 90)
    $bubblePath.CloseFigure()
    $graphics.FillPath($bubbleBrush, $bubblePath)

    $waveAmplitudes = @(0, 2, 3, 5, 9, 15, 8, 22, 12, 33, 18, 42, 28, 55, 35, 20, 44, 25, 12, 36, 58, 30, 18, 47, 24, 14, 37, 21, 50, 34, 18, 40, 25, 14, 22, 10, 6, 3, 1, 0)
    $wavePath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $topPoints = @()
    $bottomPoints = @()
    for ($index = 0; $index -lt $waveAmplitudes.Count; $index++) {
        $x = 30 + ($index * 5)
        $amplitude = $waveAmplitudes[$index]
        $topPoints += New-Object System.Drawing.PointF((S $x), (S (106 - $amplitude)))
        $bottomPoints += New-Object System.Drawing.PointF((S $x), (S (106 + $amplitude)))
    }
    [Array]::Reverse($bottomPoints)
    $wavePath.AddLines([System.Drawing.PointF[]]($topPoints + $bottomPoints))
    $wavePath.CloseFigure()
    $waveBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(248, 255, 255, 255))
    $graphics.FillPath($waveBrush, $wavePath)

    $waveBrush.Dispose()
    $wavePath.Dispose()
    $bubblePath.Dispose()
    $bubbleBrush.Dispose()
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

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$frames = @()
foreach ($size in $sizes) {
    $bitmap = New-IconBitmap -Size $size
    $pngBytes = [byte[]](Convert-BitmapToPngBytes -Bitmap $bitmap)
    $frames += [PSCustomObject]@{
        Size = $size
        Bytes = $pngBytes
    }
    if ($size -eq 256) {
        $bitmap.Save($PreviewPng, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $bitmap.Dispose()
}

$output = New-Object System.IO.FileStream($OutputIco, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$writer = New-Object System.IO.BinaryWriter($output)
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

$writer.Dispose()
$output.Dispose()

$previewScale = 4
$previewPadding = 16
$previewGap = 18
$previewSizes = @(16, 20, 24, 32, 40, 48)
$previewWidth = $previewPadding * 2 + (($previewSizes | Measure-Object -Sum).Sum * $previewScale) + (($previewSizes.Count - 1) * $previewGap)
$previewHeight = 224
$preview = New-Object System.Drawing.Bitmap($previewWidth, $previewHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$previewGraphics = [System.Drawing.Graphics]::FromImage($preview)
$previewGraphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
$previewGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$previewGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$previewGraphics.Clear([System.Drawing.Color]::FromArgb(255, 245, 247, 249))

$checkerLight = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 245, 247, 249))
$checkerDark = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 222, 227, 232))
for ($y = 0; $y -lt $previewHeight; $y += 8) {
    for ($x = 0; $x -lt $previewWidth; $x += 8) {
        $brush = if ((($x / 8) + ($y / 8)) % 2 -eq 0) { $checkerLight } else { $checkerDark }
        $previewGraphics.FillRectangle($brush, $x, $y, 8, 8)
    }
}

$xCursor = $previewPadding
foreach ($size in $previewSizes) {
    $iconBitmap = New-IconBitmap -Size $size
    $drawSize = $size * $previewScale
    $y = [int](($previewHeight - $drawSize) / 2)
    $previewGraphics.DrawImage($iconBitmap, $xCursor, $y, $drawSize, $drawSize)
    $iconBitmap.Dispose()
    $xCursor += $drawSize + $previewGap
}

$checkerLight.Dispose()
$checkerDark.Dispose()
$previewGraphics.Dispose()
$preview.Save($SizesPreviewPng, [System.Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()
