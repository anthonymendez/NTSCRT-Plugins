# Installs NTSCRT.ofx.bundle into the system OpenFX plugin folder, where
# DaVinci Resolve looks for plugins. Run from an elevated PowerShell.
#
#   .\ofx\install-windows.ps1 -Bundle C:\ntb\E\ofx\Release\NTSCRT.ofx.bundle
#
# Restart Resolve afterwards (it scans plugins at startup).
param(
    [string]$Bundle = "C:\ntb\E\ofx\Release\NTSCRT.ofx.bundle",
    [string]$Dest = "$env:CommonProgramFiles\OFX\Plugins"
)
$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated (Administrator) PowerShell."
}
if (-not (Test-Path (Join-Path $Bundle "Contents\Win64\NTSCRT.ofx"))) {
    throw "Not a built NTSCRT bundle: $Bundle (expected Contents\Win64\NTSCRT.ofx)"
}
if (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue) {
    Write-Warning "DaVinci Resolve is running; the old plugin DLL may be locked. Quit Resolve and re-run if the copy fails."
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$target = Join-Path $Dest "NTSCRT.ofx.bundle"
if (Test-Path $target) { Remove-Item -Recurse -Force -Confirm:$false $target }
Copy-Item -Recurse -Force $Bundle $target
Write-Host "Installed $target"
