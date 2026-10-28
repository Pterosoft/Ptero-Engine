param([string]$OutDir = 'K:\Farkle\Documentation\Steam_Update_Art', [string]$PreviewDir = $env:TEMP)

# Two more vertical shorts (1080x1920, 30 fps) for Reels / Shorts, alongside
# make_update_short.ps1 and make_compare_short.ps1:
#   farkle_ptero_short_1080x1920.mp4         Ptero Reconstruction, explained
#   farkle_achievements_short_1080x1920.mp4  the achievements the game can award
# Frames are drawn here and piped into ffmpeg as raw BGRA, then music and fireplace audio
# are muxed in. Text stays inside the platforms' safe area: y 250-1500, clear of the
# button column on the right.

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$bgPath    = 'K:\Farkle\Screenshots\screenshot_20261022_181742.png'
$atlasPath = 'K:\Farkle\Data\UI\Farkle\Textures\farkle-atlas.png'
$fontDir   = 'K:\Farkle\Data\UI\Farkle\Fonts'
$achDir    = 'K:\Farkle\Steamworks\Achievements'
$achCode   = 'K:\Farkle\Source\Game\GameAchievements.cpp'

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

$goldBrush = [System.Drawing.Color]::FromArgb(255, 0xD9, 0xB0, 0x6C)
$goldDim   = [System.Drawing.Color]::FromArgb(230, 0xD3, 0xB6, 0x86)
$parchment = [System.Drawing.Color]::FromArgb(240, 0xEF, 0xDD, 0xBE)
$frameGold = [System.Drawing.Color]::FromArgb(200, 0xC9, 0xA2, 0x63)

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

# A small framed caps label, as on a panel's corner.
function Draw-Chip($g, $text, $x, $y, $size) {
    # Measure the letter-spaced run by laying it out once on a scratch canvas.
    $scratch = New-Object System.Drawing.Bitmap 1, 1; $sg = [System.Drawing.Graphics]::FromImage($scratch)
    $tw = (Draw-Text $sg $text $famCaps $size $goldBrush 0 0 'Near' ($size * 0.3) 0).Width
    $sg.Dispose(); $scratch.Dispose()
    $bw = $tw + $size * 1.6; $bh = $size * 2.0
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(215, 14, 8, 5))
    $g.FillRectangle($br, [float]$x, [float]$y, [float]$bw, [float]$bh); $br.Dispose()
    $pen = New-Object System.Drawing.Pen $frameGold, 1.5
    $g.DrawRectangle($pen, [float]$x, [float]$y, [float]$bw, [float]$bh); $pen.Dispose()
    Draw-Text $g $text $famCaps $size $goldBrush ($x + $bw / 2) ($y + $bh / 2) 'Center' ($size * 0.3) 0 | Out-Null
}

# --- Shared short machinery ---------------------------------------------------
$W = 1080; $H = 1920; $Fps = 30; $cx = $W / 2
$assets = 'K:\Farkle\Data\fmod_project\Ptero-Engine\Assets'
$music  = "$assets\Emilio_Villalba.-_Viola_de_teclas_medieval_(Nickelharpa)._Saltarello.webm.1080p.vp9.wav"
$fire   = "$assets\387128__nooneisreal__the-fireplace-3.wav"

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
$lift = { param($a) (1 - $a) * 24 }

# A full-width transparent strip; $draw renders into it in local coordinates.
function New-Layer($h, [scriptblock]$draw) {
    $bmp, $lg = New-Canvas $W $h
    & $draw $lg
    $lg.Dispose()
    return $bmp
}
$layerAttr = New-Object System.Drawing.Imaging.ImageAttributes
function Draw-Layer($g, $layer, $y, $alpha, $x = 0) {
    if ($alpha -le 0.004) { return }
    $m = New-Object System.Drawing.Imaging.ColorMatrix
    $m.Matrix33 = [float]$alpha
    $layerAttr.SetColorMatrix($m)
    $dst = New-Object System.Drawing.Rectangle ([int][Math]::Round($x)), ([int][Math]::Round($y)), $layer.Width, $layer.Height
    $g.DrawImage($layer, $dst, 0, 0, $layer.Width, $layer.Height, 'Pixel', $layerAttr)
}

# Slow pan over the 3840x2160 shot between two source x centres, pushing in a little.
function Draw-Pan($g, $t, $seconds, $fromX, $toX) {
    $k = (Smooth ($t / $seconds)) * 0.85 + ($t / $seconds) * 0.15
    $zoom = 1.0 + 0.08 * $k
    $sh = $bg.Height / $zoom; $sw = $sh * $W / $H
    $cxs = $fromX + ($toX - $fromX) * $k
    $sx = [Math]::Min([Math]::Max($cxs - $sw / 2, 0), $bg.Width - $sw)
    $sy = ($bg.Height - $sh) * 0.45
    $g.DrawImage($bg, (New-Object System.Drawing.Rectangle 0, 0, $W, $H), [float]$sx, [float]$sy, [float]$sw, [float]$sh, 'Pixel', $gradeAttr)
}

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
function Fill-Black($g, $alpha) {
    if ($alpha -le 0.004) { return }
    $br = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb([int](255 * (Clamp01 $alpha)), 8, 5, 3))
    $g.FillRectangle($br, 0, 0, $W, $H); $br.Dispose()
}

$shade = New-Layer $H {
    param($lg)
    Fill-Gradient $lg 0 0 $W ($H * 0.46) 235 0 90
    Fill-Gradient $lg 0 ($H * 0.58) $W ($H * 0.42) 0 235 90
    Draw-Vignette $lg $W $H 120
}
$lyLogo = New-Layer 240 { param($lg) Draw-Text $lg 'Farkle' $famTitle 190 $goldBrush $cx 115 'Center' 0 0.03 | Out-Null }
$lyCta = New-Layer 330 {
    param($lg)
    Draw-Text $lg 'OUT NOW ON STEAM' $famCaps 40 $goldDim $cx 40 'Center' 10 0.04 | Out-Null
    Draw-Text $lg 'Learn more at' $famBody 46 $goldDim $cx 140 'Center' 0 0.04 | Out-Null
    $label = 'farkle.pterosoftstudio.com'
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddString($label, $famBody, 0, 54, (New-Object System.Drawing.PointF 0, 0), [System.Drawing.StringFormat]::GenericTypographic)
    $bw = $p.GetBounds().Width + 90; $p.Dispose()
    $by = 195; $bh = 104
    Draw-Text $lg $label $famBody 54 $parchment $cx ($by + $bh / 2 - 3) 'Center' 0 0.04 | Out-Null
    $pen = New-Object System.Drawing.Pen $goldBrush, 3
    $lg.DrawRectangle($pen, [float]($cx - $bw / 2), [float]$by, [float]$bw, [float]$bh); $pen.Dispose()
}

# Renders $seconds of frames from $drawFrame (param $g, $t), then muxes the audio.
# $musicStart picks where in the track the short begins, so the shorts don't all sound alike.
function Render-Short($name, $seconds, $musicStart, [scriptblock]$drawFrame, $previewAt) {
    $frames = [int]($seconds * $Fps)
    $silent = Join-Path $env:TEMP "farkle_short_$name.mp4"
    $outMp4 = Join-Path $OutDir "farkle_${name}_short_1080x1920.mp4"
    $psi = New-Object System.Diagnostics.ProcessStartInfo 'ffmpeg'
    $psi.Arguments = "-y -hide_banner -loglevel error -f rawvideo -pix_fmt bgra -s ${W}x${H} -r $Fps -i - " +
                     "-c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -profile:v high `"$silent`""
    $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true
    $ff = [System.Diagnostics.Process]::Start($psi)
    $stdin = $ff.StandardInput.BaseStream
    $frame, $g = New-Canvas $W $H
    $buf = New-Object byte[] ($W * $H * 4)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $W, $H
    for ($f = 0; $f -lt $frames; $f++) {
        $t = $f / $Fps
        & $drawFrame $g $t
        # Fade from and to black.
        Fill-Black $g ([Math]::Max(1 - (Smooth ($t / 0.5)), 1 - (Smooth (($seconds - $t) / 0.8))))
        $bd = $frame.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $buf, 0, $buf.Length)
        $frame.UnlockBits($bd)
        $stdin.Write($buf, 0, $buf.Length)
        if ($f % 90 -eq 0) { Write-Host "$name frame $f / $frames" }
        foreach ($p in $previewAt) { if ($f -eq [int]($p * $Fps)) { $frame.Save((Join-Path $PreviewDir "preview_${name}_$p.png")) } }
    }
    $stdin.Close(); $ff.WaitForExit()
    $g.Dispose(); $frame.Dispose()
    # Music with the hearth crackling underneath.
    $fadeOutAt = $seconds - 1.5
    & ffmpeg -y -hide_banner -loglevel error -i $silent -ss $musicStart -i $music -i $fire `
        -filter_complex "[1:a]volume=-4dB,afade=t=in:d=0.6,afade=t=out:st=${fadeOutAt}:d=1.5[m];[2:a]volume=16dB,afade=t=in:d=0.6,afade=t=out:st=${fadeOutAt}:d=1.5[f];[m][f]amix=inputs=2:normalize=0,alimiter=limit=0.95[a]" `
        -map 0:v -map '[a]' -c:v copy -c:a aac -b:a 192k -ar 48000 -t $seconds -movflags +faststart $outMp4
    Remove-Item $silent
    Write-Output $outMp4
}


# =============================================================================
# Short 1: Ptero Reconstruction
# =============================================================================
# The same illustration as the Steam explainer: a thin dark railing and a beam's edge
# across a lit wall, upscaled 2x from half-resolution samples, first by blending colours,
# then by Ptero-SR's exact coverage. The two are wiped across one another.
$wall = @(0xE8, 0xC9, 0x96); $wood = @(0x2A, 0x18, 0x0E)
function Scene-IsDark([double]$x, [double]$y) {
    $d = [Math]::Abs(($y - (2.6 + 0.30 * $x)) * 0.958)
    if ($d -lt 0.55) { return $true }
    return $y -gt (12.6 - 0.42 * $x)
}
function Mix($t) {
    $c = for ($k = 0; $k -lt 3; $k++) { [int]($wall[$k] + ($wood[$k] - $wall[$k]) * $t) }
    return [System.Drawing.Color]::FromArgb(255, $c[0], $c[1], $c[2])
}
$cols = 18; $rows = 12; $cell = 44
$sCols = $cols / 2; $sRows = $rows / 2
$samples = New-Object 'double[,]' $sCols, $sRows
for ($j = 0; $j -lt $sRows; $j++) { for ($i = 0; $i -lt $sCols; $i++) {
    $samples[$i, $j] = if (Scene-IsDark (2 * $i + 1) (2 * $j + 1)) { 1.0 } else { 0.0 }
} }
$pw = $cols * $cell; $ph = $rows * $cell; $panelX = [int](($W - $pw) / 2)
# A full-width layer holding one panel; $exact picks Ptero-SR over the bilinear blend.
function New-ReconLayer($exact) {
    New-Layer ($ph + 2) {
        param($lg)
        $px = $panelX
        for ($r = 0; $r -lt $rows; $r++) { for ($c = 0; $c -lt $cols; $c++) {
            if ($exact) {
                $n = 12; $hits = 0
                for ($b = 0; $b -lt $n; $b++) { for ($a = 0; $a -lt $n; $a++) {
                    if (Scene-IsDark ($c + ($a + 0.5) / $n) ($r + ($b + 0.5) / $n)) { $hits++ }
                } }
                $v = $hits / ($n * $n)
            } else {
                $sx = [Math]::Min([Math]::Max(($c + 0.5 - 1) / 2.0, 0), $sCols - 1)
                $sy = [Math]::Min([Math]::Max(($r + 0.5 - 1) / 2.0, 0), $sRows - 1)
                $i0 = [Math]::Floor($sx); $j0 = [Math]::Floor($sy); $fx = $sx - $i0; $fy = $sy - $j0
                $i1 = [Math]::Min($i0 + 1, $sCols - 1); $j1 = [Math]::Min($j0 + 1, $sRows - 1)
                $v = ($samples[$i0, $j0] * (1 - $fx) + $samples[$i1, $j0] * $fx) * (1 - $fy) + ($samples[$i0, $j1] * (1 - $fx) + $samples[$i1, $j1] * $fx) * $fy
            }
            $br = New-Object System.Drawing.SolidBrush (Mix $v)
            $lg.FillRectangle($br, [float]($px + $c * $cell), [float]($r * $cell), [float]$cell, [float]$cell); $br.Dispose()
        } }
        $gridPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(45, 0, 0, 0)), 1
        for ($c = 1; $c -lt $cols; $c++) { $lg.DrawLine($gridPen, [float]($px + $c * $cell), 0, [float]($px + $c * $cell), [float]$ph) }
        for ($r = 1; $r -lt $rows; $r++) { $lg.DrawLine($gridPen, [float]$px, [float]($r * $cell), [float]($px + $pw), [float]($r * $cell)) }
        $gridPen.Dispose()
        $edgePen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(190, 0xF2, 0xC2, 0x6A)), 2.5
        $edgePen.DashStyle = 'Dash'
        $lg.SetClip((New-Object System.Drawing.RectangleF ([float]$px), 0, ([float]$pw), ([float]$ph)))
        foreach ($o in @((-0.55 / 0.958), (0.55 / 0.958))) {
            $lg.DrawLine($edgePen, [float]$px, [float]((2.6 + $o) * $cell), [float]($px + $pw), [float]((2.6 + $o + 0.30 * $cols) * $cell))
        }
        $lg.DrawLine($edgePen, [float]$px, [float](12.6 * $cell), [float]($px + $pw), [float]((12.6 - 0.42 * $cols) * $cell))
        $lg.ResetClip(); $edgePen.Dispose()
        $dot = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(160, 0x6E, 0x9C, 0xC8))
        for ($j = 0; $j -lt $sRows; $j++) { for ($i = 0; $i -lt $sCols; $i++) {
            $lg.FillEllipse($dot, [float]($px + (2 * $i + 1) * $cell - 4), [float]((2 * $j + 1) * $cell - 4), 8, 8)
        } }
        $dot.Dispose()
        $pen = New-Object System.Drawing.Pen $frameGold, 2
        $lg.DrawRectangle($pen, [float]$px, 0, [float]$pw, [float]$ph); $pen.Dispose()
    }
}
$lyBlend = New-ReconLayer $false
$lyExact = New-ReconLayer $true
$lyChipBlend = New-Layer 60 { param($lg) Draw-Chip $lg 'COLOUR-ONLY UPSCALING' ($panelX + 16) 6 22 }
$lyChipExact = New-Layer 60 { param($lg) Draw-Chip $lg 'PTERO-SR' ($panelX + 16) 6 22 }
$lyNote = New-Layer 50 { param($lg) Draw-Text $lg 'Illustration  ·  same half-resolution input, 2x output' $famBody 28 $goldDim $cx 25 'Center' 0 0.04 | Out-Null }

$lyPEyebrow = New-Layer 70 { param($lg) Draw-Text $lg 'PTERO RECONSTRUCTION' $famCaps 38 $goldDim $cx 35 'Center' 9 0.04 | Out-Null }
$lyPTitle = New-Layer 250 {
    param($lg)
    Draw-Text $lg 'Edges rebuilt from' $famGoth 100 $parchment $cx 60 'Center' 0 0.04 | Out-Null
    Draw-Text $lg 'the geometry itself' $famGoth 100 $parchment $cx 165 'Center' 0 0.04 | Out-Null
}
function New-Caption($line1, $line2) {
    New-Layer 150 {
        param($lg)
        Draw-Text $lg $line1 $famBody 54 $parchment $cx 40 'Center' 0 0.04 | Out-Null
        Draw-Text $lg $line2 $famBody 54 $parchment $cx 108 'Center' 0 0.04 | Out-Null
    }
}
$capSamples = New-Caption 'The game renders at half resolution.' 'Each dot is one rendered sample.'
$capBlend   = New-Caption 'Most upscalers only see colours,' 'so edges smear and thin rails break up.'
$capExact   = New-Caption "Ptero-SR keeps the triangles' edges:" 'every pixel gets its exact coverage.'
$capResult  = New-Caption 'Clean, stable edges. No ghosting.' 'Plus Ptero-AA at native resolution.'
$lyWhere = New-Layer 130 {
    param($lg)
    Draw-Text $lg 'Settings  →  Upscaler  →  Ptero-SR' $famBody 54 $parchment $cx 40 'Center' 0 0.04 | Out-Null
    Draw-Rule $lg $cx 110 240 30
}

$panelY = 640
Render-Short 'ptero' 24 0.5 {
    param($g, $t)
    Draw-Pan $g $t 24 2600 1600
    Draw-Embers $g $t
    $g.DrawImage($shade, 0, 0, $W, $H)
    # The scene sinks into the dark while the diagram is up, and partly comes back for the ending.
    Fill-Black $g (0.82 * [Math]::Min((Envelope $t 3.4 0.8), (1 - 0.45 * (Envelope $t 19.2 0.8))))

    $a = Envelope $t 0.3 1.0;  Draw-Layer $g $lyLogo (270 + (& $lift $a)) $a
    $a = Envelope $t 1.3 0.7;  Draw-Layer $g $lyPEyebrow (505 + (& $lift $a)) $a
    $a = Envelope $t 1.8 0.9 3.8 0.5; Draw-Layer $g $lyPTitle (600 + (& $lift $a)) $a

    # Diagram: samples, the colour-only blend, then Ptero-SR wiping across it.
    $pa = Envelope $t 4.0 0.6 19.0 0.6
    Draw-Layer $g $lyBlend $panelY $pa
    $wipe = Smooth (($t - 11.0) / 1.6)
    if ($wipe -gt 0) {
        $g.SetClip((New-Object System.Drawing.Rectangle $panelX, $panelY, ([int]($pw * $wipe)), ($ph + 2)))
        Draw-Layer $g $lyExact $panelY $pa
        $g.ResetClip()
        if ($wipe -lt 0.999) {
            $lx = $panelX + $pw * $wipe
            $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb([int](255 * $pa), 0xF2, 0xC2, 0x6A)), 5
            $g.DrawLine($pen, [float]$lx, [float]($panelY - 12), [float]$lx, [float]($panelY + $ph + 12)); $pen.Dispose()
        }
    }
    Draw-Layer $g $lyChipBlend ($panelY + 10) ([Math]::Min($pa, (1 - (Envelope $t 11.0 0.4))))
    Draw-Layer $g $lyChipExact ($panelY + 10) ([Math]::Min($pa, (Envelope $t 12.2 0.4)))
    Draw-Layer $g $lyNote ($panelY + $ph + 18) $pa

    $capY = $panelY + $ph + 100
    $a = Envelope $t 4.4 0.5 7.6;   Draw-Layer $g $capSamples ($capY + (& $lift $a)) $a
    $a = Envelope $t 7.8 0.5 10.9;  Draw-Layer $g $capBlend ($capY + (& $lift $a)) $a
    $a = Envelope $t 11.4 0.5 15.4; Draw-Layer $g $capExact ($capY + (& $lift $a)) $a
    $a = Envelope $t 15.6 0.5 19.0; Draw-Layer $g $capResult ($capY + (& $lift $a)) $a

    $a = Envelope $t 19.6 0.7; Draw-Layer $g $lyWhere (720 + (& $lift $a)) $a
    $a = Envelope $t 20.2 0.8; Draw-Layer $g $lyCta (1050 + (& $lift $a)) $a
} @(2.5, 6, 9.5, 11.8, 14, 17, 22.5)


# =============================================================================
# Short 2: Achievements
# =============================================================================
# Only achievements the game can actually award: those GameAchievements.cpp unlocks.
$code = Get-Content $achCode -Raw
$earnable = Select-String -Path "$achDir\Achievements.md" -Pattern '\| `(ACH_[A-Z0-9_]+)`' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Where-Object { $code -match "`"$_`"" }
$count = @($earnable).Count

# Featured cards: icon, name, and the description split over two lines.
$featured = @(
    @('ACH_STRAIGHT_SHOOTER', 'Straight Shooter', 'Roll a straight:', '1, 2, 3, 4, 5 and 6.'),
    @('ACH_HOT_DICE', 'Hot Hands', 'Score with all six dice', 'and roll them all again.'),
    @('ACH_BANK_3000', 'Greed Is Good', 'Bank 3,000 points or more', 'in a single turn.'),
    @('ACH_OPENING_FARKLE', 'Opening Night Jitters', 'Farkle on the very', 'first roll of a match.'),
    @('ACH_COMEBACK', 'Comeback Kid', 'Win after trailing by', '1,500 points or more.'),
    @('ACH_STREAK_10', 'Unstoppable', 'Win 10 matches', 'in a row.'),
    @('ACH_IN_HOUSE', 'In-House', "Win using Ptero-SR, the", "engine's own upscaler."),
    @('ACH_MAXED_OUT', 'Maxed Out', 'Win with everything on Ultra', 'and path tracing on.'))
$lyCards = foreach ($c in $featured) {
    New-Layer 640 {
        param($lg)
        $icon = 340; $ix = $cx - $icon / 2
        for ($i = 6; $i -ge 1; $i--) {
            $sb = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(22, 0, 0, 0))
            $lg.FillRectangle($sb, [float]($ix - $i * 2), [float](20 - $i + 6), [float]($icon + $i * 4), [float]($icon + $i * 3)); $sb.Dispose()
        }
        $img = [System.Drawing.Image]::FromFile("$achDir\Icons\achieved\$($c[0]).png")
        $lg.DrawImage($img, [float]$ix, 20, [float]$icon, [float]$icon); $img.Dispose()
        Draw-Text $lg $c[1] $famGoth 76 $parchment $cx 435 'Center' 0 0.04 | Out-Null
        Draw-Text $lg $c[2] $famBody 46 $goldDim $cx 520 'Center' 0 0.04 | Out-Null
        Draw-Text $lg $c[3] $famBody 46 $goldDim $cx 578 'Center' 0 0.04 | Out-Null
    }
}
$lyAEyebrow = New-Layer 70 { param($lg) Draw-Text $lg 'NEW IN THE UPDATE' $famCaps 38 $goldDim $cx 35 'Center' 10 0.04 | Out-Null }
$lyATitle = New-Layer 140 { param($lg) Draw-Text $lg "$count Achievements" $famGoth 112 $parchment $cx 65 'Center' 0 0.04 | Out-Null }
$lyARule = New-Layer 60 { param($lg) Draw-Rule $lg $cx 30 240 32 }
# The whole set, as a grid of small icons that cascades in.
$gridCols = 8; $gIcon = 98; $gStep = 110
$gridX = $cx - $gridCols * $gStep / 2 + ($gStep - $gIcon) / 2
$icons = foreach ($n in $earnable) { [System.Drawing.Image]::FromFile("$achDir\Icons\achieved\$n.png") }
$lyGridCap = New-Layer 80 { param($lg) Draw-Text $lg 'Lucky rolls, bad beats and long streaks' $famBody 48 $parchment $cx 40 'Center' 0 0.04 | Out-Null }

$cardStart = 3.2; $cardStep = 1.55
$cardEnd = $cardStart + $cardStep * $featured.Count
Render-Short 'achievements' 24 20 {
    param($g, $t)
    Draw-Pan $g $t 24 1500 2620
    Draw-Embers $g $t
    $g.DrawImage($shade, 0, 0, $W, $H)
    Fill-Black $g (0.55 * (Envelope $t 2.8 0.8))

    $a = Envelope $t 0.3 1.0; Draw-Layer $g $lyLogo (250 + (& $lift $a)) $a
    $a = Envelope $t 1.2 0.7; Draw-Layer $g $lyAEyebrow (480 + (& $lift $a)) $a
    $a = Envelope $t 1.6 0.9; Draw-Layer $g $lyATitle (545 + (& $lift $a)) $a
    $a = Envelope $t 2.3 0.7; Draw-Layer $g $lyARule 690 $a

    for ($i = 0; $i -lt $featured.Count; $i++) {
        $t0 = $cardStart + $i * $cardStep
        $a = Envelope $t $t0 0.3 ($t0 + $cardStep) 0.3
        Draw-Layer $g $lyCards[$i] (780 + (& $lift $a)) $a
    }

    # Grid: each icon fades in a moment after the one before it.
    $gridOut = 1 - (Envelope $t 20.0 0.5)
    for ($k = 0; $k -lt $icons.Count; $k++) {
        $a = [Math]::Min((Envelope $t ($cardEnd + 0.15 + $k * 0.045) 0.35), $gridOut)
        if ($a -le 0.004) { continue }
        $m = New-Object System.Drawing.Imaging.ColorMatrix; $m.Matrix33 = [float]$a; $layerAttr.SetColorMatrix($m)
        $x = $gridX + ($k % $gridCols) * $gStep; $y = 790 + [Math]::Floor($k / $gridCols) * $gStep + (1 - $a) * 14
        $g.DrawImage($icons[$k], (New-Object System.Drawing.Rectangle ([int]$x), ([int]$y), $gIcon, $gIcon), 0, 0, $icons[$k].Width, $icons[$k].Height, 'Pixel', $layerAttr)
    }
    $a = [Math]::Min((Envelope $t ($cardEnd + 1.2) 0.6), $gridOut)
    Draw-Layer $g $lyGridCap (790 + [Math]::Ceiling($icons.Count / $gridCols) * $gStep + 20) $a

    $a = Envelope $t 20.5 0.8; Draw-Layer $g $lyCta (900 + (& $lift $a)) $a
} @(2.8, 4.0, 9.0, 18.5, 22.5)
foreach ($i in $icons) { $i.Dispose() }

$bg.Dispose(); $atlas.Dispose()
