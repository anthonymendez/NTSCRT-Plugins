# NTSCRT installer for DaVinci Resolve on Windows (OpenFX).
#
# One line, in PowerShell:
#   irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.ps1 | iex
#
# Or with options (save the script first, or use the scriptblock form):
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.ps1))) -Version v0.1.0
#   .\install.ps1 -From .\NTSCRT-OFX-windows-x64.zip     # install a zip / bundle you already have
#   .\install.ps1 -Uninstall
#
# What it does: downloads the release zip (or uses -From), extracts it to a temp
# folder, and copies NTSCRT.ofx.bundle into C:\Program Files\Common Files\OFX\Plugins.
# The copy asks for administrator rights (one UAC prompt). Then restart Resolve.
# Private repo / no public release yet: it falls back to `gh release download`
# (GitHub CLI, signed in) or the GITHUB_TOKEN environment variable.
# Works in Windows PowerShell 5.1 and PowerShell 7.
param(
    [string]$Version = "latest",          # release tag, e.g. v0.1.0
    [string]$From = "",                   # local .zip or NTSCRT.ofx.bundle folder instead of downloading
    [string]$Repo = $(if ($env:NTSCRT_REPO) { $env:NTSCRT_REPO } else { "anthonymendez/NTSCRT-Plugins" }),
    [string]$Dest = (Join-Path $env:CommonProgramFiles "OFX\Plugins"),   # OpenFX folder (override for testing)
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar on PS 5.1
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Asset = "NTSCRT-OFX-windows-x64.zip"
$Target = Join-Path $Dest "NTSCRT.ofx.bundle"

function Say($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
# Run a small PowerShell snippet as administrator (UAC prompt) and wait for it.
function Invoke-Elevated([string]$Script) {
    if (Test-Admin) { & ([scriptblock]::Create($Script)); return }
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; try { $Script } catch { Write-Host `$_ -ForegroundColor Red; Start-Sleep 5; exit 1 }"))
    $p = Start-Process powershell.exe -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand", $enc -Verb RunAs -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "The administrator step failed (exit $($p.ExitCode))." }
}
# Resolve keeps the plugin's DLLs open: replacing them while it runs would leave a
# half-deleted bundle. Wait for the user to quit it; refuse if it's still running.
function Wait-ResolveClosed {
    for ($i = 0; $i -lt 3; $i++) {
        if (-not (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue)) { return }
        Write-Warning "DaVinci Resolve is running. Quit it, then press Enter."
        Read-Host | Out-Null
        Start-Sleep -Seconds 2
    }
    if (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue) {
        throw "DaVinci Resolve is still running. Quit it and run the installer again."
    }
}
function Get-Release([string]$OutDir) {
    $zip = Join-Path $OutDir $Asset
    $url = if ($Version -eq "latest") { "https://github.com/$Repo/releases/latest/download/$Asset" }
           else { "https://github.com/$Repo/releases/download/$Version/$Asset" }
    Say "Downloading $Asset ($Version) from $Repo"
    try {
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
        return $zip
    } catch { Write-Host "    direct download failed: $($_.Exception.Message)" }
    if (Get-Command gh -ErrorAction SilentlyContinue) {
        Say "Trying GitHub CLI (gh release download)"
        $ghArgs = @("release", "download", "--repo", $Repo, "--pattern", $Asset, "--dir", $OutDir, "--clobber")
        if ($Version -ne "latest") { $ghArgs = @("release", "download", $Version) + $ghArgs[2..($ghArgs.Length - 1)] }
        & gh @ghArgs
        if ($LASTEXITCODE -eq 0 -and (Test-Path $zip)) { return $zip }
    }
    if ($env:GITHUB_TOKEN) {
        Say "Trying the GitHub API with GITHUB_TOKEN"
        $h = @{ Authorization = "Bearer $env:GITHUB_TOKEN"; "User-Agent" = "ntscrt-installer" }
        $rel = if ($Version -eq "latest") { "latest" } else { "tags/$Version" }
        $info = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/$rel" -Headers $h
        $a = $info.assets | Where-Object { $_.name -eq $Asset } | Select-Object -First 1
        if ($a) {
            $h.Accept = "application/octet-stream"
            Invoke-WebRequest -Uri $a.url -Headers $h -OutFile $zip -UseBasicParsing
            return $zip
        }
    }
    throw "Couldn't download $Asset. Check that a release with that file exists at https://github.com/$Repo/releases, or pass -From <zip>."
}

if ($Uninstall) {
    if (Test-Path $Target) {
        Wait-ResolveClosed
        Say "Removing $Target"
        Invoke-Elevated "Remove-Item -Recurse -Force -Confirm:`$false '$Target'"
    } else { Say "Not installed ($Target doesn't exist)" }
    Say "Done."
    return
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("ntscrt-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $src = $From
    if (-not $src) { $src = Get-Release $work }
    $src = (Resolve-Path $src).Path
    if ((Get-Item $src).PSIsContainer) {
        $bundle = $src
    } else {
        Say "Extracting"
        $x = Join-Path $work "x"
        Expand-Archive -Path $src -DestinationPath $x -Force
        # GitHub Actions artifacts wrap the zip in another zip: unwrap one level.
        $inner = Get-ChildItem $x -Filter "*.zip" -File | Select-Object -First 1
        if ($inner -and -not (Get-ChildItem $x -Recurse -Directory -Filter "NTSCRT.ofx.bundle")) {
            Expand-Archive -Path $inner.FullName -DestinationPath (Join-Path $x "inner") -Force
        }
        $found = Get-ChildItem $x -Recurse -Directory -Filter "NTSCRT.ofx.bundle" | Select-Object -First 1
        if (-not $found) { throw "No NTSCRT.ofx.bundle inside $src" }
        $bundle = $found.FullName
    }
    if (-not (Test-Path (Join-Path $bundle "Contents\Win64\NTSCRT.ofx"))) {
        throw "Not a Windows NTSCRT bundle: $bundle (expected Contents\Win64\NTSCRT.ofx)"
    }
    Get-ChildItem -Recurse $bundle | Unblock-File -ErrorAction SilentlyContinue

    Wait-ResolveClosed
    Say "Installing to $Target (administrator prompt)"
    Invoke-Elevated ("New-Item -ItemType Directory -Force -Path '$Dest' | Out-Null; " +
                     "if (Test-Path '$Target') { Remove-Item -Recurse -Force -Confirm:`$false '$Target' }; " +
                     "Copy-Item -Recurse -Force '$bundle' '$Target'")
    if (-not (Test-Path (Join-Path $Target "Contents\Win64\NTSCRT.ofx"))) { throw "Install didn't complete." }
    Say "Installed. Start DaVinci Resolve: Effects > OpenFX > Filters > NTSCRT."
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $work
}
