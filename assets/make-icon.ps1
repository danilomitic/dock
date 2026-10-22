# Builds the DockX app icon from assets\dockx-logo.png:
# the DX logo in a black macOS-style rounded square with transparent corners,
# written as assets\dockx.ico (16-256 px) and assets\dockx.png (256 px preview).
# Run:  powershell -ExecutionPolicy Bypass -File assets\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$logoPath = Join-Path $PSScriptRoot 'dockx-logo.png'
$logo = [System.Drawing.Bitmap]::FromFile($logoPath)

# Find the logo (everything that is not black) so it can be centered exactly.
$minX = $logo.Width; $minY = $logo.Height; $maxX = 0; $maxY = 0
for ($y = 0; $y -lt $logo.Height; $y += 2) {
    for ($x = 0; $x -lt $logo.Width; $x += 2) {
        $p = $logo.GetPixel($x, $y)
        if (($p.R + $p.G + $p.B) -gt 90) {
            if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
$logoW = $maxX - $minX; $logoH = $maxY - $minY
"logo found at $minX,$minY  ${logoW}x${logoH}"

function Squircle([float]$x, [float]$y, [float]$w, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $w - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $w - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function DrawIcon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    # The black rounded square (small icons use a bit more of the canvas so they stay readable).
    $inset = if ($size -le 32) { $size * 0.02 } else { $size * 0.055 }
    $side = $size - 2 * $inset
    $shape = Squircle $inset $inset $side ($side * 0.225)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0, 0, 0))), $shape)
    if ($size -ge 48) {
        $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(60, 255, 255, 255)), ([float]($size / 256.0))), $shape)
    }

    # The logo, centered, about 72 % of the square's width (larger on tiny sizes).
    $fill = if ($size -le 24) { 0.86 } elseif ($size -le 48) { 0.8 } else { 0.72 }
    $k = ($side * $fill) / $logoW
    $dw = $logoW * $k; $dh = $logoH * $k
    $dx = $inset + ($side - $dw) / 2; $dy = $inset + ($side - $dh) / 2
    $g.SetClip($shape)
    $srcRect = New-Object System.Drawing.RectangleF $minX, $minY, $logoW, $logoH
    $dstRect = New-Object System.Drawing.RectangleF $dx, $dy, $dw, $dh
    $g.DrawImage($logo, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
    $g.ResetClip()
    $g.Dispose()
    return $bmp
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$frames = @()
foreach ($size in $sizes) {
    $bmp = DrawIcon $size
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames += , @{ size = $size; bytes = $ms.ToArray() }
    if ($size -eq 256) { $bmp.Save((Join-Path $PSScriptRoot 'dockx.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}
$logo.Dispose()

# ICO container with PNG frames.
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $dim = if ($f.size -ge 256) { 0 } else { $f.size }
    $w.Write([Byte]$dim); $w.Write([Byte]$dim); $w.Write([Byte]0); $w.Write([Byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$f.bytes.Length); $w.Write([UInt32]$offset)
    $offset += $f.bytes.Length
}
foreach ($f in $frames) { $w.Write($f.bytes) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'dockx.ico'), $out.ToArray())
"wrote dockx.ico ($($out.Length) bytes)"
