# Regenerates the README preview images in docs/images/.
#
#   powershell.exe -NoProfile -File scripts/make_previews.ps1 [-Cli path\to\ntscrt-cli.exe] [-Resources DIR]
#
# Draws a synthetic test scene (no third-party footage), renders it through ntscrt-cli with a few looks,
# and composites a before/after hero image plus a labelled grid of looks (JPEG, 960 px wide).
param(
    [string]$Cli = 'C:/ntb/main/tools/ntscrt-cli/Release/ntscrt-cli.exe',
    [string]$Resources = 'C:/ntb/main/ofx/Release/NTSCRT.ofx.bundle/Contents/Resources',
    [string]$OutDir = '',
    [string]$WorkDir = '',
    [int]$Quality = 85
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $repo 'docs/images' }
if (-not $WorkDir) { $WorkDir = Join-Path ([IO.Path]::GetTempPath()) 'ntscrt-previews' }
New-Item -ItemType Directory -Force $OutDir, $WorkDir | Out-Null

$W = 960; $H = 540

function New-Color([string]$hex, [int]$a = 255) {
    $c = [Drawing.ColorTranslator]::FromHtml($hex); [Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B)
}

# ---- 1. Source scene ---------------------------------------------------------------------------
function New-Scene([string]$path) {
    $bmp = [Drawing.Bitmap]::new($W, $H)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.TextRenderingHint = 'AntiAliasGridFit'
    $horizon = 330

    # sky
    $sky = [Drawing.Drawing2D.LinearGradientBrush]::new([Drawing.Point]::new(0, 0), [Drawing.Point]::new(0, $horizon),
        (New-Color '#0b0420'), (New-Color '#ff5e62'))
    $blend = [Drawing.Drawing2D.ColorBlend]::new(3)
    $blend.Colors = @((New-Color '#0b0420'), (New-Color '#5b1a73'), (New-Color '#ff7b54'))
    $blend.Positions = @(0.0, 0.55, 1.0)
    $sky.InterpolationColors = $blend
    $g.FillRectangle($sky, [Drawing.Rectangle]::new(0, 0, $W, $horizon + 1))

    # stars
    $rnd = [Random]::new(7)
    for ($i = 0; $i -lt 90; $i++) {
        $x = $rnd.Next(0, $W); $y = $rnd.Next(0, 200); $s = $rnd.Next(1, 3)
        $g.FillRectangle([Drawing.Brushes]::White, [Drawing.Rectangle]::new($x, $y, $s, $s))
    }

    # striped sun
    $sunR = 150; $cx = 480; $cy = $horizon - 40
    $sunRect = [Drawing.Rectangle]::new($cx - $sunR, $cy - $sunR, 2 * $sunR, 2 * $sunR)
    $sun = [Drawing.Drawing2D.LinearGradientBrush]::new($sunRect, (New-Color '#ffe86b'), (New-Color '#ff2e88'), 90.0)
    $clip = [Drawing.Drawing2D.GraphicsPath]::new(); $clip.AddEllipse($sunRect)
    $g.SetClip($clip)
    $g.FillEllipse($sun, $sunRect)
    $y = $cy; $t = 3
    while ($y -lt $cy + $sunR) {
        $g.FillRectangle($sky, [Drawing.Rectangle]::new(0, $y, $W, $t)); $y += 18; $t += 2
    }
    $g.ResetClip()

    # mountains
    $mtn = [Drawing.SolidBrush]::new((New-Color '#2a0a3d'))
    $pts = @(@(0, $horizon), @(0, 270), @(90, 230), @(170, 285), @(250, 245), @(330, $horizon), @(620, $horizon),
        @(700, 250), @(780, 290), @(860, 220), @(960, 265), @(960, $horizon)) |
        ForEach-Object { [Drawing.Point]::new($_[0], $_[1]) }
    $g.FillPolygon($mtn, [Drawing.Point[]]$pts)

    # ground + perspective grid
    $g.FillRectangle([Drawing.SolidBrush]::new((New-Color '#12001f')), [Drawing.Rectangle]::new(0, $horizon, $W, $H - $horizon))
    $pen = [Drawing.Pen]::new((New-Color '#ff3cac'), 2)
    for ($k = -12; $k -le 12; $k++) { $g.DrawLine($pen, 480 + $k * 12, $horizon, 480 + $k * 140, $H) }
    $d = 4.0
    while ($true) {
        $yy = $horizon + [int](2600 / (14 - $d)); if ($yy -ge $H) { break }
        $g.DrawLine($pen, 0, $yy, $W, $yy); $d += 1.4
    }
    $g.DrawLine([Drawing.Pen]::new((New-Color '#ffb3e6'), 2), 0, $horizon, $W, $horizon)

    # title
    $font = [Drawing.Font]::new('Arial Black', 84, [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)
    $fmt = [Drawing.StringFormat]::new(); $fmt.Alignment = 'Center'
    $g.DrawString('NTSCRT', $font, [Drawing.SolidBrush]::new((New-Color '#ff2e88')), [Drawing.RectangleF]::new(6, 38, $W, 120), $fmt)
    $g.DrawString('NTSCRT', $font, [Drawing.SolidBrush]::new((New-Color '#5ef2ff')), [Drawing.RectangleF]::new(0, 32, $W, 120), $fmt)

    # VCR on-screen display
    $osd = [Drawing.Font]::new('Consolas', 34, [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)
    $g.DrawString([string][char]0x25B6 + ' PLAY', $osd, [Drawing.Brushes]::White, 34, 28)
    $g.DrawString('CH 03', $osd, [Drawing.Brushes]::White, 810, 28)

    # colour bars (bottom right)
    $bars = '#c0c0c0', '#c0c000', '#00c0c0', '#00c000', '#c000c0', '#c00000', '#0000c0'
    for ($b = 0; $b -lt $bars.Count; $b++) {
        $g.FillRectangle([Drawing.SolidBrush]::new((New-Color $bars[$b])), [Drawing.Rectangle]::new(756 + $b * 26, 474, 26, 40))
    }

    $g.Dispose(); $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

# ---- 2. Renders ----------------------------------------------------------------------------------
function Write-Look([string]$name, [string]$json) {
    $p = Join-Path $WorkDir "$name.json"; [IO.File]::WriteAllText($p, $json); $p
}

function Invoke-Render([string]$src, [string]$out, [string]$look, [int]$frame = 0) {
    $a = @('--in', $src, '--out', $out, '--resources', $Resources, '--fps', '30', '--frame', "$frame")
    if ($look) { $a += @('--look', $look) }
    & $Cli @a
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out)) { throw "ntscrt-cli failed for $out" }
}

# ---- 3. Compositing ------------------------------------------------------------------------------
function Get-Encoder {
    $enc = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object MimeType -eq 'image/jpeg'
    $ep = [Drawing.Imaging.EncoderParameters]::new(1)
    $ep.Param[0] = [Drawing.Imaging.EncoderParameter]::new([Drawing.Imaging.Encoder]::Quality, [long]$Quality)
    , @($enc, $ep)
}

function Add-Label($g, [string]$text, [int]$x, [int]$y, [int]$size = 20) {
    $font = [Drawing.Font]::new('Segoe UI', $size, [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)
    $sz = $g.MeasureString($text, $font)
    $pad = [int]($size * 0.45)
    $rect = [Drawing.Rectangle]::new($x, $y, [int]$sz.Width + 2 * $pad, [int]$sz.Height + $pad)
    $g.FillRectangle([Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(185, 0, 0, 0)), $rect)
    $g.DrawString($text, $font, [Drawing.Brushes]::White, $x + $pad, $y + [int]($pad / 2))
}

function Save-Jpeg($bmp, [string]$path) {
    $e = Get-Encoder; $bmp.Save($path, $e[0], $e[1])
}

# Hero: left half original, right half processed, split line + labels.
function New-Hero([string]$before, [string]$after, [string]$path) {
    $b = [Drawing.Image]::FromFile($before); $a = [Drawing.Image]::FromFile($after)
    $canvas = [Drawing.Bitmap]::new($W, $H)
    $g = [Drawing.Graphics]::FromImage($canvas)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAliasGridFit'
    $half = $W / 2
    $g.DrawImage($b, [Drawing.Rectangle]::new(0, 0, $half, $H), [Drawing.Rectangle]::new(0, 0, $b.Width / 2, $b.Height), 'Pixel')
    $g.DrawImage($a, [Drawing.Rectangle]::new($half, 0, $half, $H), [Drawing.Rectangle]::new($a.Width / 2, 0, $a.Width / 2, $a.Height), 'Pixel')
    $g.FillRectangle([Drawing.Brushes]::White, [Drawing.Rectangle]::new($half - 2, 0, 4, $H))
    Add-Label $g 'Original' 16 ($H - 56) 24
    Add-Label $g 'NTSCRT (default look)' ($half + 16) ($H - 56) 24
    $g.Dispose(); $b.Dispose(); $a.Dispose()
    Save-Jpeg $canvas $path; $canvas.Dispose()
}

# Grid: tiles of 480x270 (2 columns), each labelled.
function New-Grid([object[]]$tiles, [string]$path) {
    $cols = 2; $tw = $W / $cols; $th = $H / 2; $gap = 4
    $rows = [Math]::Ceiling($tiles.Count / $cols)
    $canvas = [Drawing.Bitmap]::new($W, $rows * $th + ($rows - 1) * $gap)
    $g = [Drawing.Graphics]::FromImage($canvas)
    $g.Clear([Drawing.Color]::White)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAliasGridFit'
    for ($i = 0; $i -lt $tiles.Count; $i++) {
        $img = [Drawing.Image]::FromFile($tiles[$i].Path)
        $x = ($i % $cols) * ($tw + $gap / 2) ; $y = [Math]::Floor($i / $cols) * ($th + $gap)
        $dst = [Drawing.Rectangle]::new([int]$x, [int]$y, [int]($tw - $gap / 2), [int]$th)
        if ($tiles[$i].Crop) {
            # close-up: a 240x135 source region at 2x, nearest neighbour so the shader's mask stays crisp
            $cr = $tiles[$i].Crop
            $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
            $g.DrawImage($img, $dst, [Drawing.Rectangle]::new($cr[0], $cr[1], $dst.Width / 2, $dst.Height / 2), 'Pixel')
            $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'Default'
        } else {
            $g.DrawImage($img, $dst)
        }
        Add-Label $g $tiles[$i].Label ([int]$x + 10) ([int]$y + $th - 40) 18
        $img.Dispose()
    }
    $g.Dispose()
    Save-Jpeg $canvas $path; $canvas.Dispose()
}

# ---- main ----------------------------------------------------------------------------------------
$src = Join-Path $WorkDir 'scene.png'
New-Scene $src

$presets = Join-Path $repo 'presets'
$looks = @(
    @{ Name = 'default';    Label = 'Default look';               Look = (Write-Look 'default' '{}'); Frame = 0 },
    @{ Name = 'vhold';      Label = 'V Hold glitch';              Look = (Join-Path $presets 'V Hold glitch.json'); Frame = 45 },
    @{ Name = 'obliterated'; Label = 'Obliterated';               Look = (Join-Path $presets 'Obliterated.json'); Frame = 30 },
    @{ Name = 'webcam';     Label = 'Webcam (low quality)';       Look = (Join-Path $presets 'Webcam (low quality).json'); Frame = 30 },
    @{ Name = 'royale';     Label = 'CRT Royale shader (2x close-up)'; Look = (Write-Look 'royale' '{"shader": {"enabled": true, "preset": "royale"}}'); Frame = 0; Crop = @(300, 20) }
)
foreach ($l in $looks) {
    $l.Path = Join-Path $WorkDir "$($l.Name).png"
    Invoke-Render $src $l.Path $l.Look $l.Frame
}

New-Hero $src $looks[0].Path (Join-Path $OutDir 'hero-before-after.jpg')
New-Grid $looks[1..4] (Join-Path $OutDir 'looks-grid.jpg')
Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize
