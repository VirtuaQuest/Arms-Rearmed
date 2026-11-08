# ARMS Recomp

A work-in-progress static recompilation of *ARMS* (Nintendo Switch, 5.4.0: the base game with its last update) into a
native Windows x86-64 program, built on [LAS Recomp](https://github.com/peiikai/LASRecomp)'s recompiler and Switch
runtime (by way of MUA3 Recomp and DOAX3 Recomp).

**Status: early bring-up, not playable.** The game boots through its startup and starts drawing, then stops a few
seconds in. What has changed and the next steps: [docs/CHANGES.md](docs/CHANGES.md).

No game files are included or ever committed. You need your own legally obtained copy of the game, its update and
the keys from your own console.

## Setup (Windows)

1. MSYS2 with UCRT64 clang, make, SDL3, FFmpeg, Vulkan, shaderc (the packages in LAS Recomp's
   `tools/setup_windows.ps1`), and Python 3.12 with a venv: `py -V:3.12 -m venv .venv` and
   `.venv/Scripts/python -m pip install unicorn==2.1.4 capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0`.
2. Extract (in Git Bash, `export MSYS_NO_PATHCONV=1` first):
   `.venv/Scripts/python -I tools/nsp_extract.py "<ARMS.xci or .nsp>" "<your prod.keys>" extracted --update "<update>.nsp"`.
   Only exefs and control are copied; the game data is read in place from your files through `extracted/romfs.src`,
   which holds your title key: keep it private.
3. In Git Bash with `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`:
   `make gen PY=.venv/Scripts/python.exe` (about 35 s), then `make -j18 PY=.venv/Scripts/python.exe` (about 8 minutes
   the first time).
4. Run: `tools/run.sh [seconds]` (log: `arms.log`).

## Credits

- **LAS Recomp** by Peiikai and contributors (MIT License, `LICENSE.LASRecomp`): the recompiler and the runtime.
- Third-party components keep their own licenses (`third_party/`).
- [Switchbrew](https://switchbrew.org) documentation.

*ARMS* is a trademark of Nintendo. This project is not affiliated with Nintendo.
