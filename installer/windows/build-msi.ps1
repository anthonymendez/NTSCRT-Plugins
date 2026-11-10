# Build the Windows MSI: dist\NTSCRT-Installer-windows.msi (version from VERSION).
# Needs the WiX v5 tool + extensions once:
#   dotnet tool install --global wix --version 5.0.2
#   wix extension add -g WixToolset.Util.wixext/5.0.2 WixToolset.UI.wixext/5.0.2
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if (-not $Out) { $Out = Join-Path $root "dist\NTSCRT-Installer-windows.msi" }
$version = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
& wix build (Join-Path $PSScriptRoot "NTSCRT.wxs") -arch x64 -d "Version=$version" `
    -ext WixToolset.Util.wixext -ext WixToolset.UI.wixext -o $Out
if ($LASTEXITCODE -ne 0) { throw "wix build failed (exit $LASTEXITCODE)" }
Write-Host "Built $Out (version $version)"
