# BunkerVR Setup's icon: a dark rounded square with a white serif B, lettered like the studio's start card, and a
# thin warm line under it. Each size is drawn separately rather than shrunk from one picture, and stored as a PNG
# inside the .ico.
param([string]$Out = "$PSScriptRoot\stage\setup.ico")
Add-Type -AssemblyName System.Drawing
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$pngs = @()
foreach ($s in $sizes) {
    $b = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)
    # The rounded square.
    $inset = [Math]::Max(0.5, $s * 0.03)
    $r = $s * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = $s - 2 * $inset
    $path.AddArc($inset, $inset, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($inset + $w - 2 * $r, $inset, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($inset + $w - 2 * $r, $inset + $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($inset, $inset + $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $top = [System.Drawing.Color]::FromArgb(255, 34, 34, 38)
    $bottom = [System.Drawing.Color]::FromArgb(255, 14, 14, 16)
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 0, $s), $top, $bottom
    $g.FillPath($fill, $path)
    if ($s -ge 32) {
        $edge = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 58, 58, 64)), ([Math]::Max(1.0, $s / 128.0))
        $g.DrawPath($edge, $path)
    }
    # The B, in the studio card's serif, sized by its ink (the glyph's drawn outline, not the font size).
    $face = 'Bodoni MT'
    $family = New-Object System.Drawing.FontFamily $face
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $glyph = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glyph.AddString('B', $family, 0, 100, (New-Object System.Drawing.PointF 0, 0), $fmt)
    $gb = $glyph.GetBounds()
    $wantH = $s * $(if ($s -le 20) { 0.62 } else { 0.56 })
    $scale = $wantH / $gb.Height
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Scale($scale, $scale)
    $glyph.Transform($m)
    $gb = $glyph.GetBounds()
    $lineGap = $(if ($s -ge 32) { $s * 0.06 } else { 0 })
    $lineH = $(if ($s -ge 32) { [Math]::Max(1.0, $s * 0.025) } else { 0 })
    $blockH = $gb.Height + $lineGap + $lineH
    $dx = ($s - $gb.Width) / 2 - $gb.X
    $dy = ($s - $blockH) / 2 - $gb.Y
    $m2 = New-Object System.Drawing.Drawing2D.Matrix
    $m2.Translate($dx, $dy)
    $glyph.Transform($m2)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 240, 240, 242))), $glyph)
    if ($s -ge 32) {
        $gb = $glyph.GetBounds()
        $lineW = $gb.Width * 0.9
        $warm = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 205, 182, 140))
        $g.FillRectangle($warm, [single](($s - $lineW) / 2), [single]($gb.Bottom + $lineGap), [single]$lineW, [single]$lineH)
    }
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
    $pngs += ,($ms.ToArray())
}
# The .ico: its header, one entry per size, then the PNGs.
$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $bw.Write([byte]$(if ($s -ge 256) { 0 } else { $s })); $bw.Write([byte]$(if ($s -ge 256) { 0 } else { $s }))
    $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Close()
"made $Out"
