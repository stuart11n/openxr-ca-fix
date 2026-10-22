# openxr-ca-fix: user guide

Reduces the red/blue colour fringing (lateral chromatic aberration) near the edges of
the lenses in OpenXR games. Made for the Pimax Crystal Light on the Pimax OpenXR runtime
with DirectX 12 games such as Microsoft Flight Simulator 2024. Other DX12 OpenXR games
should work too; other graphics APIs are passed through unchanged.

It does nothing until you tune it: the correction depends on your headset and eyes, so
it starts at zero.

## Install
1. Close any VR game.
2. Double-click `install.cmd` in this folder and accept the admin prompt.

This copies two files to `C:\Program Files\openxr-ca-fix` and registers them with OpenXR for
all users. Other OpenXR layers' entries are left alone, and the current layer list is backed
up to `%LOCALAPPDATA%\openxr-ca-fix\implicit-layers-backup.reg` first (double-click it to
restore). To update, replace this folder with the new one and run `install.cmd` again.

Layer managers such as OpenXR-API-Layers-GUI will list it as `XR_APILAYER_S16N_ca_fix`,
where you can also switch it off or change its order.

## Uninstall
Close any VR game and double-click `uninstall.cmd`. Your settings in
`%LOCALAPPDATA%\openxr-ca-fix` are kept; delete that folder too if you want them gone.

## Tune it
Settings live in `%LOCALAPPDATA%\openxr-ca-fix\config.ini` (paste that into the Explorer
address bar). The file is created the first time you start VR with the layer installed.

1. Open `config.ini` in Notepad, set `live_update=1` and save.
2. Start the game and enter VR. A keyboard needs to be within reach.
3. Hold **Ctrl+Shift** and use these keys:

| Key | Does |
|---|---|
| T | test grid on/off (white lines on black, replaces the game image) |
| Up / Down | choose what to adjust: blue, red, lens centre left/right, lens centre up/down |
| Left / Right | decrease / increase it (hold to repeat) |
| L / R / B | adjust the left eye only / right eye only / both eyes |
| E | correction on/off, to compare before and after |

A small box just below the centre of the view shows the current value. Its border colour
says what you are adjusting: blue, red, white (centre left/right) or yellow (centre up/down).
It only shows in the eye or eyes being adjusted.

4. With the test grid on, look at lines near the edges of the view:
   - colour on a line's **outer** side (facing away from the middle): increase that colour
   - colour on its **inner** side (facing the middle): decrease it
   - magenta outside: increase red and blue; green outside: decrease both
   - yellow inside with blue outside is one problem: blue is too far out, increase blue
   - if one side of an eye needs more than the other, move that eye's lens centre towards
     the side that is over-corrected
5. Turn the grid off (T) and check in the game.
6. When you are happy, set `live_update=0` in `config.ini` and save.

### Tip from the author
It's easiest to do one eye at a time (L or R). First move that eye's horizontal centre
well off to one side: this makes the fringing worse, so the colour separation is very
easy to see. Adjust red and blue until the separation looks the same on both sides, then
bring the centre back until the separation converges.

Every change is saved to `config.ini` straight away. With `live_update=1` you can also edit
the file by hand while in VR and the change shows within a second.

## Settings reference
| Setting | Meaning |
|---|---|
| `live_update` | 1 = re-read the file while in VR and accept the hotkeys. 0 (default) = read once when VR starts, keyboard ignored. Changing 0 to 1 takes effect the next time VR starts. |
| `enabled` | 1 = correction on, 0 = off |
| `left_red`, `right_red`, `left_blue`, `right_blue` | pixels of pull towards the middle for that colour and eye, measured 45 degrees off centre. Negative pushes outward. |
| `red_outer`, `blue_outer` | extra pull that only shows near the far edges (both eyes) |
| `left_center_x/y`, `right_center_x/y` | lens centre in pixels from straight ahead, +x right, +y up |

## Turning it off
- For one launch: set the environment variable `DISABLE_XR_APILAYER_S16N_CA_FIX=1`.
- Keep it installed but do nothing: set `enabled=0` in `config.ini`.

## If something is wrong
Each game writes a log to `%LOCALAPPDATA%\openxr-ca-fix\layer-<game exe>.log`, replaced on
every run. "CA pass ready" means the correction is active; a line with "failed" says what
went wrong. If the layer can't do its job it falls back to passing the game's image through
unchanged.

## License
MIT; see `LICENSE.txt`. Provided as is, with no warranty.
