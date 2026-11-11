# Build the Windows MSI: dist\NTSCRT-Installer-windows.msi (version from VERSION).
# Needs the WiX v5 tool + extensions once:
#   dotnet tool install --global wix --version 5.0.2
#   wix extension add -g WixToolset.Util.wixext/5.0.2 WixToolset.UI.wixext/5.0.2
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $Out) { $Out = Join-Path $root "dist\NTSCRT-Installer-windows.msi" }
$version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
# MSI versions are numeric: a release candidate X.Y.Z-rc.N builds as X.Y.Z (see NTSCRT.wxs).
if ($version -notmatch '^(\d+\.\d+\.\d+)(-rc\.\d+)?$') { throw "VERSION must be X.Y.Z or X.Y.Z-rc.N, got '$version'" }
$msiVersion = $Matches[1]
New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
& wix build (Join-Path $PSScriptRoot "NTSCRT.wxs") -arch x64 -d "Version=$msiVersion" `
    -ext WixToolset.Util.wixext -ext WixToolset.UI.wixext -o $Out
if ($LASTEXITCODE -ne 0) { throw "wix build failed (exit $LASTEXITCODE)" }
Write-Host "Built $Out (version $version, MSI version $msiVersion)"
