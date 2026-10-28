param([string]$OutDir = 'K:\Farkle\Documentation\Steam_Roadmap_Art')

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$bgPath    = 'K:\Farkle\Screenshots\screenshot_20260922_134810.png'
$atlasPath = 'K:\Farkle\Data\UI\Farkle\Textures\farkle-atlas.png'
$fontDir   = 'K:\Farkle\Data\UI\Farkle\Fonts'

New-Item -ItemType Directory -Force $OutDir | Out-Null

$fonts = New-Object System.Drawing.Text.PrivateFontCollection
$fonts.AddFontFile("$fontDir\Pirata_One\PirataOne-Regular.ttf")
$fonts.AddFontFile("$fontDir\IM_Fell_English_SC\IMFellEnglishSC-Regular.ttf")
$fonts.AddFontFile("$fontDir\EB_Garamond\static\EBGaramond-Italic.ttf")
function Fam($name) { $fonts.Families | Where-Object Name -eq $name | Select-Object -First 1 }
$famTitle = Fam 'Pirata One'
$famCaps  = Fam 'IM FELL English SC'
$famBody  = Fam 'EB Garamond'

$bg    = [System.Drawing.Image]::FromFile($bgPath)
$atlas = [System.Drawing.Image]::FromFile($atlasPath)
$signSrc = New-Object System.Drawing.Rectangle 533, 25, 985, 305
$fleurSrc = New-Object System.Drawing.Rectangle 1198, 780, 180, 208

$gold      = [System.Drawing.Color]::FromArgb(255, 0xE5, 0xC3, 0x8B)
$goldDim   = [System.Drawing.Color]::FromArgb(230, 0xD3, 0xB6, 0x86)
$parchment = [System.Drawing.Color]::FromArgb(240, 0xEF, 0xDD, 0xBE)

function New-Canvas($w, $h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = 'AntiAlias'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode   = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    return $bmp, $g
}

# Cover-fit the screenshot so (fx, fy) in source pixels sits at the canvas's (ax, ay) fraction.
function Draw-Background($g, $w, $h, $fx, $fy, $ax, $ay, $zoom) {
    $scale = [Math]::Max($w / $bg.Width, $h / $bg.Height) * $zoom
    $sw = $w / $scale; $sh = $h / $scale
    $sx = [Math]::Min([Math]::Max($fx - $sw * $ax, 0), $bg.Width - $sw)
    $sy = [Math]::Min([Math]::Max($fy - $sh * $ay, 0), $bg.Height - $sh)
    # Grade: darker, a touch more contrast and less orange cast.
    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix00 = 0.78; $m.Matrix11 = 0.70; $m.Matrix22 = 0.66
    $m.Matrix40 = -0.06; $m.Matrix41 = -0.06; $m.Matrix42 = -0.05
    $ia = New-Object System.Drawing.Imaging.ImageAttributes
    $ia.SetColorMatrix($m)
    $dst = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $g.DrawImage($bg, $dst, [float]$sx, [float]$sy, [float]$sw, [float]$sh, 'Pixel', $ia)
}

function Fill-Gradient($g, $x, $y, $w, $h, $a1, $a2, $angle) {
    $rect = New-Object System.Drawing.RectangleF ([float]$x), ([float]$y), ([float]$w), ([float]$h)
    $c1 = [System.Drawing.Color]::FromArgb($a1, 12, 7, 4)
    $c2 = [System.Drawing.Color]::FromArgb($a2, 12, 7, 4)
    # Brush rect is padded so GDI+ doesn't wrap the first colour back in at the far edge (visible seam).
    $brushRect = New-Object System.Drawing.RectangleF ([float]($x - 2)), ([float]($y - 2)), ([float]($w + 4)), ([float]($h + 4))
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush $brushRect, $c1, $c2, ([float]$angle)
    $br.WrapMode = 'TileFlipXY'
    $g.FillRectangle($br, $rect)
    $br.Dispose()
}

function Draw-Vignette($g, $w, $h, $strength) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddEllipse([float](-$w * 0.25), [float](-$h * 0.35), [float]($w * 1.5), [float]($h * 1.7))
    $br = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
    $br.CenterColor = [System.Drawing.Color]::FromArgb(0, 0, 0, 0)
    $br.SurroundColors = @([System.Drawing.Color]::FromArgb($strength, 6, 3, 2))
    $g.FillRectangle($br, 0, 0, $w, $h)
    $br.Dispose(); $path.Dispose()
}

# Text drawn as a path so it can carry a soft drop shadow and a dark outline.
function Draw-Text($g, $text, $fam, $size, $color, $cx, $cy, $align = 'Center', $tracking = 0, $shadow = 0.06) {
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = $align; $sf.LineAlignment = 'Center'
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    if ($tracking -eq 0) {
        $path.AddString($text, $fam, 0, [float]$size, (New-Object System.Drawing.PointF ([float]$cx), ([float]$cy)), $sf)
    } else {
        # Manual letter-spacing: lay out glyphs one by one, then shift the whole run.
        $x = 0.0; $sfl = New-Object System.Drawing.StringFormat; $sfl.LineAlignment = 'Center'
        $sfl.FormatFlags = 'MeasureTrailingSpaces'
        foreach ($ch in $text.ToCharArray()) {
            $s = [string]$ch
            if ($s -eq ' ') { $x += $size * 0.28 + $tracking; continue }
            $p = New-Object System.Drawing.Drawing2D.GraphicsPath
            $p.AddString($s, $fam, 0, [float]$size, (New-Object System.Drawing.PointF 0, 0), $sfl)
            $adv = if ($s -eq ' ') { $size * 0.28 } else { $p.GetBounds().Right }
            $mx = New-Object System.Drawing.Drawing2D.Matrix; $mx.Translate([float]$x, 0)
            $p.Transform($mx); $path.AddPath($p, $false)
            $x += $adv + $tracking
        }
        $x -= $tracking
        $ox = switch ($align) { 'Center' { $cx - $x / 2 } 'Far' { $cx - $x } default { $cx } }
        $mx = New-Object System.Drawing.Drawing2D.Matrix; $mx.Translate([float]$ox, [float]$cy)
        $path.Transform($mx)
    }
    if ($shadow -gt 0) {
        $off = $size * $shadow
        $sp = $path.Clone(); $mx = New-Object System.Drawing.Drawing2D.Matrix; $mx.Translate([float]($off * 0.6), [float]$off)
        $sp.Transform($mx)
        $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 0, 0, 0))), $sp)
    }
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 20, 12, 6)), ([float]([Math]::Max(1.0, $size * 0.035)))
    $pen.LineJoin = 'Round'
    $g.DrawPath($pen, $path)
    $g.FillPath((New-Object System.Drawing.SolidBrush $color), $path)
    $bounds = $path.GetBounds()
    $path.Dispose(); $pen.Dispose()
    return $bounds
}

# The in-game title sign with "Farkle" lettered on the board, as on the main menu.
function Draw-Sign($g, $cx, $top, $width) {
    $h = $width * $signSrc.Height / $signSrc.Width
    $x = $cx - $width / 2
    # Soft shadow under the sign.
    for ($i = 6; $i -ge 1; $i--) {
        $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(14, 0, 0, 0))
        $g.FillRectangle($br, [float]($x + $width * 0.03 - $i * 3), [float]($top + $h * 0.30 + $i * 2), [float]($width * 0.94 + $i * 6), [float]($h * 0.72 + $i * 4))
    }
    $dst = New-Object System.Drawing.RectangleF ([float]$x), ([float]$top), ([float]$width), ([float]$h)
    $g.DrawImage($atlas, $dst, $signSrc, 'Pixel')
    Draw-Text $g 'Farkle' $famTitle ($h * 0.50) $gold $cx ($top + $h * 0.635) 'Center' 0 0.035 | Out-Null
    return $h
}

function Draw-Rule($g, $cx, $cy, $halfWidth, $fleurH) {
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 0xC9, 0xA2, 0x63)), ([float]([Math]::Max(1.0, $fleurH * 0.05)))
    $gap = $fleurH * 0.75
    $g.DrawLine($pen, [float]($cx - $halfWidth), [float]$cy, [float]($cx - $gap), [float]$cy)
    $g.DrawLine($pen, [float]($cx + $gap), [float]$cy, [float]($cx + $halfWidth), [float]$cy)
    $fw = $fleurH * $fleurSrc.Width / $fleurSrc.Height
    $dst = New-Object System.Drawing.RectangleF ([float]($cx - $fw / 2)), ([float]($cy - $fleurH / 2)), ([float]$fw), ([float]$fleurH)
    $g.DrawImage($atlas, $dst, $fleurSrc, 'Pixel')
    $pen.Dispose()
}

function Save-Png($bmp, $name) {
    $p = Join-Path $OutDir $name
    $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output $p
}

$updates = 'New Engine  ·  13 Languages  ·  Multiplayer  ·  Progression  ·  More Music'

# --- Cover image 800x450 (events list, store page) ---------------------------
$w = 800; $h = 450
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2000 1250 0.55 0.55 1.0
Fill-Gradient $g 0 0 $w $h 60 150 90
Draw-Vignette $g $w $h 200
$sh = Draw-Sign $g ($w / 2) 34 500
Draw-Text $g 'The Road Ahead' $famCaps 54 $parchment ($w / 2) (34 + $sh + 46) 'Center' 2 | Out-Null
Draw-Rule $g ($w / 2) (34 + $sh + 98) 190 26
Draw-Text $g 'PLANNED UPDATES' $famCaps 22 $goldDim ($w / 2) (34 + $sh + 140) 'Center' 5 0.04 | Out-Null
Save-Png $bmp 'cover_800x450.png'
$g.Dispose(); $bmp.Dispose()

# --- Header image 1920x622 (top of the announcement) -------------------------
$w = 1920; $h = 622
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2100 1250 0.5 0.52 1.0
# Centred layout: darken the middle band evenly (two mirrored halves), let the edges breathe.
Fill-Gradient $g ($w * 0.15) 0 ($w * 0.35) $h 40 190 0
Fill-Gradient $g ($w * 0.50) 0 ($w * 0.35) $h 190 40 0
Fill-Gradient $g 0 0 ($w * 0.15) $h 40 40 0
Fill-Gradient $g ($w * 0.85) 0 ($w * 0.15) $h 40 40 0
Fill-Gradient $g 0 ($h * 0.55) $w ($h * 0.45) 0 150 90
Draw-Vignette $g $w $h 150
$cx = $w / 2
$sh = Draw-Sign $g $cx 62 740
Draw-Text $g 'The Road Ahead' $famCaps 80 $parchment $cx (62 + $sh + 68) 'Center' 3 | Out-Null
Draw-Rule $g $cx (62 + $sh + 146) 300 36
Draw-Text $g $updates $famCaps 25 $goldDim $cx (62 + $sh + 198) 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'header_1920x622.png'
$g.Dispose(); $bmp.Dispose()

# --- Library spotlight 2108x460 (Steam library banner) -----------------------
$w = 2108; $h = 460
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2700 1180 0.80 0.5 1.0
Fill-Gradient $g 0 0 ($w * 0.88) $h 230 25 0
Draw-Vignette $g $w $h 140
$sh = Draw-Sign $g 370 ((460 - 620 * 305 / 985) / 2 - 6) 620
$tx = 740
$b = Draw-Text $g 'The Road Ahead' $famCaps 88 $parchment $tx 170 'Near' 3
$ruleY = 252
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 0xC9, 0xA2, 0x63)), 2
$g.DrawLine($pen, [float]$tx, [float]$ruleY, [float]($tx + 600), [float]$ruleY); $pen.Dispose()
Draw-Text $g $updates $famCaps 25 $goldDim $tx 300 'Near' 0 0.04 | Out-Null
Save-Png $bmp 'library_spotlight_2108x460.png'
$g.Dispose(); $bmp.Dispose()

$bg.Dispose(); $atlas.Dispose()
