# Registers the layer as an implicit API layer for the current user (no admin), or
# with -Machine for all users under HKLM (self-elevates; layer managers list these).
# The loader then injects it into every OpenXR app. Set the env var
# DISABLE_XR_APILAYER_S16N_CA_FIX=1 to skip it for one launch.
param([string]$Config = "Release", [switch]$Machine)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$manifest = Join-Path $root "build\$Config\XR_APILAYER_S16N_ca_fix.json"
if (-not (Test-Path $manifest)) { throw "Manifest not found: $manifest (build first)" }
if ($Machine) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Config", $Config, "-Machine")
        exit $p.ExitCode
    }
    $key = "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
} else {
    $key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
}
# Only create the key if missing: New-Item -Force on an existing key wipes its values.
if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }
# Value name is the manifest path; DWORD 0 means enabled.
New-ItemProperty -Path $key -Name $manifest -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host "Registered $manifest under $key"
