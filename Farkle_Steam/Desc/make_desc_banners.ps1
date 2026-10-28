# Renders the localized store-description banners (984x226) into Desc\<lang>\.
# Matches the English set in Desc\en: the "wood" sprite from the Farkle UI atlas
# with the heading in flat #C9A466, Pirata One, cap height 61px, baseline y=138.
param([string[]]$Lang = @(), [string]$OutRoot = $PSScriptRoot)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$atlasPath = 'K:\Farkle\Data\UI\Farkle\Textures\farkle-atlas.png'
$uiFonts   = 'K:\Farkle\Data\UI\Farkle\Fonts'
$woodSrc   = New-Object System.Drawing.Rectangle 534, 356, 984, 226
$textColor = [System.Drawing.Color]::FromArgb(255, 0xC9, 0xA4, 0x66)
$W = 984; $H = 226
$centerX = 487        # horizontal centre of the English headings
$capTop = 77; $baseline = 138
$maxTextWidth = 755   # long English headings are shrunk to ~755px

$order = 'about', 'roll', 'gameplay', 'tavern', 'learn', 'features'

$headings = [ordered]@{
    'en'    = 'About This Game', 'Roll. Score. Risk It All.', 'Classic Farkle Gameplay', 'A Medieval Tavern Brought to Life', 'Simple to Learn. Difficult to Resist.', 'Key Features'
    'de'    = 'Über dieses Spiel', 'Würfeln. Punkten. Alles riskieren.', 'Klassisches Farkle-Spielprinzip', 'Eine mittelalterliche Taverne voller Leben', 'Schnell gelernt. Schwer zu widerstehen.', 'Hauptmerkmale'
    'pl'    = 'O grze', 'Rzucaj. Punktuj. Zaryzykuj wszystko.', 'Klasyczna rozgrywka w Farkle', 'Średniowieczna karczma pełna życia', 'Proste zasady. Trudno się oderwać.', 'Najważniejsze cechy'
    'fr'    = 'À propos du jeu', 'Lancez. Marquez. Risquez tout.', 'Le Farkle dans la pure tradition', 'Une taverne médiévale pleine de vie', 'Facile à apprendre. Difficile à lâcher.', 'Caractéristiques principales'
    'it'    = 'Informazioni sul gioco', 'Lancia. Segna. Rischia tutto.', 'Il classico gameplay di Farkle', 'Una taverna medievale piena di vita', 'Facile da imparare. Impossibile resistere.', 'Caratteristiche principali'
    'es'    = 'Acerca de este juego', 'Lanza. Puntúa. Arriésgalo todo.', 'La jugabilidad clásica de Farkle', 'Una taberna medieval llena de vida', 'Fácil de aprender. Difícil de resistir.', 'Características principales'
    'pt-br' = 'Sobre este jogo', 'Role. Pontue. Arrisque tudo.', 'A jogabilidade clássica de Farkle', 'Uma taverna medieval cheia de vida', 'Fácil de aprender. Difícil de resistir.', 'Principais características'
    'pt-pt' = 'Sobre este jogo', 'Lança. Pontua. Arrisca tudo.', 'A jogabilidade clássica de Farkle', 'Uma taberna medieval cheia de vida', 'Fácil de aprender. Difícil de resistir.', 'Principais características'
    'el'    = 'Σχετικά με το παιχνίδι', 'Ρίξε. Σκόραρε. Ρίσκαρε τα όλα.', 'Κλασικό παιχνίδι Farkle', 'Μια μεσαιωνική ταβέρνα γεμάτη ζωή', 'Μαθαίνεται εύκολα. Δύσκολα αφήνεται.', 'Βασικά χαρακτηριστικά'
    'cs'    = 'O této hře', 'Házej. Boduj. Riskni všechno.', 'Klasická hra Farkle', 'Středověká krčma plná života', 'Snadno se naučíš. Těžko odoláš.', 'Hlavní vlastnosti'
    'tr'    = 'Bu Oyun Hakkında', 'At. Puanla. Her Şeyi Riske At.', 'Klasik Farkle Oynanışı', 'Hayat Bulan Bir Orta Çağ Meyhanesi', 'Öğrenmesi Kolay. Bırakması Zor.', 'Öne Çıkan Özellikler'
    'uk'    = 'Про гру', 'Кидай. Набирай очки. Ризикуй усім.', 'Класичний ігролад Farkle', 'Середньовічна таверна, сповнена життя', 'Легко навчитися. Важко зупинитися.', 'Ключові особливості'
    'ar'    = 'حول هذه اللعبة', 'ارمِ. سجّل. خاطر بكل شيء.', 'أسلوب لعب Farkle الكلاسيكي', 'حانة من العصور الوسطى تنبض بالحياة', 'سهلة التعلّم. يصعب مقاومتها.', 'الميزات الرئيسية'
    'zh-cn' = '关于这款游戏', '掷骰。得分。孤注一掷。', '经典 Farkle 玩法', '栩栩如生的中世纪酒馆', '易于上手，欲罢不能。', '主要特色'
    'ja'    = 'ゲームについて', '振れ。稼げ。すべてを賭けろ。', '王道のFarkleゲームプレイ', '息づく中世の酒場', '覚えるのは簡単。やめるのは難しい。', '主な特徴'
    'ko'    = '게임 정보', '굴려라. 점수를 쌓아라. 모든 것을 걸어라.', '정통 Farkle 게임플레이', '생생하게 되살아난 중세 선술집', '배우기는 쉽고, 멈추기는 어렵다.', '주요 특징'
}

# Per-script font. Pirata One (the English font) covers every Latin-script language here.
# Others: size = em size; capHeight = target ink height for vertical centring; embolden = extra stroke width.
$styles = @{
    'latin' = @{ File = "$uiFonts\Pirata_One\PirataOne-Regular.ttf"; Family = 'Pirata One' }
    # The game's EB Garamond statics are CFF-flavoured and GDI+ silently falls back to Times, so use Palatino.
    'uk'    = @{ File = 'C:\Windows\Fonts\palab.ttf'; Family = 'Palatino Linotype'; Size = 80; Embolden = 0; Style = 'Bold' }
    'el'    = @{ File = 'C:\Windows\Fonts\palab.ttf'; Family = 'Palatino Linotype'; Size = 80; Embolden = 0; Style = 'Bold' }
    'ar'    = @{ File = 'C:\Windows\Fonts\timesbd.ttf'; Family = 'Times New Roman'; Size = 80; Embolden = 0; Rtl = $true; Style = 'Bold' }
    'zh-cn' = @{ File = 'C:\Windows\Fonts\simkai.ttf'; Family = 'KaiTi'; Size = 72; Embolden = 2.2 }
    'ja'    = @{ File = 'C:\Windows\Fonts\YuGothB.ttc'; Family = 'Yu Gothic'; Size = 62; Embolden = 0; Style = 'Bold' }
    'ko'    = @{ File = 'C:\Windows\Fonts\malgunbd.ttf'; Family = 'Malgun Gothic'; Size = 62; Embolden = 0; Style = 'Bold' }
}

$fonts = New-Object System.Drawing.Text.PrivateFontCollection
$fonts.AddFontFile($styles.latin.File)
function Get-Family($style) {
    $f = $fonts.Families | Where-Object Name -eq $style.Family | Select-Object -First 1
    if ($f) { return $f }
    return New-Object System.Drawing.FontFamily $style.Family   # installed system font
}

$atlas = [System.Drawing.Image]::FromFile($atlasPath)

function New-TextPath($text, $family, $size, $fontStyle, $rtl) {
    $sf = [System.Drawing.StringFormat]::GenericTypographic.Clone()
    $sf.FormatFlags = $sf.FormatFlags -bor [System.Drawing.StringFormatFlags]::NoWrap
    if ($rtl) { $sf.FormatFlags = $sf.FormatFlags -bor [System.Drawing.StringFormatFlags]::DirectionRightToLeft }
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddString($text, $family, [int]$fontStyle, [float]$size, (New-Object System.Drawing.PointF 0, 0), $sf)
    return $path
}

function Move-Path($path, $dx, $dy) {
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate([float]$dx, [float]$dy)
    $path.Transform($m)
}

function Fit-Width($path, $cx, $cy) {
    $b = $path.GetBounds()
    if ($b.Width -le $maxTextWidth) { return }
    $s = $maxTextWidth / $b.Width
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate([float]-$cx, [float]-$cy); $path.Transform($m)
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Scale([float]$s, [float]$s); $path.Transform($m)
    $m = New-Object System.Drawing.Drawing2D.Matrix
    $m.Translate([float]$cx, [float]$cy); $path.Transform($m)
}

# Calibrate Pirata One against the English art: find the em size whose "H" cap height is 61px
# and where the baseline sits relative to the path origin.
$latinFamily = Get-Family $styles.latin
$probe = New-TextPath 'H' $latinFamily 100 0 $false
$pb = $probe.GetBounds()
# 1.054: measured against Desc\en\features.png (Pirata's "H" is a touch taller than its other capitals).
$latinSize = 100 * ($baseline - $capTop) / $pb.Height * 1.054
$probe = New-TextPath 'H' $latinFamily $latinSize 0 $false
$latinBaselineOffset = $probe.GetBounds().Bottom
$probe.Dispose()

function Render-Banner($text, $lang, $outFile) {
    $bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'; $g.CompositingMode = 'SourceCopy'
    $g.DrawImage($atlas, (New-Object System.Drawing.Rectangle 0, 0, $W, $H), $woodSrc, 'Pixel')
    $g.CompositingMode = 'SourceOver'

    $style = if ($styles.ContainsKey($lang)) { $styles[$lang] } else { $styles.latin }
    $family = Get-Family $style
    $brush = New-Object System.Drawing.SolidBrush $textColor
    $midY = ($capTop + $baseline) / 2

    if ($style -eq $styles.latin) {
        $path = New-TextPath $text $family $latinSize 0 $false
        $b = $path.GetBounds()
        Move-Path $path ($centerX - ($b.Left + $b.Width / 2)) ($baseline + 2 - $latinBaselineOffset)
    } else {
        $fs = if ($style.Style -eq 'Bold') { 1 } else { 0 }
        $path = New-TextPath $text $family $style.Size $fs ([bool]$style.Rtl)
        $b = $path.GetBounds()
        Move-Path $path ($centerX - ($b.Left + $b.Width / 2)) ($midY - ($b.Top + $b.Height / 2))
    }
    Fit-Width $path $centerX $midY

    $g.FillPath($brush, $path)
    if ($style.Embolden -gt 0) {
        $pen = New-Object System.Drawing.Pen $textColor, ([float]$style.Embolden)
        $pen.LineJoin = 'Round'
        $g.DrawPath($pen, $path); $pen.Dispose()
    }
    $path.Dispose(); $brush.Dispose(); $g.Dispose()
    $bmp.Save($outFile, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$langs = if ($Lang.Count) { $Lang } else { $headings.Keys | Where-Object { $_ -ne 'en' } }
foreach ($l in $langs) {
    $dir = Join-Path $OutRoot $l
    New-Item -ItemType Directory -Force $dir | Out-Null
    for ($i = 0; $i -lt $order.Count; $i++) {
        $out = Join-Path $dir "$($order[$i]).png"
        Render-Banner $headings[$l][$i] $l $out
    }
    Write-Output "$l -> $dir"
}
$atlas.Dispose()
