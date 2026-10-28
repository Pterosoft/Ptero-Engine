# Renders every achievement icon into ..\Icons\achieved and ..\Icons\unachieved (256 x 256 PNG)
# and rewrites ..\Achievements.md / .csv. Needs Node and Microsoft Edge.
$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$build = Join-Path $tools 'build'
$icons = Join-Path (Split-Path $tools) 'Icons'

node (Join-Path $tools 'make-icons.mjs'); if ($LASTEXITCODE) { throw 'make-icons failed' }
node (Join-Path $tools 'make-docs.mjs'); if ($LASTEXITCODE) { throw 'make-docs failed' }
$layout = Get-Content (Join-Path $build 'layout.json') -Raw | ConvertFrom-Json

$edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$sheet = Join-Path $build 'sheet.png'
Remove-Item $sheet -ErrorAction SilentlyContinue
$page = ([Uri](Join-Path $build 'sheet.html')).AbsoluteUri
# Fonts and the atlas are local files, so the page needs file access; its own profile keeps
# a running Edge out of it.
$arguments = @('--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
    '--allow-file-access-from-files', '--virtual-time-budget=8000', "--user-data-dir=$build\edge-profile",
    "--window-size=$($layout.width),$($layout.height)", "--screenshot=$sheet", $page)
Start-Process $edge -ArgumentList $arguments -Wait -NoNewWindow
if (-not (Test-Path $sheet)) { throw 'Edge did not write the sheet' }

Add-Type -AssemblyName System.Drawing
$source = [System.Drawing.Bitmap]::FromFile($sheet)
$size = [int]$layout.tile
try {
    foreach ($tile in $layout.tiles) {
        $target = Join-Path $icons $tile.file
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        $bitmap = New-Object System.Drawing.Bitmap $size, $size
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.DrawImage($source, (New-Object System.Drawing.Rectangle 0, 0, $size, $size),
            (New-Object System.Drawing.Rectangle ([int]$tile.x), ([int]$tile.y), $size, $size), [System.Drawing.GraphicsUnit]::Pixel)
        $graphics.Dispose()
        $bitmap.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
    }
} finally { $source.Dispose() }
"Wrote $($layout.tiles.Count) icons to $icons"
