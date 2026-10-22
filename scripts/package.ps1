# Builds Release and copies the DLL and manifest into dist\ for end users.
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") -Config Release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
foreach ($f in "XR_APILAYER_S16N_ca_fix.dll", "XR_APILAYER_S16N_ca_fix.json") {
    Copy-Item -Path (Join-Path $root "build\Release\$f") -Destination (Join-Path $root "dist") -Force
}
Write-Host "dist\ updated"
