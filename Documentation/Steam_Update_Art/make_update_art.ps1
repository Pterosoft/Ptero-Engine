param([string]$OutDir = 'K:\Farkle\Documentation\Steam_Update_Art')

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$bgPath    = 'K:\Farkle\Screenshots\screenshot_20261022_181742.png'
$atlasPath = 'K:\Farkle\Data\UI\Farkle\Textures\farkle-atlas.png'
$fontDir   = 'K:\Farkle\Data\UI\Farkle\Fonts'

New-Item -ItemType Directory -Force $OutDir | Out-Null

$fonts = New-Object System.Drawing.Text.PrivateFontCollection
$fonts.AddFontFile("$fontDir\Pirata_One\PirataOne-Regular.ttf")
$fonts.AddFontFile("$fontDir\IM_Fell_English_SC\IMFellEnglishSC-Regular.ttf")
$fonts.AddFontFile("$fontDir\EB_Garamond\static\EBGaramond-Regular.ttf")
$fonts.AddFontFile("$fontDir\Grenze_Gotisch\static\GrenzeGotisch-SemiBold.ttf")
function Fam($name) { $fonts.Families | Where-Object Name -like $name | Select-Object -First 1 }
$famTitle = Fam 'Pirata One'
$famCaps  = Fam 'IM FELL English SC'
$famBody  = Fam 'EB Garamond'
$famGoth  = Fam 'Grenze Gotisch*'

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
    # Grade: the candlelit shot is already dark; only trim the orange cast slightly.
    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix00 = 1.04; $m.Matrix11 = 0.98; $m.Matrix22 = 0.94
    $m.Matrix40 = -0.01; $m.Matrix41 = -0.01; $m.Matrix42 = -0.01
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

# The title wordmark as on the main menu: Pirata One in warm gold, no sign board.
$wordGold = [System.Drawing.Color]::FromArgb(255, 0xE2, 0xBC, 0x7A)
# $cy is the visual centre of the glyphs; Pirata One sits high in its line box, so measure and shift.
function Draw-Wordmark($g, $cx, $cy, $size) {
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddString('Farkle', $famTitle, 0, [float]$size, (New-Object System.Drawing.PointF 0, 0), $sf)
    # Flat fill only: no drop shadow or outline.
    $b = $p.GetBounds()
    $mx = New-Object System.Drawing.Drawing2D.Matrix
    $mx.Translate([float]($cx - ($b.X + $b.Width / 2)), [float]($cy - ($b.Y + $b.Height / 2)))
    $p.Transform($mx)
    $br = New-Object System.Drawing.SolidBrush $wordGold
    $g.FillPath($br, $p)
    $br.Dispose(); $p.Dispose()
}
# Steam capsules may carry only the game's logo, so these get the centred wordmark and nothing else.
# Source focus points (3840x2160): table ~(1950, 1250), hearth ~(3400, 1050).

# --- Main capsule 1232x706 ---------------------------------------------------
$w = 1232; $h = 706
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2050 1180 0.5 0.55 1.06
Fill-Gradient $g 0 0 $w $h 95 95 90
Draw-Vignette $g $w $h 110
Draw-Wordmark $g ($w / 2) ($h / 2) 300
Save-Png $bmp 'capsule_main_1232x706.png'
$g.Dispose(); $bmp.Dispose()

# --- Header capsule 920x430 --------------------------------------------------
$w = 920; $h = 430
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2000 0 0.5 0.0 1.0
Fill-Gradient $g 0 0 $w $h 95 95 90
Draw-Vignette $g $w $h 110
Draw-Wordmark $g ($w / 2) ($h / 2) 215
Save-Png $bmp 'capsule_header_920x430.png'
$g.Dispose(); $bmp.Dispose()

# --- Small capsule 462x174 (logo has to read at a glance) ---------------------
$w = 462; $h = 174
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2100 1000 0.5 0.5 1.0
Fill-Gradient $g 0 0 $w $h 110 110 90
Draw-Vignette $g $w $h 120
Draw-Wordmark $g ($w / 2) ($h / 2) 150
Save-Png $bmp 'capsule_small_462x174.png'
$g.Dispose(); $bmp.Dispose()

# --- Vertical capsule 748x896 ------------------------------------------------
$w = 748; $h = 896
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 1965 1250 0.5 0.6 1.0
Fill-Gradient $g 0 0 $w $h 95 95 90
Draw-Vignette $g $w $h 120
Draw-Wordmark $g ($w / 2) ($h / 2) 260
Save-Png $bmp 'capsule_vertical_748x896.png'
$g.Dispose(); $bmp.Dispose()

# --- Library capsule 600x900 -------------------------------------------------
$w = 600; $h = 900
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 1965 1250 0.5 0.6 1.0
Fill-Gradient $g 0 0 $w $h 95 95 90
Draw-Vignette $g $w $h 120
Draw-Wordmark $g ($w / 2) ($h / 2) 215
Save-Png $bmp 'capsule_library_600x900.png'
$g.Dispose(); $bmp.Dispose()

# --- Bluesky announcement 2000x1125 (16:9 shows uncropped in the feed) -------
$w = 2000; $h = 1125
$bmp, $g = New-Canvas $w $h
# Centred layout: title block over the ceiling and back wall, features and link over the dark
# floor, leaving the table and hearth visible in the middle band.
Draw-Background $g $w $h 1960 1150 0.5 0.53 1.0
Fill-Gradient $g 0 0 $w ($h * 0.50) 235 0 90
Fill-Gradient $g 0 ($h * 0.62) $w ($h * 0.38) 0 240 90
Draw-Vignette $g $w $h 120
$cx = $w / 2
$goldBrush = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xB0, 0x6C)

function Measure-Text($text, $fam, $size) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddString($text, $fam, 0, [float]$size, (New-Object System.Drawing.PointF 0, 0), [System.Drawing.StringFormat]::GenericTypographic)
    $b = $p.GetBounds(); $p.Dispose()
    return $b
}
function Draw-Diamond($g, $x, $y, $d, $color) {
    $pts = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ([float]$x), ([float]($y - $d))),
        (New-Object System.Drawing.PointF ([float]($x + $d)), ([float]$y)),
        (New-Object System.Drawing.PointF ([float]$x), ([float]($y + $d))),
        (New-Object System.Drawing.PointF ([float]($x - $d)), ([float]$y)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush $color), $pts)
}
# A row of items separated by gold diamonds, centred as a whole on $cx.
function Draw-FeatureRow($g, $items, $cy, $size, $gap) {
    $widths = foreach ($it in $items) { (Measure-Text $it $famBody $size).Width }
    $total = ($widths | Measure-Object -Sum).Sum + $gap * ($items.Count - 1)
    $x = $cx - $total / 2
    for ($i = 0; $i -lt $items.Count; $i++) {
        Draw-Text $g $items[$i] $famBody $size $parchment ($x + $widths[$i] / 2) $cy 'Center' 0 0.04 | Out-Null
        $x += $widths[$i]
        if ($i -lt $items.Count - 1) { Draw-Diamond $g ($x + $gap / 2) ($cy + 2) 7 $goldBrush; $x += $gap }
    }
}

Draw-Text $g 'Farkle' $famTitle 215 $goldBrush $cx 150 'Center' 0 0.03 | Out-Null
Draw-Text $g 'NEW UPDATE' $famCaps 34 $goldDim $cx 290 'Center' 9 0.04 | Out-Null
Draw-Text $g 'A New Engine Foundation' $famGoth 100 $parchment $cx 375 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 452 240 32

Draw-FeatureRow $g @('Virtualized Geometry', 'Virtual Shadow Maps', 'Path Tracing') 868 40 64
Draw-FeatureRow $g @('Better Global Illumination', 'Bloom & Lens Flares', 'Steam Achievements') 924 40 64

# Outlined call-to-action box, like a button.
$label = 'Learn more at farkle.pterosoftstudio.com'
$bw = (Measure-Text $label $famBody 38).Width + 80; $bh = 80; $by = 990
Draw-Text $g $label $famBody 38 $parchment $cx ($by + $bh / 2 - 2) 'Center' 0 0.04 | Out-Null
$pen = New-Object System.Drawing.Pen $goldBrush, 2.5
$g.DrawRectangle($pen, [float]($cx - $bw / 2), [float]$by, [float]$bw, [float]$bh); $pen.Dispose()
Save-Png $bmp 'bluesky_update_2000x1125.png'
$g.Dispose(); $bmp.Dispose()


# --- Bluesky before/after 2000x1125 ------------------------------------------
# Two 16:9 panels side by side over a blurred, darkened copy of the new scene.
$before = [System.Drawing.Image]::FromFile((Join-Path $OutDir 'before_tavern_1920x1080.png'))
$w = 2000; $h = 1125
$bmp, $g = New-Canvas $w $h
# Cheap blur: shrink the shot to a thumbnail, then stretch it back up.
$thumb = New-Object System.Drawing.Bitmap 80, 45
$tg = [System.Drawing.Graphics]::FromImage($thumb); $tg.InterpolationMode = 'HighQualityBicubic'
$tg.DrawImage($bg, 0, 0, 80, 45); $tg.Dispose()
$g.DrawImage($thumb, -20, -20, $w + 40, $h + 40); $thumb.Dispose()
$br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200, 12, 7, 4))
$g.FillRectangle($br, 0, 0, $w, $h); $br.Dispose()
Draw-Vignette $g $w $h 160

$cx = $w / 2
$goldBrush = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xB0, 0x6C)
Draw-Text $g 'Farkle' $famTitle 150 $goldBrush $cx 105 'Center' 0 0.03 | Out-Null
Draw-Text $g 'THE TAVERN, REBUILT' $famCaps 32 $goldDim $cx 215 'Center' 9 0.04 | Out-Null

$pw = 960; $ph = 540; $gap = 28; $py = 268
$px1 = ($w - 2 * $pw - $gap) / 2; $px2 = $px1 + $pw + $gap
function Draw-Panel($img, $x, $y, $srcRect) {
    for ($i = 5; $i -ge 1; $i--) {
        $sb = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(22, 0, 0, 0))
        $g.FillRectangle($sb, [float]($x - $i * 2), [float]($y - $i + 4), [float]($pw + $i * 4), [float]($ph + $i * 3)); $sb.Dispose()
    }
    $dst = New-Object System.Drawing.RectangleF ([float]$x), ([float]$y), ([float]$pw), ([float]$ph)
    $g.DrawImage($img, $dst, $srcRect, 'Pixel')
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 0xC9, 0xA2, 0x63)), 2
    $g.DrawRectangle($pen, [float]$x, [float]$y, [float]$pw, [float]$ph); $pen.Dispose()
}
Draw-Panel $before $px1 $py (New-Object System.Drawing.RectangleF 0, 0, $before.Width, $before.Height)
Draw-Panel $bg $px2 $py (New-Object System.Drawing.RectangleF 0, 0, $bg.Width, $bg.Height)

$ly = $py + $ph + 52
Draw-Text $g 'BEFORE' $famCaps 34 $goldDim ($px1 + $pw / 2) $ly 'Center' 9 0.04 | Out-Null
Draw-Text $g 'Farkle 1.0' $famBody 44 $parchment ($px1 + $pw / 2) ($ly + 52) 'Center' 0 0.04 | Out-Null
Draw-Text $g 'AFTER' $famCaps 34 $goldDim ($px2 + $pw / 2) $ly 'Center' 9 0.04 | Out-Null
Draw-Text $g 'A New Engine Foundation' $famBody 44 $parchment ($px2 + $pw / 2) ($ly + 52) 'Center' 0 0.04 | Out-Null

Draw-Text $g 'Learn more at farkle.pterosoftstudio.com' $famBody 34 $gold $cx ($h - 52) 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'bluesky_before_after_2000x1125.png'
$g.Dispose(); $bmp.Dispose(); $before.Dispose()

$bg.Dispose(); $atlas.Dispose()
