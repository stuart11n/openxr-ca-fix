# Removes any registration of this layer from the current user's implicit layer list,
# or with -Machine from the all-users (HKLM) list (self-elevates).
param([switch]$Machine)
if ($Machine) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        $p = Start-Process powershell -Verb RunAs -Wait -PassThru -ArgumentList @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-Machine")
        exit $p.ExitCode
    }
    $key = "HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
} else {
    $key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
}
if (-not (Test-Path $key)) { return }
$props = Get-ItemProperty -Path $key
foreach ($p in $props.PSObject.Properties) {
    if ($p.Name -like "*XR_APILAYER_S16N_ca_fix.json") {
        Remove-ItemProperty -Path $key -Name $p.Name
        Write-Host "Unregistered $($p.Name) from $key"
    }
}
