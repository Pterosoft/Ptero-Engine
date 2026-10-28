param([string]$OutDir = 'K:\Farkle\Documentation\Steam_Update_Art', [double]$Seconds = 15)

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

# Vertical before/after short (1080x1920, 30 fps) for Reels / Shorts. The two shots are
# stacked, the new one is revealed with a wipe, then an end card carries the link.
# Frames are piped into ffmpeg as raw BGRA; music and fireplace audio are muxed after.

$W = 1080; $H = 1920; $Fps = 30
$frames = [int]($Seconds * $Fps)
$assets = 'K:\Farkle\Data\fmod_project\Ptero-Engine\Assets'
$music  = "$assets\Emilio_Villalba.-_Viola_de_teclas_medieval_(Nickelharpa)._Saltarello.webm.1080p.vp9.wav"
$fire   = "$assets\387128__nooneisreal__the-fireplace-3.wav"
$silent = Join-Path $env:TEMP 'farkle_compare_video.mp4'
$outMp4 = Join-Path $OutDir 'farkle_before_after_short_1080x1920.mp4'
$before = [System.Drawing.Image]::FromFile((Join-Path $OutDir 'before_tavern_1920x1080.png'))

$goldBrush = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xB0, 0x6C)

function Clamp01($v) { [Math]::Min(1.0, [Math]::Max(0.0, $v)) }
function Smooth($v) { $v = Clamp01 $v; $v * $v * (3 - 2 * $v) }
# 0 -> 1 over [t0, t0+fadeIn], back to 0 over [t1-fadeOut, t1] (t1 < 0 means stay).
function Envelope($t, $t0, $fadeIn, $t1 = -1, $fadeOut = 0.35) {
    $a = Smooth (($t - $t0) / $fadeIn)
    if ($t1 -ge 0) { $a = [Math]::Min($a, (Smooth (($t1 - $t) / $fadeOut))) }
    return $a
}
function New-Layer($lw, $lh, [scriptblock]$draw) {
    $bmp, $lg = New-Canvas $lw $lh
    & $draw $lg
    $lg.Dispose()
    return $bmp
}
$layerAttr = New-Object System.Drawing.Imaging.ImageAttributes
function Draw-Layer($g, $layer, $x, $y, $alpha) {
    if ($alpha -le 0.004) { return }
    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix33 = [float]$alpha
    $layerAttr.SetColorMatrix($m)
    $dst = New-Object System.Drawing.Rectangle ([int][Math]::Round($x)), ([int][Math]::Round($y)), $layer.Width, $layer.Height
    $g.DrawImage($layer, $dst, 0, 0, $layer.Width, $layer.Height, 'Pixel', $layerAttr)
}

# --- Static layers ------------------------------------------------------------
$cx = $W / 2
# Background: blurred, darkened new scene (thumbnail stretched back up).
$backdrop = New-Layer $W $H {
    param($lg)
    $thumb = New-Object System.Drawing.Bitmap 48, 27
    $tg = [System.Drawing.Graphics]::FromImage($thumb); $tg.InterpolationMode = 'HighQualityBicubic'
    $tg.DrawImage($bg, 0, 0, 48, 27); $tg.Dispose()
    $bw = $H * 16 / 9
    $lg.DrawImage($thumb, [float](($W - $bw) / 2), -20, [float]$bw, [float]($H + 40)); $thumb.Dispose()
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(218, 12, 7, 4))
    $lg.FillRectangle($br, 0, 0, $W, $H); $br.Dispose()
    Draw-Vignette $lg $W $H 170
}
$lyLogo = New-Layer $W 230 { param($lg) Draw-Text $lg 'Farkle' $famTitle 170 $goldBrush $cx 105 'Center' 0 0.03 | Out-Null }
$lyEyebr = New-Layer $W 60 { param($lg) Draw-Text $lg 'THE TAVERN, REBUILT' $famCaps 32 $goldDim $cx 30 'Center' 8 0.04 | Out-Null }
# Panel labels: small dark tags pinned to each panel's top-left corner.
function New-Tag($caps, $sub) {
    New-Layer 560 80 {
        param($lg)
        # Lay the text out once to learn its width, then clear and draw the box underneath it.
        $b = Draw-Text $lg $caps $famCaps 30 $goldBrush 28 32 'Near' 4 0.03
        $b2 = Draw-Text $lg "·  $sub" $famBody 34 $parchment ($b.Right + 10) 30 'Near' 0 0.03
        $tw = $b2.Right + 28
        $lg.Clear([System.Drawing.Color]::Transparent)
        $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200, 14, 9, 5))
        $lg.FillRectangle($br, 0, 0, [float]$tw, 64); $br.Dispose()
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 0xC9, 0xA2, 0x63)), 2
        $lg.DrawRectangle($pen, 1, 1, [float]($tw - 2), 62); $pen.Dispose()
        Draw-Text $lg $caps $famCaps 30 $goldBrush 28 32 'Near' 4 0.03 | Out-Null
        Draw-Text $lg "·  $sub" $famBody 34 $parchment ($b.Right + 10) 30 'Near' 0 0.03 | Out-Null
    }
}
$tagBefore = New-Tag 'BEFORE' 'Farkle 1.0'
$tagAfter  = New-Tag 'AFTER' 'New Engine'
# End card.
$lyTitle = New-Layer $W 250 {
    param($lg)
    Draw-Text $lg 'A New Engine' $famGoth 112 $parchment $cx 65 'Center' 0 0.04 | Out-Null
    Draw-Text $lg 'Foundation' $famGoth 112 $parchment $cx 180 'Center' 0 0.04 | Out-Null
}
$lyRule = New-Layer $W 60 { param($lg) Draw-Rule $lg $cx 30 260 34 }
$lyCta = New-Layer $W 230 {
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
$rng = New-Object System.Random 11
$embers = for ($i = 0; $i -lt 55; $i++) {
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

# --- Panels -------------------------------------------------------------------
$pw = 1000; $ph = 562; $px = ($W - $pw) / 2
$pyBefore = 360; $pyAfter = 985
$panelAttr = New-Object System.Drawing.Imaging.ImageAttributes
# Draws $img into the panel slot with a slow push-in; $reveal (0..1) wipes it in from the left.
function Draw-Panel($g, $img, $y, $zoom, $alpha, $reveal) {
    if ($alpha -le 0.004 -or $reveal -le 0.001) { return }
    $sw = $img.Width / $zoom; $sh = $img.Height / $zoom
    $sx = ($img.Width - $sw) / 2; $sy = ($img.Height - $sh) / 2
    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix33 = [float]$alpha
    $panelAttr.SetColorMatrix($m)
    $clipW = $pw * $reveal
    $g.SetClip((New-Object System.Drawing.RectangleF ([float]$px), ([float]$y), ([float]$clipW), ([float]$ph)))
    $dst = New-Object System.Drawing.Rectangle ([int]$px), ([int]$y), $pw, $ph
    $g.DrawImage($img, $dst, [float]$sx, [float]$sy, [float]$sw, [float]$sh, 'Pixel', $panelAttr)
    $g.ResetClip()
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb([int](200 * $alpha), 0xC9, 0xA2, 0x63)), 2
    $g.DrawRectangle($pen, [float]$px, [float]$y, [float]$clipW, [float]$ph); $pen.Dispose()
    if ($reveal -lt 0.999) {
        # Glowing edge riding the wipe.
        $ex = $px + $clipW
        foreach ($k in @(@(26, 40), @(12, 90), @(3, 255))) {
            $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([int]($k[1] * $alpha), 255, 196, 110))
            $g.FillRectangle($br, [float]($ex - $k[0] / 2), [float]($y - 6), [float]$k[0], [float]($ph + 12)); $br.Dispose()
        }
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
$lift = { param($a) (1 - $a) * 24 }
$tEnd = 10.2   # panels hand over to the end card here
for ($f = 0; $f -lt $frames; $f++) {
    $t = $f / $Fps
    $g.DrawImage($backdrop, 0, 0, $W, $H)
    Draw-Embers $g $t

    $a = Envelope $t 0.2 0.9; Draw-Layer $g $lyLogo 0 (95 + (& $lift $a)) $a
    $a = Envelope $t 0.8 0.7 $tEnd 0.5; Draw-Layer $g $lyEyebr 0 (285 + (& $lift $a)) $a

    $zoom = 1.0 + 0.10 * ($t / $Seconds)
    $pa = Envelope $t 0.9 0.7 $tEnd 0.6
    Draw-Panel $g $before ($pyBefore + (& $lift $pa)) $zoom $pa 1.0
    Draw-Layer $g $tagBefore ($px + 18) ($pyBefore + 18 + (& $lift $pa)) (Envelope $t 1.4 0.5 $tEnd 0.6)

    $reveal = Smooth (($t - 3.0) / 1.6)
    $aa = Envelope $t 3.0 0.15 $tEnd 0.6
    Draw-Panel $g $bg $pyAfter $zoom $aa $reveal
    Draw-Layer $g $tagAfter ($px + 18) ($pyAfter + 18) (Envelope $t 4.5 0.5 $tEnd 0.6)

    # End card.
    $a = Envelope $t ($tEnd + 0.3) 0.9; Draw-Layer $g $lyTitle 0 (560 + (& $lift $a)) $a
    $a = Envelope $t ($tEnd + 0.9) 0.7; Draw-Layer $g $lyRule 0 820 $a
    $a = Envelope $t ($tEnd + 1.3) 0.8; Draw-Layer $g $lyCta 0 (930 + (& $lift $a)) $a

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

$before.Dispose(); $bg.Dispose(); $atlas.Dispose()
