# Draws the DockX app icon and writes assets\dockx.ico (PNG frames, 16-256 px).
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File assets\make-icon.ps1
Add-Type -AssemblyName System.Drawing

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function DrawIcon([int]$size) {
    $s = $size / 256.0
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.Clear([System.Drawing.Color]::Transparent)

    # Background: a macOS-style rounded square with a dusk gradient.
    $bg = RoundedRect (8 * $s) (8 * $s) (240 * $s) (240 * $s) (56 * $s)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, (8 * $s)), (New-Object System.Drawing.PointF 0, (248 * $s)), ([System.Drawing.Color]::FromArgb(255, 94, 140, 255)), ([System.Drawing.Color]::FromArgb(255, 88, 40, 170))
    $blend = New-Object System.Drawing.Drawing2D.ColorBlend 3
    $blend.Colors = @([System.Drawing.Color]::FromArgb(255, 110, 170, 255), [System.Drawing.Color]::FromArgb(255, 120, 90, 230), [System.Drawing.Color]::FromArgb(255, 70, 30, 140))
    $blend.Positions = @(0.0, 0.55, 1.0)
    $grad.InterpolationColors = $blend
    $g.FillPath($grad, $bg)

    # Soft light at the top.
    $sb = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, (6 * $s)), (New-Object System.Drawing.PointF 0, (122 * $s)), ([System.Drawing.Color]::FromArgb(70, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $g.SetClip((New-Object System.Drawing.RectangleF 0, 0, $size, (120 * $s)))
    $g.FillPath($sb, $bg)
    $g.ResetClip()

    # The glass dock shelf.
    $shelf = RoundedRect (30 * $s) (160 * $s) (196 * $s) (56 * $s) (18 * $s)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(90, 255, 255, 255))), $shelf)
    $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(140, 255, 255, 255)), ([float](2.5 * $s))), $shelf)

    # App icons on the shelf; the one under the "cursor" is magnified, like the real dock.
    $apps = @(
        @{ x = 44;  y = 170; w = 36; c = [System.Drawing.Color]::FromArgb(255, 255, 159, 10) },
        @{ x = 86;  y = 152; w = 54; c = [System.Drawing.Color]::FromArgb(255, 255, 255, 255) },
        @{ x = 146; y = 162; w = 44; c = [System.Drawing.Color]::FromArgb(255, 48, 209, 88) },
        @{ x = 194; y = 174; w = 28; c = [System.Drawing.Color]::FromArgb(255, 255, 69, 120) }
    )
    foreach ($a in $apps) {
        $r = RoundedRect ($a.x * $s) ($a.y * $s) ($a.w * $s) ($a.w * $s) ($a.w * 0.24 * $s)
        $shadow = RoundedRect ($a.x * $s) (($a.y + 3) * $s) ($a.w * $s) ($a.w * $s) ($a.w * 0.24 * $s)
        $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(60, 0, 0, 0))), $shadow)
        $g.FillPath((New-Object System.Drawing.SolidBrush $a.c), $r)
    }
    # A purple glyph on the big white icon, and the running dot under it.
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 120, 90, 230))), (100 * $s), (166 * $s), (26 * $s), (26 * $s))
    $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 255, 255, 255))), (109 * $s), (209 * $s), (8 * $s), (8 * $s))

    $g.Dispose()
    return $bmp
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @()
foreach ($size in $sizes) {
    $bmp = DrawIcon $size
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $frames += , @{ size = $size; bytes = $ms.ToArray() }
    if ($size -eq 256) { $bmp.Save((Join-Path $PSScriptRoot 'dockx.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}

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
