# Prompt: build an OpenXR layer that fixes edge chromatic aberration

Copy everything below the line into a coding assistant that can edit files and run commands
on a Windows PC.

---

You are building **openxr-ca-fix**, an OpenXR API layer DLL for Windows. It reduces the
residual lateral chromatic aberration (red/blue colour fringing) that a VR headset's lens
leaves near the edges of the view. Do the work yourself: install any tools you need, write the
code, build it, and test it with me in the headset at each step.

## My setup (assume nothing beyond this)
- Headset: Pimax Crystal Light, running on the Pimax OpenXR runtime.
- Games: DirectX 12 OpenXR games, mainly Microsoft Flight Simulator 2024 (MSFS).
- Windows x64, with Visual Studio 2022 (C++ workload) and CMake 3.20 or later.
- Other implicit OpenXR layers are already installed and must keep working, for example
  OpenXR Toolkit, DesktopXR and VectorXR.

## What the layer does
1. It hooks `xrEndFrame`.
2. For each view of each projection layer, it runs a full-screen pixel shader that reads the
   game's swapchain image and writes a corrected image into a swapchain the layer owns. That
   swapchain is what gets submitted to the runtime.
3. The shader leaves green where it is and resamples red and blue radially around the lens
   centre, using separate coefficients for each.
4. Depth layers, quad layers and anything it can't handle pass through unchanged.
5. Any failure falls back, first to a plain copy and then to the game's own swapchain. The
   layer must never break a game.

### Correction model
Work in tangent space, using the view's `XrFovf` (tanLeft, tanRight, tanUp, tanDown):
```
d  = tan(uv) - centre
r2 = dot(d, d)
s  = 1 + k1*r2 + k2*r2*r2
sample_uv = toUv(centre + d*s)        // separately for R (redK) and B (blueK)
```
- A positive k samples further out, which pulls that colour towards the middle.
- Account for the sub-image rect: `uvOffset = rect.offset / textureSize` and
  `uvScale = rect.extent / textureSize`.

### Config units, which must be human-scale
Raw k values are tiny and impossible to tune by hand. Config values are therefore **pixels of
pull at 45° off centre** (tan = 1), and are converted for each view:
```
pxPerTanX = rect.width  / (tanRight - tanLeft)
pxPerTanY = rect.height / (tanUp - tanDown)
k1 = colour_px / pxPerTanX
k2 = colour_outer_px / pxPerTanX
centre = (centre_x_px / pxPerTanX, centre_y_px / pxPerTanY)
```

### Config file
The config lives at `%LOCALAPPDATA%\openxr-ca-fix\config.ini`.
- If the file is missing, write it with defaults: everything 0, so the layer starts doing
  nothing, plus `enabled=1` and `live_update=0`.
- Keys:
  - `live_update`, `enabled`
  - `left_red`, `left_blue`, `right_red`, `right_blue`
  - `red_outer`, `blue_outer` (both eyes)
  - `left_center_x`, `left_center_y`, `right_center_x`, `right_center_y`
- Put a comment header at the top that explains how to tune.
- Read the file once in `xrCreateSession`.
- Only when `live_update=1`:
  - Re-read the file when its modification time changes (check every ~45 frames).
  - Accept the hotkeys below.
  - When a hotkey changes a value, rewrite the file and record that write's timestamp, so the
    layer doesn't reload its own write.
- When `live_update=0`, ignore the keyboard completely.

### In-headset tuning (tuning by editing the file alone was too hard)
Read keys with `GetAsyncKeyState`, and only while **Ctrl+Shift** is held:

| Key | Action |
|---|---|
| T | Test grid on/off. White 3 px lines every 96 px on black, drawn in the shader before the correction so it is corrected too. |
| Up / Down | Choose the parameter: blue, red, centre x, centre y. |
| Left / Right | Change it by ±0.25 px for colours or ±10 px for centres, with auto-repeat. |
| L / R / B | Scope the change to the left eye, right eye or both. |
| E | Correction on/off, for before/after comparison. |

- Draw a HUD in the shader: a box of about 380×120 px at (0.5, 0.6) of the view showing the
  value as seven-segment digits (`[-]ddd.dd`).
- The border colour shows the parameter: blue, red, white for centre x, yellow for centre y.
- Show the box only in the eyes being adjusted, and for about 4 s after a keypress.

Tuning guidance to put in the docs (the user's view of a grid line near the edge):
- Colour on the line's outer side, away from the middle: increase that colour.
- Colour on the inner side: decrease it.
- Magenta outside means increase both red and blue; green outside means decrease both.
- If one side of an eye is over-corrected compared with the other, move that eye's lens
  centre towards the over-corrected side.

## Implementation notes
- C++17 and CMake. Fetch OpenXR-SDK headers with FetchContent (tag `release-1.1.38`). Link
  `d3d12` and `d3dcompiler`. Use the static CRT, `/W4 /permissive-`, and a `.def` file that
  exports only `xrNegotiateLoaderApiLayerInterface`.
- Layer name: `XR_APILAYER_<VENDOR>_ca_fix`. Generate the JSON manifest with CMake. Include a
  `disable_environment` variable so the layer can be skipped for one launch.
- Loader negotiation:
  - Implement `xrNegotiateLoaderApiLayerInterface`, validating all struct sizes and versions.
  - Implement `xrCreateApiLayerInstance`: walk `XrApiLayerCreateInfo->nextInfo`, call the
    next layer, then resolve the next functions.
  - Implement your own `xrGetInstanceProcAddr` that returns the hooks. Set the next-GIPA
    pointer last, so hooking only starts once everything is resolved.
- Hooks:
  - `xrCreateSession`: take the `ID3D12Device` and `ID3D12CommandQueue` from
    `XrGraphicsBindingD3D12KHR`.
  - `xrDestroySession`.
  - `xrCreateSwapchain`: add `TRANSFER_SRC | SAMPLED` to the game's colour swapchains, and
    retry without them if creation fails.
  - `xrEnumerateSwapchainImages`: remember the game's `ID3D12Resource`s.
  - `xrAcquireSwapchainImage` / `xrReleaseSwapchainImage`: track which image indices the game
    acquired and released.
  - `xrDestroySwapchain`, `xrDestroyInstance`, `xrEndFrame`.
- In `xrEndFrame`:
  - Copy the frame info and the layer arrays.
  - For each projection view, use the game's last released image as the source.
  - Lazily create a matching output swapchain, and acquire, wait and release it around the
    shader pass.
  - Substitute that swapchain and an `imageRect` covering the whole image.
  - Chain depth info (`XrCompositionLayerDepthInfoKHR`) through unchanged.
- D3D12 copier class:
  - A command allocator ring 3 deep, guarded by a fence.
  - Barriers: RENDER_TARGET to PIXEL_SHADER_RESOURCE for the source and RENDER_TARGET for the
    destination, both restored afterwards. Swapchain images rest in RENDER_TARGET.
  - Compile the HLSL at runtime with `D3DCompile`. Use a full-screen triangle VS.
  - The root signature has root constants for the parameters, one SRV table and a static
    linear clamp sampler.
  - Cache PSOs per format. Create the SRV and RTV with the swapchain's own format, which may
    be typeless or sRGB.
  - Use a descriptor ring per frame slot.
  - Only shade when the swapchain has arraySize 1, sampleCount 1 and SAMPLED usage. Otherwise
    use `CopyResource`.
- Logging: write to `%LOCALAPPDATA%\openxr-ca-fix\layer-<exe name>.log`, overwritten each run.
  Log the negotiation, the swapchains, the first few frames' layer and view layout, and
  `CA pass ready` once the shader compiles.

## Build plan: stop after each step and have me test it in the headset
1. **Pass-through layer.** Hook everything and log it, but change nothing. Register the layer.
   I confirm that the log appears and the game is unaffected.
2. **Swapchain wrap and copy.** Submit the layer's own swapchains with a plain copy. The image
   should look identical, with no fps loss.
3. **CA shader, config, tuning UI.** I tune the correction in the headset and confirm the
   fringing is reduced.
4. **Cleanup and distribution.**
   - Remove leftover debugging code.
   - Add a prebuilt DLL and manifest in `dist/`, with `install.cmd`/`install.ps1` and
     `uninstall.cmd`/`uninstall.ps1`. Install copies to `C:\Program Files\openxr-ca-fix` and
     registers under HKLM with self-elevation, keeping the elevated window open on error.
   - Add an end-user `dist/README.md` and a developer `README.md`.
   - Possible later work: fence and barrier tidy-up, and a ~0.2 s one-time hitch when the
     swapchains are first created.

Provide `scripts/build.ps1` (configure and build Release),
`register.ps1`/`unregister.ps1` (HKCU by default, `-Machine` for HKLM) and `package.ps1`
(build, then copy to `dist/`).

## Hard rules and lessons learned
- **Never recreate or clear a shared registry key.** Layers are registered as values under
  `HKLM` or `HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`. Each value is named by the
  manifest path, and DWORD 0 means enabled.
  - Running `New-Item -Force` on that existing key deletes every other layer's registration.
    That happened once. Create the key only if it doesn't exist (`Test-Path`), and only ever
    add or remove your own value.
  - Back up the key (`reg export`) before any write, and diff it afterwards.
  - Ask me before writing to the registry.
  - Store the backup outside the install folder, for example in `%LOCALAPPDATA%`, so
    uninstalling doesn't delete it.
- Value order in the Implicit key is load order. Add the layer last, and don't reorder other
  layers' values.
- Remove any other registration of this layer (HKLM or HKCU, other paths) when installing, so
  it never loads twice.
- MSFS keeps the layer DLL loaded until the sim quits. Close the game before rebuilding or
  installing, or the DLL will be locked.
- MSFS 2024 facts:
  - It submits one projection layer.
  - Each eye has its own swapchain at 3882×4031 (not an array swapchain).
  - Depth submission is enabled.
  - It destroys and recreates swapchains mid-session, so handle swapchain churn.
- Don't jump to conclusions from logs. If a log looks incomplete, check that the game has
  actually reached VR before deciding something failed.
- Before publishing the repo, check its history and the DLL for secrets, local paths and
  personal emails.
