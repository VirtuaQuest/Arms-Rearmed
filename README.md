# ARMS Recomp

A static recompilation of *ARMS* (Nintendo Switch, version 5.4.0: the base game with its last update) into a native
Windows x86-64 program, built on [LAS Recomp](https://github.com/peiikai/LASRecomp)'s recompiler and Switch runtime
(by way of MUA3 Recomp and DOAX3 Recomp). The game's ARM64 code is translated to C and compiled for the PC; a Switch
runtime takes the console's place (graphics through Vulkan, audio, input, files, services).

**Status: playable offline.** Every offline mode has been played through at 60 fps with keyboard, mouse and
controller input: Grand Prix, Versus (Fight, Team Fight, V-Ball, Hoops, Skillshot, Hedlok Scramble, 1-on-100,
ARMS Test), Training, and the Badges, Gallery, Replays, Dashboard and Get ARMS screens. Shaders are prepared before
the game appears (a few seconds; about 12 s the first time after an install), so there is no stutter while it
starts and no model shows up late. Online play, local wireless, the web manual and the eShop aren't available: the
game says so and stays in its menus. Details: [docs/CHANGES.md](docs/CHANGES.md).

No game files are included or ever committed. You need your own legally obtained copy of the game, its update and
the keys from your own console.

## Setup (Windows)

1. Install [MSYS2](https://www.msys2.org) to `C:\msys64`, then in its UCRT64 shell:
   `pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld make mingw-w64-ucrt-x86_64-SDL3
   mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc`
   (as LAS Recomp's `tools/setup_windows.ps1`).
2. Install [Python 3.12](https://www.python.org) (the `py` launcher).
3. Build the launcher once, from Git Bash or the MSYS2 UCRT64 shell in this folder:
   `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make launcher dlls` (`dlls` copies the MSYS2 DLLs
   next to it, so it starts from Explorer).
4. Run `build\armslauncher.exe`. On **GAME FILES**, choose your base game (XCI or NSP), the 5.4.0 update (NSP) and
   your `prod.keys`, then **INSTALL**: it makes the Python environment, extracts the game's code, recompiles it and
   builds the PC version (about 10 minutes the first time). Then **FIGHT!** on the PLAY page.

Only the game's code (exefs) and control data are copied out; the game data is read in place from your files through
`extracted/romfs.src`, which holds your title key: keep it private.

### By hand

```sh
py -V:3.12 -m venv .venv
.venv/Scripts/python -m pip install unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0
export MSYS_NO_PATHCONV=1 PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
.venv/Scripts/python -I tools/nsp_extract.py "C:/path/ARMS.xci" "C:/path/prod.keys" extracted --update "C:/path/update.nsp"
make gen PY=.venv/Scripts/python.exe      # about 30 s
make -j16 PY=.venv/Scripts/python.exe     # about 8 minutes the first time
tools/run.sh                              # or build/armsrecomp.exe extracted (log: arms.log)
```

## Playing

| Action | Keyboard / mouse | Controller (by position, as on the Switch) |
|---|---|---|
| Move, curve a punch | W A S D | Left stick |
| Left / right punch | Left / right mouse (or Backspace / Enter) | Bottom / right face button (B / A) |
| Grab | Middle mouse, or both mouse buttons | Both punches (A + B) |
| Jump / dash | Space / Left Shift | Top / left face button (X / Y) |
| Guard | Left Ctrl | Left stick press |
| Rush | Q or E | L or R |
| Pause (+) / (-) | Tab / M | Start / Back |
| Menu (controls, video, audio) | Esc or F1 | Home / Guide |

Everything can be rebound in game (Esc or F1, then Controls): keys, mouse buttons and controller buttons. Any
controller SDL 3 knows works (Xbox, PlayStation, Switch Pro, and more), with rumble. The launcher's GRAPHICS page
(and the in-game Video tab) set the window mode and size, the internal resolution (up to 4x, or dynamic), FSR 1
upscaling, SMAA and the aspect ratio. ARMS runs at its own 60 fps: its fights step once a frame.

## Credits

- **LAS Recomp** by Peiikai and contributors (MIT License, `LICENSE.LASRecomp`): the recompiler and the runtime.
- Third-party components keep their own licenses (`third_party/`): SDL 3, Dear ImGui, FFmpeg, AMD FSR, SMAA, and the
  Baloo 2 font (SIL Open Font License) the launcher uses.
- [Switchbrew](https://switchbrew.org) documentation.

*ARMS* is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo. The launcher's look is
original: no game art, fonts or logos.
