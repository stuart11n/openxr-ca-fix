# openxr-ca-fix

OpenXR API layer that reduces residual lateral chromatic aberration at the lens edges
(Pimax Crystal Light, Pimax OpenXR runtime, DX12 games).

**Want to build your own?** [`docs/BUILD_PROMPT.md`](docs/BUILD_PROMPT.md) is a prompt a coding assistant can follow to rebuild this layer from scratch.

**Using it?** A prebuilt layer and installer are in [`dist/`](dist/); see
[`dist/README.md`](dist/README.md) for install and tuning instructions. The rest of this
page is for building from source.

## How it works
Each eye's colour image goes through a full-screen pass that resamples red and blue
radially around the lens centre (green fixed) into a layer-owned swapchain, which is
what gets submitted. Depth and quad layers pass through untouched; any failure falls back
to a plain copy or to the app's own swapchain.

## Tuning
Set `live_update=1` in `%LOCALAPPDATA%\openxr-ca-fix\config.ini` (takes effect the next time VR
starts). With it at 0, the default, the file is read once per VR session and the keyboard is
ignored. With it at 1, the file is re-read about twice a second and the hotkeys work.

In the headset, hold **Ctrl+Shift** and press:

| Key | Does |
|---|---|
| T | test grid on/off (white lines on black, run through the correction) |
| Up / Down | choose what to adjust: blue, red, lens centre x, lens centre y |
| Left / Right | decrease / increase it (0.25 px for colours, 10 px for centres; hold to repeat) |
| L / R / B | adjust left eye / right eye / both |
| E | correction on/off |

A readout just below the centre of the view shows the value; its border colour says what is
selected (blue, red, white = centre x, yellow = centre y), and it only appears in the eyes
being adjusted. Every change is saved to `%LOCALAPPDATA%\openxr-ca-fix\config.ini`, which can
also be edited by hand (changes apply within a second).

Colour on a line's outer side (facing away from the middle) means increase that colour; on
its inner side, decrease it. If one side of an eye is over-corrected compared with the other,
move that eye's lens centre towards the over-corrected side.

Tip: tune one eye at a time. Move its horizontal centre well off to one side to exaggerate the
fringing, adjust red and blue until the separation is symmetrical, then bring the centre back until
the separation converges.

## Build
Requires Visual Studio 2022 (C++ workload) and CMake 3.20+. OpenXR headers are downloaded at configure time.

    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
    powershell -ExecutionPolicy Bypass -File scripts\register.ps1

This registers per-user (HKCU, no admin). Add `-Machine` to register for all users under HKLM
instead; it prompts for elevation, and layer managers such as OpenXR-API-Layers-GUI list it there.

`scripts\unregister.ps1` removes it (`-Machine` for the HKLM entry). Set `DISABLE_XR_APILAYER_S16N_CA_FIX=1` to skip it for one launch.

To refresh the prebuilt files in `dist\` after a change, run `scripts\package.ps1` and commit
the DLL and manifest. Note that `dist\install.ps1` removes any other registration of this
layer, including a developer one pointing at `build\Release`.

## Logs
`%LOCALAPPDATA%\openxr-ca-fix\layer-<game exe>.log`, overwritten each run.

## Still to do
- Fence and resource-barrier cleanup.

## License
MIT; see [LICENSE](LICENSE).

---
Made by [www.s16n.com](https://www.s16n.com)
