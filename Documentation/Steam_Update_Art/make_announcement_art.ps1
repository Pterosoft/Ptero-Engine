param([string]$OutDir = 'K:\Farkle\Documentation\Steam_Update_Art\Announcement')

# Artwork for the Steam "Major Update" event (text in Steam_Announcement_Update.txt).

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$bgPath    = 'K:\Farkle\Screenshots\screenshot_20261022_181742.png'
$atlasPath = 'K:\Farkle\Data\UI\Farkle\Textures\farkle-atlas.png'
$fontDir   = 'K:\Farkle\Data\UI\Farkle\Fonts'
$iconDir   = 'K:\Farkle\Steamworks\Achievements\Icons\achieved'

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
$fleurSrc = New-Object System.Drawing.Rectangle 1198, 780, 180, 208

$gold      = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xB0, 0x6C)
$goldDim   = [System.Drawing.Color]::FromArgb(230, 0xD3, 0xB6, 0x86)
$parchment = [System.Drawing.Color]::FromArgb(240, 0xEF, 0xDD, 0xBE)
$frameGold = [System.Drawing.Color]::FromArgb(200, 0xC9, 0xA2, 0x63)

$updateTitle = 'A New Engine Foundation'
$features = 'Virtualized Geometry  ·  Virtual Shadow Maps  ·  Ptero Reconstruction  ·  48 Achievements'

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

# Blurred, darkened copy of the scene, for panels that carry their own content.
function Draw-BlurredBackground($g, $w, $h, $alpha) {
    # Cheap blur: shrink the shot to a thumbnail, then stretch it back up.
    $thumb = New-Object System.Drawing.Bitmap 80, 45
    $tg = [System.Drawing.Graphics]::FromImage($thumb); $tg.InterpolationMode = 'HighQualityBicubic'
    $tg.DrawImage($bg, 0, 0, 80, 45); $tg.Dispose()
    $g.DrawImage($thumb, -20, -20, $w + 40, $h + 40); $thumb.Dispose()
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($alpha, 12, 7, 4))
    $g.FillRectangle($br, 0, 0, $w, $h); $br.Dispose()
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
            $adv = $p.GetBounds().Right
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

# The title wordmark as on the main menu: Pirata One in warm gold.
# $cy is the visual centre of the glyphs; Pirata One sits high in its line box, so measure and shift.
function Draw-Wordmark($g, $cx, $cy, $size, $align = 'Center') {
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = 'Center'; $sf.LineAlignment = 'Center'
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddString('Farkle', $famTitle, 0, [float]$size, (New-Object System.Drawing.PointF 0, 0), $sf)
    $b = $p.GetBounds()
    $ox = if ($align -eq 'Near') { $cx - $b.X } else { $cx - ($b.X + $b.Width / 2) }
    $mx = New-Object System.Drawing.Drawing2D.Matrix
    $mx.Translate([float]$ox, [float]($cy - ($b.Y + $b.Height / 2)))
    $p.Transform($mx)
    $sp = $p.Clone(); $mx = New-Object System.Drawing.Drawing2D.Matrix; $mx.Translate([float]($size * 0.018), [float]($size * 0.03))
    $sp.Transform($mx)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(160, 0, 0, 0))), $sp)
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xE2, 0xBC, 0x7A))), $p)
    $bounds = $p.GetBounds()
    $p.Dispose(); $sp.Dispose()
    return $bounds
}

function Draw-Frame($g, $x, $y, $w, $h) {
    for ($i = 5; $i -ge 1; $i--) {
        $sb = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(22, 0, 0, 0))
        $g.FillRectangle($sb, [float]($x - $i * 2), [float]($y - $i + 4), [float]($w + $i * 4), [float]($h + $i * 3)); $sb.Dispose()
    }
}

function Save-Png($bmp, $name, $dir = $OutDir) {
    $p = Join-Path $dir $name
    $bmp.Save($p, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output $p
}

# Source focus points (3840x2160): table ~(1950, 1250), hearth ~(3400, 1050).

# --- Cover image 800x450 (events list, store page) ---------------------------
$w = 800; $h = 450
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2000 1150 0.5 0.55 1.0
Fill-Gradient $g 0 0 $w $h 120 175 90
Draw-Vignette $g $w $h 170
$cx = $w / 2
Draw-Wordmark $g $cx 130 150 | Out-Null
Draw-Text $g 'NEW UPDATE' $famCaps 22 $goldDim $cx 238 'Center' 6 0.04 | Out-Null
Draw-Text $g $updateTitle $famGoth 52 $parchment $cx 292 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 346 180 24
Draw-Text $g 'Ptero Reconstruction  ·  48 Achievements' $famBody 24 $parchment $cx 392 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'cover_800x450.png'
$g.Dispose(); $bmp.Dispose()

# --- Header image 1920x622 (top of the announcement) -------------------------
$w = 1920; $h = 622
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2050 1100 0.5 0.5 1.0
# Centred layout: darken the middle band evenly (two mirrored halves), let the edges breathe.
Fill-Gradient $g ($w * 0.15) 0 ($w * 0.35) $h 60 200 0
Fill-Gradient $g ($w * 0.50) 0 ($w * 0.35) $h 200 60 0
Fill-Gradient $g 0 0 ($w * 0.15) $h 60 60 0
Fill-Gradient $g ($w * 0.85) 0 ($w * 0.15) $h 60 60 0
Draw-Vignette $g $w $h 150
$cx = $w / 2
Draw-Wordmark $g $cx 140 200 | Out-Null
Draw-Text $g 'NEW UPDATE' $famCaps 30 $goldDim $cx 285 'Center' 9 0.04 | Out-Null
Draw-Text $g $updateTitle $famGoth 92 $parchment $cx 365 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 448 300 34
Draw-Text $g $features $famCaps 27 $goldDim $cx 505 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'header_1920x622.png'
$g.Dispose(); $bmp.Dispose()

# --- Library spotlight 2108x460 (Major Update events) ------------------------
$w = 2108; $h = 460
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2700 1150 0.80 0.5 1.0
Fill-Gradient $g 0 0 $w $h 235 70 0
Draw-Vignette $g $w $h 140
$wb = Draw-Wordmark $g 110 ($h / 2) 230 'Near'
$tx = $wb.Right + 90
$pen = New-Object System.Drawing.Pen $frameGold, 2
$g.DrawLine($pen, [float]($tx - 45), [float]110, [float]($tx - 45), [float]($h - 110)); $pen.Dispose()
Draw-Text $g 'NEW UPDATE' $famCaps 28 $goldDim $tx 150 'Near' 8 0.04 | Out-Null
Draw-Text $g $updateTitle $famGoth 88 $parchment $tx 228 'Near' 0 0.04 | Out-Null
Draw-Text $g $features $famCaps 25 $goldDim $tx 310 'Near' 0 0.04 | Out-Null
Save-Png $bmp 'library_spotlight_2108x460.png'
$g.Dispose(); $bmp.Dispose()


# --- Ptero Reconstruction explainer 1920x1080 --------------------------------
# An illustration, not a capture: one patch of output pixels where a thin dark railing and
# a dark beam cross a lit plaster wall, upscaled 2x from half-resolution render samples.
# Left: what a colour-only upscaler can do with those samples (bilinear between them).
# Right: Ptero-SR, which knows the triangle edges and shades each output pixel by its
# exact coverage.
$w = 1920; $h = 1080
$bmp, $g = New-Canvas $w $h
Draw-BlurredBackground $g $w $h 215
Draw-Vignette $g $w $h 160
$cx = $w / 2
Draw-Text $g 'PTERO RECONSTRUCTION' $famCaps 40 $goldDim $cx 92 'Center' 10 0.04 | Out-Null
Draw-Text $g 'Edges rebuilt from the geometry itself' $famGoth 76 $parchment $cx 172 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 242 260 30

$wall = @(0xE8, 0xC9, 0x96); $wood = @(0x2A, 0x18, 0x0E)
# Scene in output-pixel units: a thin railing at a shallow angle and a beam's edge.
function Scene-IsDark([double]$x, [double]$y) {
    $d = [Math]::Abs(($y - (2.6 + 0.30 * $x)) * 0.958)
    if ($d -lt 0.55) { return $true }
    return $y -gt (12.6 - 0.42 * $x)
}
function Mix($t) {
    $c = for ($k = 0; $k -lt 3; $k++) { [int]($wall[$k] + ($wood[$k] - $wall[$k]) * $t) }
    return [System.Drawing.Color]::FromArgb(255, $c[0], $c[1], $c[2])
}
# Render samples at half resolution: the centres of 2x2 output blocks, point sampled.
$cols = 18; $rows = 12
$sCols = $cols / 2; $sRows = $rows / 2
$samples = New-Object 'double[,]' $sCols, $sRows
for ($j = 0; $j -lt $sRows; $j++) { for ($i = 0; $i -lt $sCols; $i++) {
    $samples[$i, $j] = if (Scene-IsDark (2 * $i + 1) (2 * $j + 1)) { 1.0 } else { 0.0 }
} }
# One panel of the diagram: $exact draws Ptero-SR's coverage, otherwise the bilinear result.
function Draw-ReconPanel($g, $px, $py, $cell, $exact) {
$pw = $cols * $cell; $ph = $rows * $cell
for ($r = 0; $r -lt $rows; $r++) { for ($c = 0; $c -lt $cols; $c++) {
    if (-not $exact) {
    # Left: bilinear between the four nearest render samples (clamped at the borders).
    $sx = [Math]::Min([Math]::Max(($c + 0.5 - 1) / 2.0, 0), $sCols - 1)
    $sy = [Math]::Min([Math]::Max(($r + 0.5 - 1) / 2.0, 0), $sRows - 1)
    $i0 = [Math]::Floor($sx); $j0 = [Math]::Floor($sy); $fx = $sx - $i0; $fy = $sy - $j0
    $i1 = [Math]::Min($i0 + 1, $sCols - 1); $j1 = [Math]::Min($j0 + 1, $sRows - 1)
    $v = ($samples[$i0, $j0] * (1 - $fx) + $samples[$i1, $j0] * $fx) * (1 - $fy) + ($samples[$i0, $j1] * (1 - $fx) + $samples[$i1, $j1] * $fx) * $fy
    $br = New-Object System.Drawing.SolidBrush (Mix $v)
    $g.FillRectangle($br, [float]($px + $c * $cell), [float]($py + $r * $cell), [float]$cell, [float]$cell); $br.Dispose()
    continue }
    # Right: exact coverage, integrated with 12x12 samples per output pixel.
    $n = 12; $hits = 0
    for ($b = 0; $b -lt $n; $b++) { for ($a = 0; $a -lt $n; $a++) {
        if (Scene-IsDark ($c + ($a + 0.5) / $n) ($r + ($b + 0.5) / $n)) { $hits++ }
    } }
    $br = New-Object System.Drawing.SolidBrush (Mix ($hits / ($n * $n)))
    $g.FillRectangle($br, [float]($px + $c * $cell), [float]($py + $r * $cell), [float]$cell, [float]$cell); $br.Dispose()
} }
    # Pixel grid and the true edges.
    $gridPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(45, 0, 0, 0)), 1
    $edgePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(190, 0xF2, 0xC2, 0x6A)), ([float]([Math]::Max(1.5, $cell / 21)))
    $edgePen.DashStyle = 'Dash'
    for ($c = 1; $c -lt $cols; $c++) { $g.DrawLine($gridPen, [float]($px + $c * $cell), [float]$py, [float]($px + $c * $cell), [float]($py + $ph)) }
    for ($r = 1; $r -lt $rows; $r++) { $g.DrawLine($gridPen, [float]$px, [float]($py + $r * $cell), [float]($px + $pw), [float]($py + $r * $cell)) }
    $g.SetClip((New-Object System.Drawing.RectangleF ([float]$px), ([float]$py), ([float]$pw), ([float]$ph)))
    $off = 0.55 / 0.958
    foreach ($o in @(-$off, $off)) {
        $g.DrawLine($edgePen, [float]$px, [float]($py + (2.6 + $o) * $cell), [float]($px + $pw), [float]($py + (2.6 + $o + 0.30 * $cols) * $cell))
    }
    $g.DrawLine($edgePen, [float]$px, [float]($py + 12.6 * $cell), [float]($px + $pw), [float]($py + (12.6 - 0.42 * $cols) * $cell))
    $g.ResetClip()
    # Render-sample positions: one dot per 2x2 block of output pixels.
    for ($j = 0; $j -lt $sRows; $j++) { for ($i = 0; $i -lt $sCols; $i++) {
        $dx = $px + (2 * $i + 1) * $cell; $dy = $py + (2 * $j + 1) * $cell
        $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 0x6E, 0x9C, 0xC8))), [float]($dx - 3), [float]($dy - 3), 6, 6)
    } }
    $pen = New-Object System.Drawing.Pen $frameGold, 2
    $g.DrawRectangle($pen, [float]$px, [float]$py, [float]$pw, [float]$ph); $pen.Dispose()
    $gridPen.Dispose(); $edgePen.Dispose()
}
$cell = 42; $pw = $cols * $cell; $ph = $rows * $cell
$gap = 90; $py = 300
$px1 = ($w - 2 * $pw - $gap) / 2; $px2 = $px1 + $pw + $gap
Draw-ReconPanel $g $px1 $py $cell $false
Draw-ReconPanel $g $px2 $py $cell $true

$ly = $py + $ph + 50
Draw-Text $g 'COLOUR-ONLY UPSCALING' $famCaps 30 $goldDim ($px1 + $pw / 2) $ly 'Center' 6 0.04 | Out-Null
Draw-Text $g 'Blends between render samples: edges smear, thin railings break up' $famBody 30 $parchment ($px1 + $pw / 2) ($ly + 48) 'Center' 0 0.04 | Out-Null
Draw-Text $g 'PTERO-SR' $famCaps 30 $goldDim ($px2 + $pw / 2) $ly 'Center' 6 0.04 | Out-Null
Draw-Text $g 'Knows each triangle edge: every pixel gets its exact coverage' $famBody 30 $parchment ($px2 + $pw / 2) ($ly + 48) 'Center' 0 0.04 | Out-Null
Draw-Text $g 'Illustration  ·  same half-resolution input, 2x output  ·  dots mark render samples, dashes the true edges' $famBody 24 $goldDim $cx ($h - 48) 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'ptero_reconstruction_1920x1080.png'
$g.Dispose(); $bmp.Dispose()


# --- Achievements showcase 1920x1080 -----------------------------------------
$showcase = @(
    @('ACH_FIRST_VICTORY', 'First Victory'), @('ACH_STRAIGHT_SHOOTER', 'Straight Shooter'), @('ACH_HOT_DICE', 'Hot Hands'),
    @('ACH_SIX_OF_A_KIND', 'The Full Hand'), @('ACH_FIRST_FARKLE', 'Farkle!'), @('ACH_HEARTBREAKER', 'Heartbreaker'),
    @('ACH_COMEBACK', 'Comeback Kid'), @('ACH_BANK_3000', 'Greed Is Good'), @('ACH_STREAK_10', 'Unstoppable'),
    @('ACH_MATCHES_100', 'Tavern Legend'), @('ACH_SPEED_RUN', 'Speed Run'), @('ACH_INDECISIVE', 'Indecisive'),
    @('ACH_FOLLOW_THE_LIGHT', 'Follow the Light'), @('ACH_IN_HOUSE', 'In-House'), @('ACH_POTATO_MODE', 'Potato Mode'),
    @('ACH_MAXED_OUT', 'Maxed Out'), @('ACH_PURIST', 'Purist'), @('ACH_COLLECTOR', 'Collector'))
$w = 1920; $h = 1080
$bmp, $g = New-Canvas $w $h
Draw-BlurredBackground $g $w $h 200
Draw-Vignette $g $w $h 160
$cx = $w / 2
Draw-Text $g 'NOW ON STEAM' $famCaps 40 $goldDim $cx 92 'Center' 10 0.04 | Out-Null
Draw-Text $g '48 Achievements to Earn' $famGoth 84 $parchment $cx 175 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 248 260 30

# Achievement icons with their names underneath, centred on $cx.
function Draw-IconGrid($g, $items, $cx, $top, $perRow, $icon, $colW, $rowH, $labelSize) {
    $x0 = $cx - $perRow * $colW / 2
    for ($k = 0; $k -lt $items.Count; $k++) {
        $col = $k % $perRow; $row = [Math]::Floor($k / $perRow)
        $icx = $x0 + $col * $colW + $colW / 2; $iy = $top + $row * $rowH
        Draw-Frame $g ($icx - $icon / 2) $iy $icon $icon
        $img = [System.Drawing.Image]::FromFile((Join-Path $iconDir "$($items[$k][0]).png"))
        $g.DrawImage($img, [float]($icx - $icon / 2), [float]$iy, [float]$icon, [float]$icon); $img.Dispose()
        Draw-Text $g $items[$k][1] $famBody $labelSize $parchment $icx ($iy + $icon + $labelSize * 1.05) 'Center' 0 0.04 | Out-Null
    }
}
Draw-IconGrid $g $showcase $cx 292 6 150 270 232 30
Draw-Text $g 'Gameplay feats, lucky rolls, bad beats, and a few for the graphics menu' $famBody 32 $goldDim $cx ($h - 46) 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'achievements_1920x1080.png'
$g.Dispose(); $bmp.Dispose()


# =============================================================================
# Social media (posts in Social_Posts_Update.txt). Bluesky uses the 16:9 images above
# and in Steam_Update_Art; the 4:5 carousel serves both Instagram and Facebook.
# =============================================================================
$socialDir = Join-Path $OutDir 'Social'
New-Item -ItemType Directory -Force $socialDir | Out-Null
$before = [System.Drawing.Image]::FromFile('K:\Farkle\Documentation\Steam_Update_Art\before_tavern_1920x1080.png')
$featureList = @('Virtualized Geometry', 'Virtual Shadow Maps', 'Ptero Reconstruction', '48 Steam Achievements')

# A small framed caps label, as on a panel's corner.
function Draw-Chip($g, $text, $x, $y, $size) {
    # Measure the letter-spaced run by laying it out once on a scratch canvas.
    $scratch = New-Object System.Drawing.Bitmap 1, 1; $sg = [System.Drawing.Graphics]::FromImage($scratch)
    $tw = (Draw-Text $sg $text $famCaps $size $gold 0 0 'Near' ($size * 0.3) 0).Width
    $sg.Dispose(); $scratch.Dispose()
    $bw = $tw + $size * 1.6; $bh = $size * 2.0
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(215, 14, 8, 5))
    $g.FillRectangle($br, [float]$x, [float]$y, [float]$bw, [float]$bh); $br.Dispose()
    $pen = New-Object System.Drawing.Pen $frameGold, 1.5
    $g.DrawRectangle($pen, [float]$x, [float]$y, [float]$bw, [float]$bh); $pen.Dispose()
    Draw-Text $g $text $famCaps $size $gold ($x + $bw / 2) ($y + $bh / 2) 'Center' ($size * 0.3) 0 | Out-Null
}
function Draw-Panel($g, $img, $x, $y, $pw, $ph) {
    Draw-Frame $g $x $y $pw $ph
    $g.DrawImage($img, [float]$x, [float]$y, [float]$pw, [float]$ph)
    $pen = New-Object System.Drawing.Pen $frameGold, 2
    $g.DrawRectangle($pen, [float]$x, [float]$y, [float]$pw, [float]$ph); $pen.Dispose()
}

# --- Instagram / Facebook carousel, 1080x1350 (4:5) --------------------------
$w = 1080; $h = 1350; $cx = $w / 2

# 1. Cover
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 1960 1180 0.5 0.55 1.0
Fill-Gradient $g 0 0 $w ($h * 0.50) 235 0 90
Fill-Gradient $g 0 ($h * 0.62) $w ($h * 0.38) 0 240 90
Draw-Vignette $g $w $h 130
Draw-Wordmark $g $cx 165 190 | Out-Null
Draw-Text $g 'NEW UPDATE' $famCaps 32 $goldDim $cx 300 'Center' 9 0.04 | Out-Null
Draw-Text $g 'A New Engine' $famGoth 100 $parchment $cx 390 'Center' 0 0.04 | Out-Null
Draw-Text $g 'Foundation' $famGoth 100 $parchment $cx 490 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 575 220 30
for ($i = 0; $i -lt $featureList.Count; $i++) {
    Draw-Text $g $featureList[$i] $famBody 46 $parchment $cx (1005 + $i * 62) 'Center' 0 0.04 | Out-Null
}
Draw-Text $g 'SWIPE FOR MORE' $famCaps 24 $goldDim $cx 1290 'Center' 6 0.04 | Out-Null
Save-Png $bmp 'carousel_1_cover_1080x1350.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# 2. Before / after, stacked
$bmp, $g = New-Canvas $w $h
Draw-BlurredBackground $g $w $h 205
Draw-Vignette $g $w $h 160
Draw-Text $g 'THE TAVERN, REBUILT' $famCaps 34 $goldDim $cx 78 'Center' 8 0.04 | Out-Null
$pw = 960; $ph = 540; $px = ($w - $pw) / 2
Draw-Panel $g $before $px 140 $pw $ph
Draw-Chip $g 'BEFORE' ($px + 20) 160 24
Draw-Panel $g $bg $px 720 $pw $ph
Draw-Chip $g 'AFTER' ($px + 20) 740 24
Draw-Text $g 'Farkle 1.0   →   A New Engine Foundation' $famBody 36 $parchment $cx 1306 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'carousel_2_before_after_1080x1350.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# 3. Ptero Reconstruction, panels stacked
$bmp, $g = New-Canvas $w $h
Draw-BlurredBackground $g $w $h 215
Draw-Vignette $g $w $h 160
Draw-Text $g 'PTERO RECONSTRUCTION' $famCaps 30 $goldDim $cx 70 'Center' 7 0.04 | Out-Null
Draw-Text $g 'Edges rebuilt from the geometry' $famGoth 58 $parchment $cx 140 'Center' 0 0.04 | Out-Null
$cell = 36; $pw = $cols * $cell; $ph = $rows * $cell; $px = ($w - $pw) / 2
Draw-ReconPanel $g $px 205 $cell $false
Draw-Chip $g 'COLOUR-ONLY UPSCALING' ($px + 14) 219 20
Draw-Text $g 'Blends between samples: edges smear, thin rails break up' $famBody 31 $parchment $cx (205 + $ph + 38) 'Center' 0 0.04 | Out-Null
Draw-ReconPanel $g $px 745 $cell $true
Draw-Chip $g 'PTERO-SR' ($px + 14) 759 20
Draw-Text $g 'Knows each triangle edge: exact coverage per pixel' $famBody 31 $parchment $cx (745 + $ph + 38) 'Center' 0 0.04 | Out-Null
Draw-Text $g 'Illustration  ·  same half-resolution input, 2x output' $famBody 24 $goldDim $cx 1305 'Center' 0 0.04 | Out-Null
Save-Png $bmp 'carousel_3_ptero_reconstruction_1080x1350.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# 4. Achievements
$showcase12 = @(
    @('ACH_FIRST_VICTORY', 'First Victory'), @('ACH_STRAIGHT_SHOOTER', 'Straight Shooter'), @('ACH_HOT_DICE', 'Hot Hands'),
    @('ACH_FIRST_FARKLE', 'Farkle!'), @('ACH_HEARTBREAKER', 'Heartbreaker'), @('ACH_COMEBACK', 'Comeback Kid'),
    @('ACH_BANK_3000', 'Greed Is Good'), @('ACH_SPEED_RUN', 'Speed Run'), @('ACH_FOLLOW_THE_LIGHT', 'Follow the Light'),
    @('ACH_IN_HOUSE', 'In-House'), @('ACH_POTATO_MODE', 'Potato Mode'), @('ACH_COLLECTOR', 'Collector'))
$bmp, $g = New-Canvas $w $h
Draw-BlurredBackground $g $w $h 200
Draw-Vignette $g $w $h 160
Draw-Text $g 'NOW ON STEAM' $famCaps 32 $goldDim $cx 78 'Center' 8 0.04 | Out-Null
Draw-Text $g '48 Achievements' $famGoth 92 $parchment $cx 165 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 240 220 28
Draw-IconGrid $g $showcase12 $cx 292 3 172 330 258 32
Save-Png $bmp 'carousel_4_achievements_1080x1350.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# 5. Call to action, by the hearth
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2900 1150 0.62 0.5 1.0
Fill-Gradient $g 0 0 $w $h 150 215 90
Draw-Vignette $g $w $h 150
Draw-Wordmark $g $cx 420 210 | Out-Null
Draw-Text $g 'OUT NOW ON STEAM' $famCaps 36 $goldDim $cx 580 'Center' 9 0.04 | Out-Null
Draw-Text $g 'Pull up a chair.' $famGoth 84 $parchment $cx 680 'Center' 0 0.04 | Out-Null
Draw-Text $g "The fire's been rebuilt." $famGoth 84 $parchment $cx 780 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 870 220 30
$label = 'farkle.pterosoftstudio.com'
$p = New-Object System.Drawing.Drawing2D.GraphicsPath
$p.AddString($label, $famBody, 0, 44, (New-Object System.Drawing.PointF 0, 0), [System.Drawing.StringFormat]::GenericTypographic)
$bw = $p.GetBounds().Width + 80; $p.Dispose(); $bh = 88; $by = 950
Draw-Text $g $label $famBody 44 $parchment $cx ($by + $bh / 2 - 3) 'Center' 0 0.04 | Out-Null
$pen = New-Object System.Drawing.Pen $gold, 2.5
$g.DrawRectangle($pen, [float]($cx - $bw / 2), [float]$by, [float]$bw, [float]$bh); $pen.Dispose()
Save-Png $bmp 'carousel_5_out_now_1080x1350.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# --- Instagram / Facebook story 1080x1920 ------------------------------------
# Content stays between y 250 and 1580: the apps cover the top and bottom with UI.
$w = 1080; $h = 1920; $cx = $w / 2
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 1960 1180 0.5 0.55 1.0
Fill-Gradient $g 0 0 $w ($h * 0.48) 235 0 90
Fill-Gradient $g 0 ($h * 0.60) $w ($h * 0.40) 0 240 90
Draw-Vignette $g $w $h 130
Draw-Wordmark $g $cx 400 220 | Out-Null
Draw-Text $g 'NEW UPDATE' $famCaps 36 $goldDim $cx 550 'Center' 10 0.04 | Out-Null
Draw-Text $g 'A New Engine' $famGoth 110 $parchment $cx 650 'Center' 0 0.04 | Out-Null
Draw-Text $g 'Foundation' $famGoth 110 $parchment $cx 760 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 855 240 32
for ($i = 0; $i -lt $featureList.Count; $i++) {
    Draw-Text $g $featureList[$i] $famBody 50 $parchment $cx (1235 + $i * 68) 'Center' 0 0.04 | Out-Null
}
Draw-Text $g 'OUT NOW ON STEAM' $famCaps 34 $gold $cx 1560 'Center' 9 0.04 | Out-Null
Save-Png $bmp 'story_1080x1920.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# --- YouTube thumbnail 1280x720 ----------------------------------------------
# Few, big words: it has to read at the size of a phone's video list.
$w = 1280; $h = 720
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 2300 1150 0.62 0.5 1.0
Fill-Gradient $g 0 0 ($w * 0.75) $h 240 0 0
Draw-Vignette $g $w $h 120
$tx = 60
Draw-Wordmark $g $tx 150 170 'Near' | Out-Null
Draw-Text $g 'New Engine' $famGoth 118 $parchment $tx 345 'Near' 0 0.05 | Out-Null
Draw-Text $g '+ 48 Achievements' $famGoth 78 $gold $tx 470 'Near' 0 0.05 | Out-Null
Draw-Text $g 'UPDATE OUT NOW' $famCaps 38 $goldDim $tx 590 'Near' 9 0.04 | Out-Null
Save-Png $bmp 'youtube_thumbnail_1280x720.png' $socialDir
$g.Dispose(); $bmp.Dispose()

# --- YouTube community post / Facebook square 1080x1080 ----------------------
$w = 1080; $h = 1080; $cx = $w / 2
$bmp, $g = New-Canvas $w $h
Draw-Background $g $w $h 1960 1180 0.5 0.55 1.0
Fill-Gradient $g 0 0 $w ($h * 0.50) 235 0 90
Fill-Gradient $g 0 ($h * 0.62) $w ($h * 0.38) 0 240 90
Draw-Vignette $g $w $h 130
Draw-Wordmark $g $cx 150 180 | Out-Null
Draw-Text $g 'NEW UPDATE' $famCaps 30 $goldDim $cx 280 'Center' 9 0.04 | Out-Null
Draw-Text $g $updateTitle $famGoth 48 $parchment $cx 362 'Center' 0 0.04 | Out-Null
Draw-Rule $g $cx 435 220 28
Draw-Text $g 'Virtualized Geometry  ·  Virtual Shadow Maps' $famBody 40 $parchment $cx 905 'Center' 0 0.04 | Out-Null
Draw-Text $g 'Ptero Reconstruction  ·  48 Steam Achievements' $famBody 40 $parchment $cx 963 'Center' 0 0.04 | Out-Null
Draw-Text $g 'OUT NOW ON STEAM' $famCaps 26 $goldDim $cx 1030 'Center' 7 0.04 | Out-Null
Save-Png $bmp 'square_1080x1080.png' $socialDir
$g.Dispose(); $bmp.Dispose()

$before.Dispose()
$bg.Dispose(); $atlas.Dispose()
