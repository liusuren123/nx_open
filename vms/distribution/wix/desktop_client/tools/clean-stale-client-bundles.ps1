#Requires -Version 5.1
<#
.SYNOPSIS
    Removes stale "Nx Meta Client" bundle registrations left behind by repeatedly installing
    locally-built installers that share the same version.

.DESCRIPTION
    WiX 3.14's light.exe generates a RANDOM registration id for every bundle it links, so every
    rebuild of the installer registers itself under a new key in "Apps & Features". Installing a
    build over an existing one of the same version does not remove the previous registration
    (Burn only removes related bundles with a *lower* version), so the entries accumulate.

    Apart from the clutter this is dangerous: a stale entry's "Uninstall" uninstalls the shared
    client MSI, which removes the application, its shortcuts and the client settings.

    This script keeps only the most recently installed registration and removes the others together
    with their "ProgramData\Package Cache" folders. MSI products (WindowsInstaller entries) are
    never touched.

.PARAMETER DisplayName
    Product display name to clean up. Default: "Nx Meta Client".

.PARAMETER DryRun
    Only report what would be removed.
#>
[CmdletBinding()]
param(
    [string] $DisplayName = 'Nx Meta Client',
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

if (-not $DryRun) {
    $isAdmin = ([Security.Principal.WindowsPrincipal] `
        [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        throw 'Removing registrations requires an elevated (administrator) PowerShell. Use -DryRun to preview.'
    }
}

$uninstallRoots = @(
    'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
    'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
)
$dependencyRoots = @(
    'HKLM:\SOFTWARE\Classes\Installer\Dependencies',
    'HKLM:\SOFTWARE\WOW6432Node\Classes\Installer\Dependencies'
)

function Get-CacheTimestamp([string] $cachePath) {
    if ([string]::IsNullOrWhiteSpace($cachePath)) { return [datetime]::MinValue }
    try {
        if (Test-Path -LiteralPath $cachePath) { return (Get-Item -LiteralPath $cachePath).LastWriteTime }
        $dir = Split-Path -Parent $cachePath
        if (Test-Path -LiteralPath $dir) { return (Get-Item -LiteralPath $dir).LastWriteTime }
    } catch { }
    return [datetime]::MinValue
}

# Collect Burn bundle registrations of this product (they carry a BundleProviderKey).
$bundles = @()
foreach ($root in $uninstallRoots) {
    if (-not (Test-Path $root)) { continue }
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
        $p = Get-ItemProperty -LiteralPath $_.PSPath -ErrorAction SilentlyContinue
        if ($null -eq $p) { return }
        if ($p.DisplayName -ne $DisplayName) { return }
        if ([string]::IsNullOrWhiteSpace($p.BundleProviderKey)) { return }
        $bundles += [pscustomobject]@{
            KeyPath     = $_.PSPath
            KeyName     = $_.PSChildName
            ProviderKey = $p.BundleProviderKey
            CachePath   = $p.BundleCachePath
            Version     = $p.BundleVersion
            Timestamp   = Get-CacheTimestamp $p.BundleCachePath
        }
    }
}

if ($bundles.Count -le 1) {
    Write-Host "Nothing to do: found $($bundles.Count) '$DisplayName' bundle registration(s)."
    return
}

$keep = $bundles | Sort-Object -Property Timestamp, Version -Descending | Select-Object -First 1
$stale = $bundles | Where-Object { $_.KeyName -ne $keep.KeyName }

Write-Host "Keeping : $($keep.KeyName)  (provider $($keep.ProviderKey), version $($keep.Version), cache $($keep.CachePath))"
foreach ($s in $stale) {
    Write-Host "Removing: $($s.KeyName)  (provider $($s.ProviderKey), version $($s.Version), cache $($s.CachePath))"
}

if ($DryRun) {
    Write-Host 'Dry run - nothing was changed.'
    return
}

$backupDir = Join-Path $env:TEMP 'nx-stale-client-bundles-backup'
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
Write-Host "Backing up removed keys to $backupDir"

foreach ($s in $stale) {
    $regPath = ($s.KeyPath -replace '^.*Registry::', '')
    $backupFile = Join-Path $backupDir (($s.KeyName -replace '[{}]', '') + '.reg')
    & reg.exe export $regPath $backupFile /y | Out-Null

    if (Test-Path -LiteralPath $s.KeyPath) {
        Remove-Item -LiteralPath $s.KeyPath -Recurse -Force
        Write-Host "  removed ARP key $($s.KeyName)"
    }

    if (-not [string]::IsNullOrWhiteSpace($s.CachePath)) {
        $cacheDir = Split-Path -Parent $s.CachePath
        if ((Test-Path -LiteralPath $cacheDir) -and ($cacheDir -like '*\Package Cache\*')) {
            Remove-Item -LiteralPath $cacheDir -Recurse -Force -ErrorAction Continue
            Write-Host "  removed cache $cacheDir"
        }
    }

    foreach ($root in $dependencyRoots) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -like "*$($s.ProviderKey)*" } |
            ForEach-Object {
                Remove-Item -LiteralPath $_.PSPath -Recurse -Force
                Write-Host "  removed dependency $($_.PSChildName)"
            }
    }
}

Write-Host 'Done.'
