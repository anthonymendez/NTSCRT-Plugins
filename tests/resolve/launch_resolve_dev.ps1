# Start DaVinci Resolve with a development build of the NTSCRT OpenFX plug-in,
# without installing it (no administrator rights needed).
#
#   .\tests\resolve\launch_resolve_dev.ps1 [-Bundle C:\ntb\main\ofx\Release\NTSCRT.ofx.bundle]
#
# Copies the bundle to %TEMP%\ntscrt-ofx-dev (so rebuilding doesn't fight Resolve
# over the loaded DLL), points OFX_PLUGIN_PATH at it, sets NTSCRT_OFX_LOG (render
# failure reasons) and starts Resolve. Resolve must be closed first. An installed
# copy in the OFX folder still loads too; the newest version wins.
param(
    [string]$Bundle = (Join-Path $PSScriptRoot "..\..\build\ofx\Release\NTSCRT.ofx.bundle"),
    [string]$Resolve = "C:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe"
)
$ErrorActionPreference = "Stop"
if (Get-Process -Name Resolve -ErrorAction SilentlyContinue) { throw "Quit DaVinci Resolve first." }
if (-not (Test-Path (Join-Path $Bundle "Contents\Win64\NTSCRT.ofx"))) { throw "No NTSCRT bundle at $Bundle (build it, or pass -Bundle)." }
$dir = Join-Path ([IO.Path]::GetTempPath()) "ntscrt-ofx-dev"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory $dir | Out-Null
Copy-Item -Recurse $Bundle $dir
$env:OFX_PLUGIN_PATH = $dir
$env:NTSCRT_OFX_LOG = Join-Path $dir "ntscrt-ofx.log"
$v = Get-Content (Join-Path $dir "NTSCRT.ofx.bundle\Contents\Resources\VERSION") -ErrorAction SilentlyContinue
Write-Host "Starting Resolve with NTSCRT $v from $dir (log: $env:NTSCRT_OFX_LOG)"
Start-Process $Resolve
