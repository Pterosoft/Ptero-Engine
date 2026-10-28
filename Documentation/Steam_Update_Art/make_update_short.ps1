param([string]$OutDir = 'K:\Farkle\Documentation\Steam_Update_Art', [double]$Seconds = 18)

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

# Vertical short (1080x1920, 30 fps) for Reels / Shorts. Frames are drawn here and piped
# straight into ffmpeg as raw BGRA, then music and fireplace audio are muxed in.
# Text stays inside the platforms' safe area (clear of the top bar, the caption block at
# the bottom and the button column on the right).

$W = 1080; $H = 1920; $Fps = 30
$frames = [int]($Seconds * $Fps)
$assets = 'K:\Farkle\Data\fmod_project\Ptero-Engine\Assets'
$music  = "$assets\Emilio_Villalba.-_Viola_de_teclas_medieval_(Nickelharpa)._Saltarello.webm.1080p.vp9.wav"
$fire   = "$assets\387128__nooneisreal__the-fireplace-3.wav"
$silent = Join-Path $env:TEMP 'farkle_short_video.mp4'
$outMp4 = Join-Path $OutDir 'farkle_update_short_1080x1920.mp4'

$goldBrush = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xB0, 0x6C)
$grade = New-Object System.Drawing.Imaging.ColorMatrix
$grade.Matrix00 = 1.04; $grade.Matrix11 = 0.98; $grade.Matrix22 = 0.94
$grade.Matrix40 = -0.01; $grade.Matrix41 = -0.01; $grade.Matrix42 = -0.01
$gradeAttr = New-Object System.Drawing.Imaging.ImageAttributes
$gradeAttr.SetColorMatrix($grade)

function Clamp01($v) { [Math]::Min(1.0, [Math]::Max(0.0, $v)) }
function Smooth($v) { $v = Clamp01 $v; $v * $v * (3 - 2 * $v) }
# 0 -> 1 over [t0, t0+fadeIn], back to 0 over [t1-fadeOut, t1] (t1 < 0 means stay).
function Envelope($t, $t0, $fadeIn, $t1 = -1, $fadeOut = 0.35) {
    $a = Smooth (($t - $t0) / $fadeIn)
    if ($t1 -ge 0) { $a = [Math]::Min($a, (Smooth (($t1 - $t) / $fadeOut))) }
    return $a
}

# A full-width transparent strip; $draw renders into it in local coordinates.
function New-Layer($h, [scriptblock]$draw) {
    $bmp, $lg = New-Canvas $W $h
    & $draw $lg
    $lg.Dispose()
    return $bmp
}
$layerAttr = New-Object System.Drawing.Imaging.ImageAttributes
function Draw-Layer($g, $layer, $y, $alpha) {
    if ($alpha -le 0.004) { return }
    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix33 = [float]$alpha
    $layerAttr.SetColorMatrix($m)
    $dst = New-Object System.Drawing.Rectangle 0, ([int][Math]::Round($y)), $layer.Width, $layer.Height
    $g.DrawImage($layer, $dst, 0, 0, $layer.Width, $layer.Height, 'Pixel', $layerAttr)
}

# --- Static layers ------------------------------------------------------------
$cx = $W / 2
# Darkening that sits between the moving scene and the text.
$shade = New-Layer $H {
    param($lg)
    Fill-Gradient $lg 0 0 $W ($H * 0.46) 235 0 90
    Fill-Gradient $lg 0 ($H * 0.58) $W ($H * 0.42) 0 235 90
    Draw-Vignette $lg $W $H 120
}
$lyLogo  = New-Layer 280 { param($lg) Draw-Text $lg 'Farkle' $famTitle 225 $goldBrush $cx 135 'Center' 0 0.03 | Out-Null }
$lyEyebr = New-Layer 70  { param($lg) Draw-Text $lg 'NEW UPDATE' $famCaps 40 $goldDim $cx 35 'Center' 10 0.04 | Out-Null }
$lyTitle = New-Layer 250 {
    param($lg)
    Draw-Text $lg 'A New Engine' $famGoth 108 $parchment $cx 60 'Center' 0 0.04 | Out-Null
    Draw-Text $lg 'Foundation' $famGoth 108 $parchment $cx 165 'Center' 0 0.04 | Out-Null
}
$lyRule  = New-Layer 60  { param($lg) Draw-Rule $lg $cx 30 260 34 }
$lyWhats = New-Layer 60  { param($lg) Draw-Text $lg "WHAT'S NEW" $famCaps 34 $goldDim $cx 30 'Center' 9 0.04 | Out-Null }
$features = 'Virtualized Geometry', 'Virtual Shadow Maps', 'Path Tracing', 'Better Global Illumination', 'Bloom & Lens Flares', 'Steam Achievements'
$lyFeat = foreach ($f in $features) {
    New-Layer 130 { param($lg) Draw-Text $lg $f $famBody 78 $parchment $cx 65 'Center' 0 0.04 | Out-Null }
}
$lyCta = New-Layer 230 {
    param($lg)
    Draw-Text $lg 'Learn more at' $famBody 46 $goldDim $cx 40 'Center' 0 0.04 | Out-Null
    $label = 'farkle.pterosoftstudio.com'
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddString($label, $famBody, 0, 54, (New-Object System.Drawing.PointF 0, 0), [System.Drawing.StringFormat]::GenericTypographic)
    $bw = $p.GetBounds().Width + 90; $p.Dispose()
    $by = 95; $bh = 104
    Draw-Text $lg $label $famBody 54 $parchment $cx ($by + $bh / 2 - 3) 'Center' 0 0.04 | Out-Null
    $pen = New-Object System.Drawing.Pen $goldBrush, 3
    $lg.DrawRectangle($pen, [float]($cx - $bw / 2), [float]$by, [float]$bw, [float]$bh); $pen.Dispose()
}

# --- Embers -------------------------------------------------------------------
$rng = New-Object System.Random 7
$embers = for ($i = 0; $i -lt 70; $i++) {
    [pscustomobject]@{
        X = $rng.NextDouble() * $W; Y = $rng.NextDouble() * $H
        Speed = 35 + $rng.NextDouble() * 70; Size = 2 + $rng.NextDouble() * 4
        Phase = $rng.NextDouble() * 6.28; Sway = 10 + $rng.NextDouble() * 25
        Bright = 90 + $rng.Next(150)
    }
}
function Draw-Embers($g, $t) {
    foreach ($e in $embers) {
        $y = ($e.Y - $e.Speed * $t) % $H; if ($y -lt 0) { $y += $H }
        $x = $e.X + [Math]::Sin($t * 0.9 + $e.Phase) * $e.Sway
        $flick = 0.65 + 0.35 * [Math]::Sin($t * 5 + $e.Phase * 3)
        $a = [int]($e.Bright * $flick)
        $s = $e.Size
        $glow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([int]($a * 0.25), 255, 140, 40))
        $core = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, 255, 200, 120))
        $g.FillEllipse($glow, [float]($x - $s * 2), [float]($y - $s * 2), [float]($s * 4), [float]($s * 4))
        $g.FillEllipse($core, [float]($x - $s / 2), [float]($y - $s / 2), [float]$s, [float]$s)
        $glow.Dispose(); $core.Dispose()
    }
}

# --- Encode -------------------------------------------------------------------
$psi = New-Object System.Diagnostics.ProcessStartInfo 'ffmpeg'
$psi.Arguments = "-y -hide_banner -loglevel error -f rawvideo -pix_fmt bgra -s ${W}x${H} -r $Fps -i - " +
                 "-c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -profile:v high `"$silent`""
$psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true
$ff = [System.Diagnostics.Process]::Start($psi)
$stdin = $ff.StandardInput.BaseStream

$frame, $g = New-Canvas $W $H
$buf = New-Object byte[] ($W * $H * 4)
$rect = New-Object System.Drawing.Rectangle 0, 0, $W, $H
# Pan window over the 3840x2160 shot: from the candle bench towards the hearth, slowly pushing in.
$srcH0 = $bg.Height; $aspect = $W / $H
$lift = { param($a) (1 - $a) * 24 }
$featStart = 3.9; $featStep = 1.35
$featEnd = $featStart + $featStep * $features.Count
for ($f = 0; $f -lt $frames; $f++) {
    $t = $f / $Fps
    $k = (Smooth ($t / $Seconds)) * 0.85 + ($t / $Seconds) * 0.15
    $zoom = 1.0 + 0.08 * $k
    $sh = $srcH0 / $zoom; $sw = $sh * $aspect
    $cxs = 1500 + (2620 - 1500) * $k
    $sx = [Math]::Min([Math]::Max($cxs - $sw / 2, 0), $bg.Width - $sw)
    $sy = ($srcH0 - $sh) * 0.45
    $g.DrawImage($bg, $rect, [float]$sx, [float]$sy, [float]$sw, [float]$sh, 'Pixel', $gradeAttr)
    Draw-Embers $g $t
    $g.DrawImage($shade, 0, 0, $W, $H)

    $a = Envelope $t 0.3 1.0;  Draw-Layer $g $lyLogo  (130 + (& $lift $a)) $a
    $a = Envelope $t 1.4 0.7;  Draw-Layer $g $lyEyebr (395 + (& $lift $a)) $a
    $a = Envelope $t 1.9 0.9;  Draw-Layer $g $lyTitle (455 + (& $lift $a)) $a
    $a = Envelope $t 2.9 0.7;  Draw-Layer $g $lyRule  675 $a
    # Features take turns in one slot, then the call to action replaces them.
    $a = Envelope $t 3.6 0.5 $featEnd 0.35; Draw-Layer $g $lyWhats 1140 $a
    for ($i = 0; $i -lt $features.Count; $i++) {
        $t0 = $featStart + $i * $featStep
        $a = Envelope $t $t0 0.3 ($t0 + $featStep) 0.3
        Draw-Layer $g $lyFeat[$i] (1200 + (& $lift $a)) $a
    }
    $a = Envelope $t ($featEnd + 0.3) 0.8; Draw-Layer $g $lyCta (1160 + (& $lift $a)) $a

    # Fade from and to black.
    $fade = [Math]::Max(1 - (Smooth ($t / 0.5)), 1 - (Smooth (($Seconds - $t) / 0.8)))
    if ($fade -gt 0.004) {
        $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([int](255 * $fade), 0, 0, 0))
        $g.FillRectangle($br, 0, 0, $W, $H); $br.Dispose()
    }

    $bd = $frame.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $buf, 0, $buf.Length)
    $frame.UnlockBits($bd)
    $stdin.Write($buf, 0, $buf.Length)
    if ($f % 90 -eq 0) { Write-Host "frame $f / $frames" }
    if ($f -eq [int](13.5 * $Fps)) { Save-Png $frame 'short_preview_frame.png' | Out-Null }
}
$stdin.Close(); $ff.WaitForExit()
$g.Dispose(); $frame.Dispose()

# Music (starts after its 0.5 s lead-in) with the hearth crackling underneath.
$fadeOutAt = $Seconds - 1.5
& ffmpeg -y -hide_banner -loglevel error -i $silent -ss 0.5 -i $music -i $fire `
    -filter_complex "[1:a]volume=-4dB,afade=t=in:d=0.6,afade=t=out:st=${fadeOutAt}:d=1.5[m];[2:a]volume=16dB,afade=t=in:d=0.6,afade=t=out:st=${fadeOutAt}:d=1.5[f];[m][f]amix=inputs=2:normalize=0,alimiter=limit=0.95[a]" `
    -map 0:v -map '[a]' -c:v copy -c:a aac -b:a 192k -ar 48000 -t $Seconds -movflags +faststart $outMp4
Remove-Item $silent
Write-Output $outMp4

$bg.Dispose(); $atlas.Dispose()
