# Registers the layer as an implicit API layer for the current user (no admin).
# The loader then injects it into every OpenXR app. Set the env var
# DISABLE_XR_APILAYER_S16N_CA_FIX=1 to skip it for one launch.
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$manifest = Join-Path $root "build\$Config\XR_APILAYER_S16N_ca_fix.json"
if (-not (Test-Path $manifest)) { throw "Manifest not found: $manifest (build first)" }
$key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
New-Item -Path $key -Force | Out-Null
# Value name is the manifest path; DWORD 0 means enabled.
New-ItemProperty -Path $key -Name $manifest -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host "Registered $manifest"
