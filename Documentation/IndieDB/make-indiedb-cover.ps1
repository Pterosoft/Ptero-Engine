# Renders the IndieDB featured image (1600x1200 JPEG, 4:3) for the release article.
# Adapted from Website/make-news-cover.ps1: same sign, fonts and fleur rule, English only.
# Background is the refreshed tavern shot used on the website hero.
# -Giveaway renders the IndieDB giveaway logo instead (same art, giveaway headline).
param([switch]$Giveaway)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$bgPath    = 'K:\Farkle\Screenshots\screenshot_20261022_181742.png'
$atlasPath = 'K:\Farkle\Data\UI\Farkle\Textures\farkle-atlas.png'
$uiFonts   = 'K:\Farkle\Data\UI\Farkle\Fonts'
$outDir    = $PSScriptRoot

$fonts = New-Object System.Drawing.Text.PrivateFontCollection
$fonts.AddFontFile("$uiFonts\Pirata_One\PirataOne-Regular.ttf")
$fonts.AddFontFile("$uiFonts\IM_Fell_English_SC\IMFellEnglishSC-Regular.ttf")
function Get-Family($name) {
    $f = $fonts.Families | Where-Object Name -eq $name | Select-Object -First 1
    if ($f) { return $f }
    return New-Object System.Drawing.FontFamily $name
}

# IM Fell English SC covers every Latin-script locale; the rest use installed fonts.
$scriptFonts = @{
    'latin' = @{ Family = 'IM FELL English SC'; Style = 0; Scale = 1.0;  Tracking = 3; Upper = $true }
    'uk'    = @{ Family = 'Palatino Linotype';  Style = 1; Scale = 0.9;  Tracking = 1 }
    'el'    = @{ Family = 'Palatino Linotype';  Style = 1; Scale = 0.9;  Tracking = 1 }
    'ar'    = @{ Family = 'Times New Roman';    Style = 1; Scale = 1.0;  Tracking = 0; Rtl = $true }
    'zh-cn' = @{ Family = 'KaiTi';              Style = 0; Scale = 0.95; Tracking = 0; Embolden = 0.04 }
    'ja'    = @{ Family = 'Yu Gothic';          Style = 1; Scale = 0.85; Tracking = 0 }
    'ko'    = @{ Family = 'Malgun Gothic';      Style = 1; Scale = 0.85; Tracking = 0 }
}

$bg    = [System.Drawing.Image]::FromFile($bgPath)
$atlas = [System.Drawing.Image]::FromFile($atlasPath)
$signSrc  = New-Object System.Drawing.Rectangle 533, 25, 985, 305
$fleurSrc = New-Object System.Drawing.Rectangle 1198, 780, 180, 208
$gold      = [System.Drawing.Color]::FromArgb(255, 0xE5, 0xC3, 0x8B)
$goldDim   = [System.Drawing.Color]::FromArgb(235, 0xD3, 0xB6, 0x86)
$parchment = [System.Drawing.Color]::FromArgb(245, 0xEF, 0xDD, 0xBE)
$W = 1600; $H = 1200

function New-TextPath($text, $family, $style, $size, $tracking, $rtl) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $sf = [System.Drawing.StringFormat]::GenericTypographic.Clone()
    if ($rtl) { $sf.FormatFlags = $sf.FormatFlags -bor [System.Drawing.StringFormatFlags]::DirectionRightToLeft }
    if ($tracking -eq 0) {
        $path.AddString($text, $family, $style, [float]$size, (New-Object System.Drawing.PointF 0, 0), $sf)
        return $path
    }
    # Manual letter-spacing for the small-caps title.
    $x = 0.0
    foreach ($ch in $text.ToCharArray()) {
        if ($ch -eq ' ') { $x += $size * 0.28 + $tracking; continue }
        $p = New-Object System.Drawing.Drawing2D.GraphicsPath
        $p.AddString([string]$ch, $family, $style, [float]$size, (New-Object System.Drawing.PointF 0, 0), $sf)
        $b = $p.GetBounds()
        $m = New-Object System.Drawing.Drawing2D.Matrix; $m.Translate([float]$x, 0); $p.Transform($m)
        $path.AddPath($p, $false)
        $x += $b.Right + $tracking
    }
    return $path
}

# Centre the path on (cx, cy), shrink it to maxWidth, then fill with shadow and outline.
function Draw-Centered($g, $path, $cx, $cy, $maxWidth, $color, $size, $embolden) {
    $b = $path.GetBounds()
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate([float](-$b.Left - $b.Width / 2), [float](-$b.Top - $b.Height / 2)); $path.Transform($m)
    if ($b.Width -gt $maxWidth) { $m = New-Object System.Drawing.Drawing2D.Matrix; $s = $maxWidth / $b.Width; $m.Scale([float]$s, [float]$s); $path.Transform($m) }
    $m = New-Object System.Drawing.Drawing2D.Matrix; $m.Translate([float]$cx, [float]$cy); $path.Transform($m)
    $shadow = $path.Clone(); $m = New-Object System.Drawing.Drawing2D.Matrix; $m.Translate([float]($size * 0.035), [float]($size * 0.06)); $shadow.Transform($m)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 0, 0, 0))), $shadow)
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 20, 12, 6)), ([float]([Math]::Max(1.5, $size * 0.035))); $pen.LineJoin = 'Round'
    $g.DrawPath($pen, $path)
    $g.FillPath((New-Object System.Drawing.SolidBrush $color), $path)
    if ($embolden) { $p2 = New-Object System.Drawing.Pen $color, ([float]($size * $embolden)); $p2.LineJoin = 'Round'; $g.DrawPath($p2, $path) }
}

function Fill-Gradient($g, $x, $y, $w, $h, $a1, $a2, $angle) {
    $rect = New-Object System.Drawing.RectangleF ([float]$x), ([float]$y), ([float]$w), ([float]$h)
    $pad  = New-Object System.Drawing.RectangleF ([float]($x - 2)), ([float]($y - 2)), ([float]($w + 4)), ([float]($h + 4))
    $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush $pad, ([System.Drawing.Color]::FromArgb($a1, 12, 7, 4)), ([System.Drawing.Color]::FromArgb($a2, 12, 7, 4)), ([float]$angle)
    $br.WrapMode = 'TileFlipXY'
    $g.FillRectangle($br, $rect); $br.Dispose()
}
$style = $scriptFonts.latin
$family = Get-Family $style.Family
$bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'

# Background: 4:3 crop holding the table and the hearth, darkened at top and bottom for text.
$sh = $bg.Height; $sw = $sh * $W / $H; $sx = [Math]::Min(760, $bg.Width - $sw)
$cm = New-Object System.Drawing.Imaging.ColorMatrix; $cm.Matrix00 = 0.85; $cm.Matrix11 = 0.78; $cm.Matrix22 = 0.74
$ia = New-Object System.Drawing.Imaging.ImageAttributes; $ia.SetColorMatrix($cm)
$g.DrawImage($bg, (New-Object System.Drawing.Rectangle 0, 0, $W, $H), [float]$sx, [float]0, [float]$sw, [float]$sh, 'Pixel', $ia)
Fill-Gradient $g 0 0 $W 620 215 0 90
Fill-Gradient $g 0 860 $W 340 0 225 90
$vp = New-Object System.Drawing.Drawing2D.GraphicsPath; $vp.AddEllipse([float](-$W * 0.3), [float](-$H * 0.3), [float]($W * 1.6), [float]($H * 1.6))
$vb = New-Object System.Drawing.Drawing2D.PathGradientBrush $vp; $vb.CenterColor = [System.Drawing.Color]::FromArgb(0, 0, 0, 0); $vb.SurroundColors = @([System.Drawing.Color]::FromArgb(190, 6, 3, 2))
$g.FillRectangle($vb, 0, 0, $W, $H)

# Sign, title, fleur rule, subtitle.
$signW = 900; $signH = $signW * $signSrc.Height / $signSrc.Width; $top = 56
$g.DrawImage($atlas, (New-Object System.Drawing.RectangleF ([float](($W - $signW) / 2)), ([float]$top), ([float]$signW), ([float]$signH)), $signSrc, 'Pixel')
$farkle = New-TextPath 'Farkle' (Get-Family 'Pirata One') 0 ($signH * 0.5) 0 $false
Draw-Centered $g $farkle ($W / 2) ($top + $signH * 0.63) 820 $gold ($signH * 0.5) 0

$titleSize = 96
$headline = if ($Giveaway) { 'STEAM KEY GIVEAWAY' } else { 'OUT NOW ON STEAM' }
$titlePath = New-TextPath $headline $family 0 $titleSize 6 $false
Draw-Centered $g $titlePath ($W / 2) ($top + $signH + 92) 1400 $parchment $titleSize 0

$ruleY = $top + $signH + 180; $half = 380; $fh = 50; $gap = $fh * 0.75
$pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(150, 0xC9, 0xA2, 0x63)), 2.5
$g.DrawLine($pen, [float]($W / 2 - $half), [float]$ruleY, [float]($W / 2 - $gap), [float]$ruleY)
$g.DrawLine($pen, [float]($W / 2 + $gap), [float]$ruleY, [float]($W / 2 + $half), [float]$ruleY)
$fw = $fh * $fleurSrc.Width / $fleurSrc.Height
$g.DrawImage($atlas, (New-Object System.Drawing.RectangleF ([float]($W / 2 - $fw / 2)), ([float]($ruleY - $fh / 2)), ([float]$fw), ([float]$fh)), $fleurSrc, 'Pixel')

$subline = if ($Giveaway) { 'TAVERN DICE  ·  WIN A FREE COPY' } else { 'TAVERN DICE  ·  MADE IN PTERO-ENGINE' }
$subPath = New-TextPath $subline $family 0 40 5 $false
Draw-Centered $g $subPath ($W / 2) ($ruleY + 70) 1200 $goldDim 40 0

# Three highlights along the floor: release, engine update, localization.
$cols = @(@('New Engine', 'VG · VSM · Path Tracing'), @('48 Achievements', 'On Steam'), @('16 Languages', 'Translated UI'))
$colW = $W / 3; $rowY = 1035
for ($i = 0; $i -lt 3; $i++) {
    $cx = $colW * $i + $colW / 2
    $big = New-TextPath $cols[$i][0].ToUpper() $family 0 50 3 $false
    Draw-Centered $g $big $cx $rowY ($colW - 70) $gold 50 0
    $small = New-TextPath $cols[$i][1].ToUpper() $family 0 30 3 $false
    Draw-Centered $g $small $cx ($rowY + 58) ($colW - 60) $parchment 30 0
    if ($i -gt 0) { $g.DrawLine($pen, [float]($colW * $i), [float]($rowY - 50), [float]($colW * $i), [float]($rowY + 95)) }
}

$g.Dispose()
$codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
$ep = New-Object System.Drawing.Imaging.EncoderParameters 1
$ep.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 90L
$name = if ($Giveaway) { 'farkle-indiedb-giveaway-1600x1200.jpg' } else { 'farkle-indiedb-featured-1600x1200.jpg' }
$out = Join-Path $outDir $name
$bmp.Save($out, $codec, $ep); $bmp.Dispose()
$bg.Dispose(); $atlas.Dispose()
Write-Output $out
