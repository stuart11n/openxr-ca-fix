# openxr-ca-fix

OpenXR API layer that reduces residual lateral chromatic aberration at the lens edges
(Pimax Crystal Light, Pimax OpenXR runtime, DX12 games).

## Status
Step 2: captures the game's D3D12 device and queue, and for each colour swapchain in a
projection layer copies the released image into a layer-owned swapchain, which is what
gets submitted. The image should look identical to no layer. Depth and quad layers pass
through untouched; any failure falls back to submitting the app's swapchain.

## Build
Requires Visual Studio 2022 (C++ workload) and CMake 3.20+. OpenXR headers are downloaded at configure time.

    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
    powershell -ExecutionPolicy Bypass -File scripts\register.ps1

This registers per-user (HKCU, no admin). Add `-Machine` to register for all users under HKLM
instead; it prompts for elevation, and layer managers such as OpenXR-API-Layers-GUI list it there.

`scripts\unregister.ps1` removes it (`-Machine` for the HKLM entry). Set `DISABLE_XR_APILAYER_S16N_CA_FIX=1` to skip it for one launch.

## Logs
`%LOCALAPPDATA%\openxr-ca-fix\layer-<game exe>.log`, overwritten each run.

## Plan
1. Pass-through logging layer (this).
2. Hook xrCreateSession for the D3D12 device/queue, wrap swapchain creation, copy app images through unchanged.
3. Per-channel radial CA shader, coefficients in a config file.
4. Fence and resource-barrier cleanup.
