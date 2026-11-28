# Settings and the overlay in the GameCube runtime

What is shown and edited in the game, and by the launcher, is described by the **game's `recomplauncher.json`** (the `config` and `settings` parts), not by this runtime. The in-game menu is the separate [To-Recomp/overlay](https://github.com/To-Recomp/overlay) project, the same for every console of every game: it reads the manifest, edits the configuration files in place, and tells the program (here, `gp::overlay_glue`) when a file changed. See that project's `docs/MANIFEST.md` and `docs/UI.md`.

This page covers what the GameCube runtime does with it.

## Wiring

- `gp::Config::data_dir` (or `RECOMPLAUNCHER_DATA`): the instance's data folder, where `settings.toml` and `input.toml` live.
- `gp::Config::manifest` (or `TOREC_MANIFEST`): the game's `recomplauncher.json`. Without one there is no overlay and the runtime reads the files (or the defaults) as usual. The release's start script exports `TOREC_MANIFEST`.
- Build: `GCRECOMP_OVERLAY_DIR` (default `../overlay`) is the overlay checkout; without it the runtime builds with `GCRECOMP_NO_OVERLAY`. `GCRECOMP_OVERLAY=OFF` does the same.
- `src/gp/overlay_host.cpp` is the runtime's side of the overlay's `Host`: it lists gamepads and audio devices, captures a key or a button for a rebinding (`gp/bindings.h`), suspends the pad while the menu is open, and re-reads the configuration when the overlay writes a file.

## What the runtime reads

The runtime applies these keys live (the settings overlay and the launcher write them; the runtime never does). A missing key gives the default; a wrong value is reported on stderr and falls back to the default of that key. They are the keys of the shared settings fragment of the overlay (`packaging/settings-manifest.json` there), so a game that merges it shows them.

`settings.toml`:

| Table | Key | Type | Default | Meaning |
| --- | --- | --- | --- | --- |
| `[video]` | `fullscreen` | bool | `false` | full screen or a window (`RECOMPLAUNCHER_MODE=bigpicture` forces full screen for one run, not saved) |
| `[video]` | `vsync` | bool | `true` | wait for the screen's refresh (never during a `GCRECOMP_BENCH` run) |
| `[video]` | `aspect` | `"fit"` \| `"stretch"` \| `"wide16x9"` \| `"wide21x9"` | `"fit"` | `fit`: 4:3 with bars; `stretch`: fills the window; the others stretch the 4:3 picture into the largest box of that shape |
| `[video]` | `stats` | `"off"` \| `"fps"` | `"off"` | frame-rate counter (drawn by the overlay) |
| `[audio]` | `volume` | float 0..1 | `1.0` | the perceived volume: the gain is its square |
| `[audio]` | `device` | string | `""` | playback device name; empty = the system's default |

`input.toml`:

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `glyphs` | `"GameCube"` \| `"Xbox"` \| `"Nintendo"` \| `"PlayStation"` | `"GameCube"` | which controller's button names the overlay shows |
| `stick_deadzone` | float 0..0.5 | `0.125` | fraction of a stick's travel read as centred |
| `[keyboard]`, `[gamepad]` | one key per GameCube control | see below | what drives each control |

A binding value is names joined by `|` (`a = "Return|Z"`), or a TOML array of strings; an empty string means "not bound". An unknown key or button name makes the runtime ignore both tables of that read (the previous bindings stay, or the defaults at start) and say so on stderr.

### The 20 GameCube controls

| Key | Control | Keyboard default | Gamepad default |
| --- | --- | --- | --- |
| `a` | A | `Return\|Z` | `A` |
| `b` | B | `X\|Backspace` | `X` |
| `x` | X | `C` | `B` |
| `y` | Y | `V` | `Y` |
| `start` | Start | `Space` | `Start` |
| `z` | Z | `W` | `RB` |
| `l` | L | `Q` | `LT\|LB` |
| `r` | R | `E` | `RT` |
| `dpad_up` `dpad_down` `dpad_left` `dpad_right` | D-pad | `Up` `Down` `Left` `Right` | `D-pad Up` ... |
| `stick_up` `stick_down` `stick_left` `stick_right` | Control stick | `Up` `Down` `Left` `Right` | `Left Stick Up` ... |
| `cstick_up` `cstick_down` `cstick_left` `cstick_right` | C-stick | none | `Right Stick Up` ... |

The pad defaults put the GameCube's buttons where they sit on the pad. The file names the pad's positions Xbox-style (A south, B east, X west, Y north), so GameCube A is `"A"`, GameCube B (the small one to the lower left) is `"X"`, GameCube X is `"B"` and GameCube Y is `"Y"`.

Names: keyboard = SDL key names (`Return`, `Space`, `Left Shift`, `Keypad 4`...); `F1` (the overlay) and `F10` (the diagnostic bundle) are refused. Gamepad = the launcher's vocabulary: `A` `B` `X` `Y` (`Cross` `Circle` `Square` `Triangle` are read as A B X Y), `LB` `RB`, `LT` `RT`, `Start`, `Back`, `Guide` (reserved: opens the overlay), `L3` `R3`, `D-pad Up/Down/Left/Right`, and `Left Stick Up/Down/Left/Right`, `Right Stick Up/Down/Left/Right`.

The manifest block that lists these 20 controls for the launcher and the overlay is the overlay's `packaging/bindings-gamecube.json`.
