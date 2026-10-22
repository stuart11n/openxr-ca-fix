# Removes any registration of this layer from the current user's implicit layer list.
$key = "HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
if (-not (Test-Path $key)) { return }
$props = Get-ItemProperty -Path $key
foreach ($p in $props.PSObject.Properties) {
    if ($p.Name -like "*XR_APILAYER_S16N_ca_fix.json") {
        Remove-ItemProperty -Path $key -Name $p.Name
        Write-Host "Unregistered $($p.Name)"
    }
}
