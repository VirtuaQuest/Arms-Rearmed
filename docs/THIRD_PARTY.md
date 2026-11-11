# Third-party components in the ARMS Rearmed release

ARMS Rearmed's release zip contains **no game code, game data, art, fonts or keys from Nintendo**. The player supplies
their own legally obtained copy of ARMS, its 5.4.0 update and their console's keys; the game code is recompiled from
those files on the player's own PC by the tools below.

ARMS Rearmed is built on LAS Recomp by Peiikai and contributors (MIT License, `LICENSE.LASRecomp`). These components
are bundled and keep their own licenses. Source code for the exact versions shipped is at the links given.

| Component | Version | License | Source |
|---|---|---|---|
| SDL 3 | 3.4.18 | zlib | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18 |
| FFmpeg (LGPL configuration: VP8/VP9/H.264/Vorbis/Opus/AAC decoders, Matroska/MP4 demuxers, swscale; linked dynamically) | 9.0.2 | LGPL 2.1+ | https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz (built by `tools/build_deps_windows.sh`) |
| shaderc (GLSL to SPIR-V) | 2026.4 | Apache 2.0 | https://github.com/google/shaderc |
| libiconv (used by SDL 3) | 1.19 | LGPL 2.1+ | https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz |
| GCC runtime: libgcc, libstdc++ | 16.2.0 | GPL 3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org/ |
| winpthreads (mingw-w64) | 14.0.0 | MIT / BSD | https://www.mingw-w64.org/ |
| Zig (compiles the game code during setup; includes LLVM, Clang, LLD and the MinGW-w64 C runtime) | 0.17.0 | MIT; LLVM: Apache 2.0 with LLVM exceptions; MinGW-w64: see its headers | https://ziglang.org/download/0.17.0/zig-0.17.0.tar.xz |
| CPython (inside armsgen) | 3.12.10 | PSF License | https://github.com/python/cpython/releases/tag/v3.12.10 |
| PyInstaller (packs armsgen) | 6.22.3 | GPL 2.0+ with the bootloader exception | https://github.com/pyinstaller/pyinstaller/releases/tag/v6.22.3 |
| Capstone (armsgen) | 5.0.9 | BSD 3-clause | https://pypi.org/project/capstone/5.0.9/ |
| Unicorn (armsgen; a separate program the launcher runs) | 2.1.4 | GPL 2.0 (Python bindings: BSD) | https://pypi.org/project/unicorn/2.1.4/, https://github.com/unicorn-engine/unicorn |
| python-lz4 (armsgen; includes LZ4) | 4.4.5 | BSD 3-clause; LZ4: BSD 2-clause | https://pypi.org/project/lz4/4.4.5/ |
| PyCryptodome (armsgen) | 3.23.0 | BSD 2-clause / public domain | https://pypi.org/project/pycryptodome/3.23.0/ |
| Dear ImGui (built in) | 1.91.9 | MIT | https://github.com/ocornut/imgui |
| SMAA (shader and lookup textures, built in) | 2013 | MIT | https://github.com/iryoku/smaa |
| AMD FidelityFX FSR 1 (built in) | 1.0 | MIT | https://github.com/GPUOpen-Effects/FidelityFX-FSR |
| Ryujinx-derived code (built in, `third_party/ryujinx`) | - | MIT | https://github.com/Ryujinx/Ryujinx |
| Baloo 2 font (SemiBold) | 1.640 | SIL OFL 1.1 (`Baloo2-OFL.txt`) | https://github.com/EkType/Baloo2 |
| BIZ UDPGothic font (Regular) | 1.05 | SIL OFL 1.1 (`BIZUDPGothic-OFL.txt`) | https://github.com/googlefonts/morisawa-biz-ud-gothic |
| DejaVu Sans font (Book, Bold) | 2.37 | Bitstream Vera license (`DejaVu-LICENSE.txt`) | https://github.com/dejavu-fonts/dejavu-fonts/releases/tag/version_2_37 |

The Windows build takes SDL 3, shaderc, libiconv and the compiler runtime from MSYS2's UCRT64 packages
(https://packages.msys2.org, which links each package's source). The build scripts that fetch and build the rest, with
checksums, are in this repository: `tools/build_deps_windows.sh` (FFmpeg), `tools/fetch_zig.sh` and `tools/zig_slim.sh`
(zig), `tools/armsgen.spec` (armsgen). The release's icon is original (`tools/make_original_icon.py`).

*ARMS* is a trademark of Nintendo. ARMS Rearmed is not affiliated with or endorsed by Nintendo.
