# install.ps1's shader warm-up step, against a built bundle (or release zip): it fills a
# cache folder of its own, and every way it can go wrong (a cache folder that can't be
# written, no warm-up tool in the bundle, the time limit, a tool that fails) is one line,
# never a failed install. Installs into temporary folders only (-Dest), never elevated, and
# never touches the user's own shader cache (NTSCRT_SHADER_CACHE is set for every run).
#
#   pwsh tests/installer/test_install_warmup.ps1 -From <NTSCRT.ofx.bundle | NTSCRT-OFX-windows-x64.zip>
param([Parameter(Mandatory = $true)][string]$From)
$ErrorActionPreference = "Stop"
$installer = Join-Path $PSScriptRoot "..\..\install.ps1"
$work = Join-Path ([IO.Path]::GetTempPath()) ("ntscrt-warmup-test-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$savedCache = $env:NTSCRT_SHADER_CACHE
$savedWarm = $env:NTSCRT_SHADER_WARMUP
$failures = @()

# One install into a new folder; returns its console output. A thrown install is a failure.
function Invoke-Install([string]$Name, [string]$Source, [string]$Cache, [hashtable]$Extra = @{}) {
    $dest = Join-Path $work "dest-$Name"
    $env:NTSCRT_SHADER_CACHE = $Cache
    # 1: also from an administrator PowerShell (CI runners are elevated with UAC on).
    $env:NTSCRT_SHADER_WARMUP = "1"
    $out = & $installer -From $Source -Dest $dest @Extra *>&1 | Out-String
    Write-Host "---- $Name ----"
    Write-Host $out
    if (-not (Test-Path (Join-Path $dest "NTSCRT.ofx.bundle\Contents\Win64\NTSCRT.ofx"))) { $script:failures += "${Name}: not installed" }
    return $out
}
function Test-Has([string]$Name, [string]$Out, [string]$Pattern) {
    if ($Out -notmatch $Pattern) { $script:failures += "${Name}: no line matching '$Pattern'" }
}

try {
    # A folder of the bundle to edit (the warm-up tool removed / replaced).
    $src = (Resolve-Path $From).Path
    if (-not (Get-Item $src).PSIsContainer) {
        Expand-Archive -Path $src -DestinationPath (Join-Path $work "x") -Force
        $src = (Get-ChildItem (Join-Path $work "x") -Recurse -Directory -Filter "NTSCRT.ofx.bundle" | Select-Object -First 1).FullName
    }

    # 1. Warm-up into an empty folder: every preset built, warm.txt written.
    $cache = Join-Path $work "cache"
    $out = Invoke-Install "warm" $src $cache
    Test-Has "warm" $out "presets ready in"
    Test-Has "warm" $out "CRT shaders ready"
    if (-not (Get-ChildItem $cache -Recurse -Filter warm.txt -ErrorAction SilentlyContinue)) { $failures += "warm: no warm.txt in $cache" }

    # 2. A cache folder that can't be made (a file in the way): one line, installed.
    $blocker = Join-Path $work "blocker"
    Set-Content -LiteralPath $blocker -Value "not a folder"
    $out = Invoke-Install "unwritable" $src (Join-Path $blocker "cache")
    Test-Has "unwritable" $out "shader cache off"
    Test-Has "unwritable" $out "Shader warm-up didn't run"
    if ((Get-Content -LiteralPath $blocker) -ne "not a folder") { $failures += "unwritable: the file in the way changed" }

    # 3. The time limit.
    $out = Invoke-Install "timeout" $src (Join-Path $work "cache-timeout") @{ WarmupTimeout = 0.001 }
    Test-Has "timeout" $out "stopped at its time limit"

    # 4. -NoWarmup and NTSCRT_SHADER_WARMUP=0.
    $out = Invoke-Install "nowarmup" $src (Join-Path $work "cache-none") @{ NoWarmup = $true }
    Test-Has "nowarmup" $out "Shader warm-up skipped \(-NoWarmup\)"
    if (Test-Path (Join-Path $work "cache-none")) { $failures += "nowarmup: a cache folder was made" }

    # 5. A bundle without the tool (releases before it), and a tool that fails.
    $old = Join-Path $work "old\NTSCRT.ofx.bundle"
    Copy-Item -LiteralPath $src -Destination $old -Recurse
    Remove-Item -LiteralPath (Join-Path $old "Contents\Win64\ntscrt-warm.exe")
    $out = Invoke-Install "notool" $old (Join-Path $work "cache-notool")
    Test-Has "notool" $out "this version has no warm-up tool"
    $bad = Join-Path $work "bad\NTSCRT.ofx.bundle"
    Copy-Item -LiteralPath $src -Destination $bad -Recurse
    # Any program that exits non-zero with these arguments stands in for a failing tool.
    Copy-Item -LiteralPath (Join-Path $env:SystemRoot "System32\whoami.exe") -Destination (Join-Path $bad "Contents\Win64\ntscrt-warm.exe") -Force
    $out = Invoke-Install "badtool" $bad (Join-Path $work "cache-bad")
    Test-Has "badtool" $out "Shader warm-up (incomplete|didn't run)"
} finally {
    $env:NTSCRT_SHADER_CACHE = $savedCache
    $env:NTSCRT_SHADER_WARMUP = $savedWarm
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
if ($failures.Count) {
    $failures | ForEach-Object { Write-Host "FAILED: $_" -ForegroundColor Red }
    exit 1
}
Write-Host "install.ps1 shader warm-up: all cases OK"
