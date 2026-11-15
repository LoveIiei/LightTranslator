# Generates res/app.ico, res/tray-light.ico and res/tray-dark.ico.
# tray-light.ico = dark glyph for a light taskbar; tray-dark.ico = white glyph for a dark taskbar.
# Usage: powershell -ExecutionPolicy Bypass -File tools\make-icons.ps1
Add-Type -AssemblyName System.Drawing

function New-RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-IconImage([int]$s, [System.Drawing.Color]$glyph, $fill) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    $stroke = [Math]::Max(1.0, $s / 12.0)
    $inset = $stroke / 2 + 0.5
    $path = New-RoundedRect $inset $inset ($s - 2 * $inset) ($s - 2 * $inset) ($s * 0.22)
    if ($fill) {
        $brush = New-Object System.Drawing.SolidBrush $fill
        $g.FillPath($brush, $path)
        $brush.Dispose()
    } else {
        $pen = New-Object System.Drawing.Pen $glyph, $stroke
        $g.DrawPath($pen, $path)
        $pen.Dispose()
    }

    $font = New-Object System.Drawing.Font 'Segoe UI', ($s * 0.56), ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = [System.Drawing.StringAlignment]::Center
    $fmt.LineAlignment = [System.Drawing.StringAlignment]::Center
    $textBrush = New-Object System.Drawing.SolidBrush $glyph
    $rect = New-Object System.Drawing.RectangleF 0, ($s * 0.02), $s, $s
    $g.DrawString('A', $font, $textBrush, $rect, $fmt)
    $textBrush.Dispose(); $font.Dispose(); $fmt.Dispose(); $path.Dispose(); $g.Dispose()

    # ICO image entry: BITMAPINFOHEADER (height doubled) + bottom-up BGRA rows + 1bpp AND mask.
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter $ms
    $maskRow = [int]([Math]::Floor(($s + 31) / 32)) * 4
    $bw.Write([int]40); $bw.Write([int]$s); $bw.Write([int]($s * 2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($s * $s * 4 + $maskRow * $s))
    $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0)
    for ($y = $s - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $s; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }
    $bw.Write((New-Object byte[] ($maskRow * $s)))
    $bw.Flush()
    $bmp.Dispose()
    return ,$ms.ToArray()
}

function Write-Icon([string]$path, [int[]]$sizes, [System.Drawing.Color]$glyph, $fill) {
    $images = @()
    foreach ($s in $sizes) { $images += ,(New-IconImage $s $glyph $fill) }
    $fs = [System.IO.File]::Create($path)
    $bw = New-Object System.IO.BinaryWriter $fs
    $bw.Write([int16]0); $bw.Write([int16]1); $bw.Write([int16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $bw.Write([byte]$sizes[$i]); $bw.Write([byte]$sizes[$i]); $bw.Write([byte]0); $bw.Write([byte]0)
        $bw.Write([int16]1); $bw.Write([int16]32)
        $bw.Write([int]$images[$i].Length); $bw.Write([int]$offset)
        $offset += $images[$i].Length
    }
    foreach ($img in $images) { $bw.Write($img) }
    $bw.Flush(); $fs.Close()
}

$res = Join-Path (Split-Path $PSScriptRoot -Parent) 'res'
New-Item -ItemType Directory -Force $res | Out-Null
$traySizes = 16, 20, 24, 32, 40, 48
Write-Icon (Join-Path $res 'tray-light.ico') $traySizes ([System.Drawing.Color]::FromArgb(255, 26, 26, 26)) $null
Write-Icon (Join-Path $res 'tray-dark.ico') $traySizes ([System.Drawing.Color]::White) $null
Write-Icon (Join-Path $res 'app.ico') (16, 24, 32, 48, 64) ([System.Drawing.Color]::White) ([System.Drawing.Color]::FromArgb(255, 0, 103, 192))
Write-Host "Icons written to $res"
