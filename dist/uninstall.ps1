# Unregisters the layer (all users and current user; self-elevates) and deletes
# %ProgramFiles%\openxr-ca-fix. Settings and logs in %LOCALAPPDATA%\openxr-ca-fix are kept.
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
foreach ($key in "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit",
                 "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit") {
    if (-not (Test-Path $key)) { continue }
    foreach ($p in (Get-ItemProperty -Path $key).PSObject.Properties) {
        if ($p.Name -like "*$name.json") {
            Remove-ItemProperty -Path $key -Name $p.Name
            Write-Host "Unregistered $($p.Name)"
        }
    }
}

$dest = Join-Path $env:ProgramFiles "openxr-ca-fix"
if (Test-Path $dest) {
    try {
        Remove-Item -Recurse -Force $dest
        Write-Host "Deleted $dest"
    } catch {
        Write-Host "Unregistered, but could not delete $dest (a VR game may still have it loaded). Delete it later."
    }
}
if ($Elevated) { Read-Host "Press Enter to close" }
