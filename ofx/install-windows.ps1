# Installs NTSCRT.ofx.bundle into the system OpenFX folder that DaVinci Resolve
# scans at startup. Easiest: double-click "Install NTSCRT.cmd" next to this file.
#
#   powershell -ExecutionPolicy Bypass -File install-windows.ps1 [-Bundle <path>] [-Uninstall]
#
# With no -Bundle it uses the NTSCRT.ofx.bundle sitting next to this script
# (the layout of the release zip). Asks for administrator rights if needed.
# Works in Windows PowerShell 5.1 and PowerShell 7.
param(
    [string]$Bundle = (Join-Path $PSScriptRoot "NTSCRT.ofx.bundle"),
    [string]$Dest = (Join-Path $env:CommonProgramFiles "OFX\Plugins"),
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    # Re-launch elevated (UAC prompt) with the same arguments, and wait.
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
                 "-Bundle", "`"$Bundle`"", "-Dest", "`"$Dest`"")
    if ($Uninstall) { $argList += "-Uninstall" }
    $p = Start-Process -FilePath "powershell.exe" -ArgumentList $argList -Verb RunAs -Wait -PassThru
    exit $p.ExitCode
}

$target = Join-Path $Dest "NTSCRT.ofx.bundle"
try {
    if (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue) {
        Write-Warning "DaVinci Resolve is running. Quit it first if the copy fails (the plugin DLL is locked while Resolve runs)."
    }
    if ($Uninstall) {
        if (Test-Path $target) { Remove-Item -Recurse -Force -Confirm:$false $target }
        Write-Host "Removed $target"
    } else {
        if (-not (Test-Path (Join-Path $Bundle "Contents\Win64\NTSCRT.ofx"))) {
            throw "Not an NTSCRT bundle: $Bundle (expected Contents\Win64\NTSCRT.ofx). Unzip the whole download first."
        }
        New-Item -ItemType Directory -Force -Path $Dest | Out-Null
        if (Test-Path $target) { Remove-Item -Recurse -Force -Confirm:$false $target }
        Copy-Item -Recurse -Force $Bundle $target
        # Files from a downloaded zip carry the "from the internet" mark; clear it.
        Get-ChildItem -Recurse $target | Unblock-File -ErrorAction SilentlyContinue
        Write-Host "Installed $target"
        Write-Host "Restart DaVinci Resolve. The effect is under Effects > OpenFX > Filters > NTSCRT."
    }
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}
Read-Host "Press Enter to close"
