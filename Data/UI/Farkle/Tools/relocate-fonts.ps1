# Run after moving the Farkle directory. RmlUi 6.3 font-face src is CWD-relative.
$farkleRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path.Replace('\', '/')
$stylesheet = Join-Path $farkleRoot 'farkle.rcss'
$content = [IO.File]::ReadAllText($stylesheet)
$content = [regex]::Replace($content, 'src: "[^"]*/Fonts/', ('src: "' + $farkleRoot + '/Fonts/'))
[IO.File]::WriteAllText($stylesheet, $content, [Text.UTF8Encoding]::new($false))
Write-Output "Font paths updated for $farkleRoot"
