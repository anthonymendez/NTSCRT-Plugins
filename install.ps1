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
# If the direct download fails it falls back to `gh release download`
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
function Test-Writable([string]$Dir) {
    while ($Dir -and -not (Test-Path -LiteralPath $Dir)) { $Dir = Split-Path $Dir -Parent }
    if (-not $Dir) { return $false }
    $probe = Join-Path $Dir (".ntscrt-probe-" + [Guid]::NewGuid().ToString("N"))
    try { [IO.File]::WriteAllText($probe, ""); Remove-Item -LiteralPath $probe -Force; $true } catch { $false }
}
function Invoke-Elevated([string]$Script) {
    if ((Test-Admin) -or (Test-Writable $Dest)) { & ([scriptblock]::Create($Script)); return }
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes("`$ErrorActionPreference='Stop'; try { $Script } catch { Write-Host `$_ -ForegroundColor Red; Start-Sleep 5; exit 1 }"))
    $p = Start-Process powershell.exe -ArgumentList "-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand", $enc -Verb RunAs -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "The administrator step failed (exit $($p.ExitCode))." }
}
# The install / uninstall step, run as administrator. Atomic: the new bundle is
# copied next to the old one and verified, then the two folders are swapped by
# renaming, with a rollback if the swap fails. A failed or interrupted copy never
# touches the installed plugin. Windows allows renaming the folder while Resolve
# has the plugin loaded, so this is safe with Resolve open; the copy Resolve still
# holds is deleted on a later run. Folders named NTSCRT.ofx.bundle.new-* / .old-*
# don't end in .ofx.bundle, so Resolve never loads them.
$SwapScript = @'
param([string]$Mode, [string]$Bundle, [string]$Dest)
$ErrorActionPreference = 'Stop'
$name = 'NTSCRT.ofx.bundle'
$target = Join-Path $Dest $name
$tag = [Guid]::NewGuid().ToString('N').Substring(0, 8)
function Remove-Leftovers {
    Get-ChildItem -LiteralPath $Dest -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "$name.new-*" -or $_.Name -like "$name.old-*" } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Remove-Leftovers
if ($Mode -eq 'uninstall') {
    if (Test-Path -LiteralPath $target) { Rename-Item -LiteralPath $target -NewName "$name.old-$tag" }
    Remove-Leftovers
    return
}
$staging = Join-Path $Dest "$name.new-$tag"
try {
    Copy-Item -LiteralPath $Bundle -Destination $staging -Recurse -Force
    $want = @(Get-ChildItem -LiteralPath $Bundle -Recurse -File).Count
    $have = @(Get-ChildItem -LiteralPath $staging -Recurse -File).Count
    if ($have -ne $want) { throw "Copy incomplete ($have of $want files)." }
} catch {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    throw
}
$old = $null
if (Test-Path -LiteralPath $target) {
    $old = "$name.old-$tag"
    try { Rename-Item -LiteralPath $target -NewName $old }
    catch {   # the installed plugin is untouched
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
        throw "Couldn't replace the installed plugin (a file in it is in use: close DaVinci Resolve and run the installer again). The installed version is untouched."
    }
}
try {
    Rename-Item -LiteralPath $staging -NewName $name
} catch {
    if ($old) { Rename-Item -LiteralPath (Join-Path $Dest $old) -NewName $name }   # roll back
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    throw
}
Remove-Leftovers   # the old copy; stays behind (harmlessly) while Resolve still has it open
'@
function Invoke-Swap([string]$Mode, [string]$BundlePath) {
    $q = { param($v) "'" + ($v -replace "'", "''") + "'" }
    Invoke-Elevated ("& {`n$SwapScript`n} -Mode $Mode -Bundle " + (& $q $BundlePath) + " -Dest " + (& $q $Dest))
}
function Get-BundleVersion([string]$Bundle) {
    $f = Join-Path $Bundle "Contents\Resources\VERSION"
    if (Test-Path -LiteralPath $f) { "v" + (Get-Content -LiteralPath $f -TotalCount 1).Trim() } else { "" }
}
function Show-ResolveNote([string]$What) {
    if (Get-Process -Name "Resolve" -ErrorAction SilentlyContinue) {
        Write-Warning "DaVinci Resolve is running. $What when you restart it."
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
        Show-ResolveNote "The effect disappears"
        Say "Removing $Target (administrator prompt)"
        Invoke-Swap "uninstall" ""
    } else { Say "Not installed ($Target doesn't exist)" }
    Say "Done."
    return
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("ntscrt-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $src = $From
    # Run from an unzipped release (script next to the bundle)? Install that copy.
    if (-not $src -and $PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot "NTSCRT.ofx.bundle"))) {
        $src = Join-Path $PSScriptRoot "NTSCRT.ofx.bundle"
    }
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

    $newVer = Get-BundleVersion $bundle
    $oldVer = if (Test-Path $Target) { Get-BundleVersion $Target } else { "" }
    Show-ResolveNote "The new version loads"
    if ($oldVer) { Say "Installing NTSCRT $newVer (replacing $oldVer) to $Target (administrator prompt)" }
    else { Say "Installing NTSCRT $newVer to $Target (administrator prompt)" }
    Invoke-Swap "install" $bundle
    if (-not (Test-Path (Join-Path $Target "Contents\Win64\NTSCRT.ofx"))) { throw "Install didn't complete." }
    Say "Installed NTSCRT $newVer. (Re)start DaVinci Resolve: Effects > OpenFX > Filters > NTSCRT."
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $work
}
