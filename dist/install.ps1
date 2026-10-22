# Installs the layer for all users: copies the DLL and manifest from this folder to
# %ProgramFiles%\openxr-ca-fix and registers it as an implicit OpenXR API layer under
# HKLM (self-elevates). Re-run to update. Any other registration of this layer (for
# example a developer build) is removed so it isn't loaded twice.
param([switch]$Elevated)
$ErrorActionPreference = "Stop"
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Elevated")
    exit $p.ExitCode
}

# The elevated copy runs in its own window; keep it open so the result can be read.
trap {
    Write-Host "ERROR: $_" -ForegroundColor Red
    if ($Elevated) { Read-Host "Press Enter to close" }
    exit 1
}

$name = "XR_APILAYER_S16N_ca_fix"
$dest = Join-Path $env:ProgramFiles "openxr-ca-fix"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
try {
    foreach ($f in "$name.dll", "$name.json") {
        $src = Join-Path $PSScriptRoot $f
        Unblock-File -Path $src -ErrorAction SilentlyContinue
        Copy-Item -Path $src -Destination $dest -Force
    }
} catch {
    Write-Host "Could not copy the layer files. Close any VR game (it keeps the DLL loaded) and run this again."
    Write-Host $_
    if ($Elevated) { Read-Host "Press Enter to close" }
    exit 1
}
$manifest = Join-Path $dest "$name.json"

# Back up the machine-wide layer list before touching it. Kept with the settings,
# so uninstalling (which deletes the Program Files folder) doesn't remove it.
$regPath = "SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
if (Test-Path "HKLM:\$regPath") {
    $backupDir = Join-Path $env:LOCALAPPDATA "openxr-ca-fix"
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    $backup = Join-Path $backupDir "implicit-layers-backup.reg"
    reg export "HKLM\$regPath" $backup /y | Out-Null
    Write-Host "Backed up the OpenXR layer list to $backup"
}

foreach ($key in "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit",
                 "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit") {
    if (-not (Test-Path $key)) { continue }
    foreach ($p in (Get-ItemProperty -Path $key).PSObject.Properties) {
        if ($p.Name -like "*$name.json" -and $p.Name -ne $manifest) {
            Remove-ItemProperty -Path $key -Name $p.Name
            Write-Host "Removed old registration $($p.Name)"
        }
    }
}

$key = "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
# Only create the key if missing: New-Item -Force on an existing key wipes its values.
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
# Value name is the manifest path; DWORD 0 means enabled.
New-ItemProperty -Path $key -Name $manifest -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host "Installed to $dest and registered."
if ($Elevated) { Read-Host "Press Enter to close" }
