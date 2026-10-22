# openxr-ca-fix

OpenXR API layer that reduces residual lateral chromatic aberration at the lens edges
(Pimax Crystal Light, Pimax OpenXR runtime, DX12 games).

## Status
Step 1: pass-through layer that logs `xrEndFrame` and `xrCreateSession`.

## Build
Requires Visual Studio 2022 (C++ workload) and CMake 3.20+. OpenXR headers are downloaded at configure time.

    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
    powershell -ExecutionPolicy Bypass -File scripts\register.ps1

`scripts\unregister.ps1` removes it. Set `DISABLE_XR_APILAYER_S16N_CA_FIX=1` to skip it for one launch.

## Logs
`%LOCALAPPDATA%\openxr-ca-fix\layer-<game exe>.log`, overwritten each run.

## Plan
1. Pass-through logging layer (this).
2. Hook xrCreateSession for the D3D12 device/queue, wrap swapchain creation, copy app images through unchanged.
3. Per-channel radial CA shader, coefficients in a config file.
4. Fence and resource-barrier cleanup.
