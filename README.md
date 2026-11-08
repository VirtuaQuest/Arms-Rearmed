# MUA3 Recomp

A work-in-progress static recompilation of *MARVEL ULTIMATE ALLIANCE 3: The Black Order* (Nintendo Switch, 4.0.1:
the base game with its last update) into a native Windows x86-64 program, built on
[LAS Recomp](https://github.com/peiikai/LASRecomp)'s recompiler and Switch runtime (by way of DOAX3 Recomp, which
shares the game's Koei Tecmo engine).

**Status: early, work in progress.** The game boots and runs at its own 30 fps at 1920x1080 through the title, the
update history, save slot and difficulty selection, with the update's content and the Expansion Pass recognised.
Gameplay hasn't been reached yet. What has been changed, the debugging switches and the open work:
[docs/CHANGES.md](docs/CHANGES.md).

No game files are included or ever committed. You need your own legally obtained copy of the game, its update and
the keys from your own console.

## How it works

The game's ARM64 code (`exefs`) is translated to C by `tools/gen.py` and compiled into `build/mua3game.dll`. The
runtime (`runtime/`) provides what the Switch would: a small Horizon kernel (threads, memory, sync), the OS services
(file system, HID, audio, time, ...), and NVN (the Switch's graphics API) on Vulkan, with a Maxwell shader translator.

| exefs module | What it is | Here |
|---|---|---|
| `main` | the game (Koei Tecmo engine, NintendoSDK 6.5.0) | recompiled: `generated/game` |
| `sdk` | NintendoSDK | recompiled: `generated/sdk`; its system calls and services are host code |
| `subsdk0` | Nintendo multimedia (movie playback) | not loaded: `movie::` is host code on FFmpeg (`runtime/hle/movie.c`) |
| `subsdk1` | NVIDIA glslc (runtime GLSL compiler) | recompiled: `generated/glslc` |

The game data (18.4 GB with the update) is not copied out: `tools/nsp_extract.py` writes `extracted/romfs.src`, and
the runtime reads the data in place from your NSPs (`runtime/romfs_nca.c`).

## Setup (Windows)

1. MSYS2 with UCRT64 clang, make, SDL3, FFmpeg, Vulkan, shaderc (see LAS Recomp's `tools/setup_windows.ps1`), and
   Python 3.12 with a venv: `py -V:3.12 -m venv .venv` and
   `.venv/Scripts/python -m pip install unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0`
   (Pillow: the window and executable icon, made from the game's own icon in your dump at build time).
2. Extract your game (in Git Bash, `export MSYS_NO_PATHCONV=1` first):
   `.venv/Scripts/python -I tools/nsp_extract.py "<base>.nsp" "<your prod.keys>" extracted --update "<update>.nsp"
   --dlc "<expansion pass>.nsp"`. Keep the NSPs where they are: the game data is read from them.
3. In Git Bash with `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`:
   `make gen` (recompile the game code into `generated/`, ~3 minutes), then `make -j16` (~40 minutes the first time).
4. Run: `build/mua3launcher.exe` (the launcher: window mode, resolution, render resolution, upscaler,
   anti-aliasing, aspect ratio and frame rate 30 / 60 / 120, then Play), or `tools/run.sh` (log: `mua3.log`).
   The settings are in `larecomp.ini` next to `extracted/`; the in-game menu (F1 or Esc) changes most of them too.

## Credits

- **LAS Recomp** by Peiikai and contributors (MIT License, `LICENSE.LASRecomp`): the recompiler (`tools/gen.py`,
  `lifter.py`, `nso.py`, `simd_c.py`, ...) and the runtime this project started from.
- Third-party components keep their own licenses (`third_party/`): Dear ImGui (MIT), SMAA (MIT), the ported audio
  renderer and shader decoder references (MIT, `third_party/ryujinx/LICENSE.txt`), fonts (SIL OFL 1.1 / DejaVu).
- Libraries: SDL 3, FFmpeg (LGPL, dynamically linked), Vulkan Memory Allocator, shaderc; the generator uses Capstone,
  Unicorn and python-lz4.
- [Switchbrew](https://switchbrew.org) documentation (NCA/PFS0/RomFS formats for `tools/nsp_extract.py`).

*MARVEL ULTIMATE ALLIANCE 3* is a trademark of Marvel and Nintendo; the game was developed by Koei Tecmo. This project
is not affiliated with Marvel, Koei Tecmo or Nintendo.
