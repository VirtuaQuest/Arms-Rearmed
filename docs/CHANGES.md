# Changes

## ARMS bring-up (2026-11-08, first part)

This repository started as a copy of MUA3 Recomp (mua3 branch of DOA-Xtreme-3-Recomp at 96e0db9, built on LAS Recomp)
and is retargeted at **ARMS 5.4.0** (title 01009B500007C000, main build ID 501924B7C22129E5, NintendoSDK 4.4.3,
NVN 53, NEX 4.3.5). The sections after this one are the history inherited from DOAX3 / MUA3.

**Modules:** `main` (0x1769000 bytes, 64,949 functions) and `sdk` (28,231 functions) are recompiled; `subsdk0` is the
movie library (host code, `runtime/hle/movie.c`); there is no glslc module (`GLSLC_*` removed from `rt.h`,
`loader.c`, `gamelib_entry.c`, the Makefile).

**Setup used:** `.venv/Scripts/python -I tools/nsp_extract.py "<ARMS.xci>" "<prod.keys>" extracted --update
"<update.nsp>"` (under 1 s: exefs, control, `romfs.src`), `make gen PY=.venv/Scripts/python.exe` (~35 s),
`make -j18 PY=.venv/Scripts/python.exe` (~8 min the first time), `tools/run.sh <seconds>` (log `arms.log`).

**Changed for ARMS:**
- `tools/nsp_extract.py`: an XCI base (the root HFS0's `secure` partition; HFS0 entries are 0x40 bytes), key file
  lines that aren't hex are skipped.
- `runtime/nvn/cmdbuf.c`: ARMS gives command buffers 512-byte chunks sized for NVN's own encoding and has an empty
  out-of-memory callback (f_7000980168), so our 88+ byte records ran out ("out of command memory"). Records now go on
  in 64 KB host blocks per guest chunk address (`ovf_next_block`), reused when the game adds that chunk again.
  `LARECOMP_LOG_CMDMEM=<n>` logs the first n AddCommandMemory calls.
- `runtime/services/audio.c`: no render info section in RequestUpdate's output below REV5. ARMS uses REV4 and
  `nn::audio::RequestUpdateAudioRenderer` aborted on the output size.
- `runtime/services/friends.c` (new): friend:u/a/m/v/s with an empty friend list. Without an IFriendService object
  `nn::friends::GetFriendList` crashed on a null session.
- Names and ids: `armsrecomp.exe`, `armsgame.dll`, `armslauncher.exe`, program id, add-on base 0x01009B500007D000,
  the version table, the window title "ARMS Recomp".

**Status (end of 2026-11-08):** boots, runs the title screen, the attract-mode fights and the menus through to
character select at a locked 60 fps on an RTX 3060 / i7-11700F, with keyboard, mouse and controller input; runs of
2+ minutes end without a crash or stall. Fighters, stages, effects, UI and movies render (see "Fixed: the fighters").

## Getting it stable (2026-11-08, second part)

Each of these was found with the tools listed after it, in this order; each fix moved the first failure further on.

- **Command lists overwritten after recording** (the "30 million SET_BLEND_COLOR records" loop of the first part).
  ARMS hands out command memory from per-frame arenas (several command buffers share one arena; temporary ones are
  given the same 2 KB chunk over and over) and places each new chunk at the old one's start + GetCommandMemoryUsed.
  Our records are several times NVN's encoding, so lists ran past what the game thought they used and the next lists
  overwrote them (CallCommands targets changed, render targets became garbage pointers, cycles).
  `runtime/nvn/cmdbuf.c` now keeps all records in host memory: each guest chunk gets host blocks (CmdChunk); the game
  is told an estimate of NVN's own usage (16 bytes + inline data per command); a chunk retires when a chunk is added
  at the same start or another buffer's estimated writes reach its start, then waits in a 6-frame quarantine before
  its blocks are reused (the estimate is only an estimate, and the render thread runs behind).
  `LARECOMP_CHECK_LISTS=1` hashes every list at EndRecording and checks it at submit / Call / Copy (and logs which
  chunk retirement hit it). Found with it.
- **CopyCommands** copies the records (it recorded a call to lists the game reuses right after copying).
- **Virtual memory pools** (`objects.c` MapVirtual): a 96 MB compressible render-target pool is mapped whole into a
  virtual pool; pool offsets resolve through the mappings (textures there had their offset as their address: crash).
- **NVN objects copied by value.** ARMS copies NVNtexture / NVNsampler objects and finalizes copies, sometimes only
  the temporary it initialized. Host heap corruption (0xC0000374; found with full page heap: IFEO GlobalFlag
  0x02000000) came from double frees and reads after free. Now: samplers are interned; a live-object registry
  (`nvn_live_*`) makes a second Finalize harmless and rejects stale pointers (`nvn_texture_of`); texture Finalize only
  marks, and `nvn_texture_gc` destroys a finalized texture once no guest object has named it for 120 presents and no
  pool slot holds it; programs and syncs are kept.
- **Sampler exhaustion → device lost.** Every sampler-pool registration created a VkSampler (ARMS registers every
  frame): NVIDIA's 4000 ran out after ~27 s (Vulkan validation found it; the layer must be registered in
  `HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers` when running elevated). One VkSampler per distinct state now, and
  registrations never take a "backend" pointer from builder memory (garbage handles in vkUpdateDescriptorSets).
- **Movie library of the older SDK:** the 3-argument `movie::Decoder` constructor and `MediaData::FindInt64` were
  missing; the attract movie stopped the thread that called them (the game waited forever at ~27 s).
- **Core exclusion** (`kernel.c`): with every guest thread running in parallel, ARMS's job code walked intrusive lists
  while another thread changed them (null-node crashes in about half the runs, timing dependent). ARMS pins its
  threads to cores (SetThreadCoreMask, now honoured and logged); on Horizon the threads of one core never run at once.
  A pinned thread now holds its core's lock while running guest code and gives it up for kernel waits, sleeps, yields,
  IPC and long host waits (present pacing, GPU fences); a higher-priority waiter preempts at the holder's next HLE or
  kernel call, or runs beside it after 2 ms (a holder in a long computation); unpinned threads float. 4/4 scripted
  runs and 2/2 attract runs were clean afterwards. `LARECOMP_CORES=0` turns it off, `LARECOMP_LOG_CORES=1` logs
  holds over 20 ms.
- **Input:** ARMS reads `nn::hid::GetNpadStates` (plural, a sample history): now HLE'd for every style, so input
  reaches the game.
- **Window crop** (`nvnWindowSetCrop`: ARMS's dynamic resolution renders into the top-left 1600x900 and presents that
  part) is applied at present. (`LARECOMP_SHOT_AT` captures are the raw window texture, uncropped; the window is right.)
- **Shader translator:** TLD4 (gather, depth compare; 2D, 2D array, cube, cube array).
- **MUA3 / Link's Awakening leftovers:** `game_frame_rate` wrote MUA3 addresses (in ARMS's memory: other data) at
  every present - now a no-op; the display runs at 60 Hz (ARMS presents every vsync and steps fights per frame, so
  other rates would change its speed); the overlay menu keeps Controls, Video, Audio, Quit.
- **Keyboard and mouse:** mouse buttons are bindable like keys; extra slots: second keys for A / B, Grab (A + B).
  Defaults follow ARMS's own Pro Controller prompts (its tutorial messages: punch A / B, grab A + B, jump X, dash Y,
  guard left stick press, rush L / R).
- **Launcher** (`launcher/launcher.cpp`, rewritten): GAME FILES (pick the base game, update and keys; INSTALL runs the
  Python environment, extraction, `make gen` and the build as child processes in a job object, with a live log),
  PLAY, GRAPHICS (window mode / size, internal resolution, upscaler, SMAA, aspect, FPS counter, volume, rumble),
  CONTROLS, ABOUT. Original look: drawn shapes, springs and halftone, Baloo 2 (OFL). Test hooks:
  `ARMS_LAUNCHER_SHOT`, `ARMS_LAUNCHER_TAB`, `ARMS_LAUNCHER_INSTALL`, `ARMS_LAUNCHER_LOG`.

## Fixed: the fighters (2026-11-08, third part)

The fighters were black in fights and missing at character select. Per-pass dumps (`LARECOMP_RENDER_THREAD=0
LARECOMP_DEBUG_FRAME=<frame>`: every pass's first colour target to build/pass_NNN.png) showed them rendered correctly
in the scene pass and lost in two later steps:
- **The composite** (fs `ae216330c4409988`) grades fighter pixels (R8 mask == 1) with a 3D LUT the game renders, an
  8x8x8 target drawn one slice at a time through layer views. The Vulkan backend refused 3D colour attachments
  ("not used by the game"), so that LUT stayed black. 3D colour images are now 2D-array compatible and a slice
  renders through a 2D view (`textures.c` vk_view).
- **Character select** shows the fighter through a UI pane that samples the frame copied into a texture - a
  different NVNtexture object over the same memory than the one the copy wrote (ARMS makes new texture objects each
  frame). Each object had its own VkImage, so the pane read an empty one. Texture objects that describe the same
  memory the same way (address, format, target, size, levels, samples, stride) now share one image, reference
  counted (`textures.c` share_*; `LARECOMP_VK_NO_TEX_SHARE=1` turns it off).
- `nvnCommandBufferSetDepthClamp` is applied (pipeline key, `poly_fill` bit 1).
Debugging switches added for this: `LARECOMP_LOG_DRAWSTATE=<fs hash>[,...]`, `LARECOMP_NO_DEPTH_TEST_FS=<fs hash>`,
`LARECOMP_LOG_FSHASH=2`; the copy trace prints each texture's memory address.

**Runtime DLLs:** `make dlls` (part of `make all`, and of the launcher's build step) copies the MSYS2 DLLs the game
and the launcher need next to them, so they start outside an MSYS2 shell.

**Also not handled yet:** `AddControlMemory`, `SetShaderScratchMemory`, `SetDepthClamp`, `SetTiledCacheAction/TileSize`
are stubs; `prepo:u` is a logging stub; motion controls (six-axis) report nothing (button controls only).

## Smooth start, no pop-in, every mode tested (2026-11-08, fourth part)

Reported after a play through the tutorial: 3-4 fps while the game started, and fighters / models appearing a moment
after they should.

- **Shaders are prepared before the game shows.** Extraction (`tools/nsp_extract.py` extract_shaders) unpacks the
  game's shader code (BNSH in the Yaz0 SARCs under Shader/, Layout/, Effect/, System/) into `extracted/shaders`; the
  startup preparation indexes it and builds the pipeline variants in `runtime/nvn/shader_variants.txt` (1870
  records: shader hashes and pipeline state, no game data) plus the player's own `cache/shader_seen.txt`, before the
  window shows the game (the title says "preparing shaders N%"). Cold cache: ~1790 pipelines in ~12 s on an
  RTX 3060; warm: ~4 s. `LARECOMP_PREP_ONLY=1` builds the caches and quits: the launcher's INSTALL / REBUILD run it as
  "Prepare the shaders".
- **No pop-in.** Draws whose program or pipeline is still building wait for it (`exec.c` prog_wait, up to 3 s; the
  recorder waits for pipelines, `LARECOMP_VK_RECORDER_WAIT_PIPES` on by default) instead of being skipped for a few
  frames. `LARECOMP_SKIP_BUILDING=1` is the old behaviour.
- **Every mode played** with scripted input (`LARECOMP_HOST_INPUT`, contact sheets of `LARECOMP_SCREENSHOT_EVERY`):
  Grand Prix (a round to KO), Versus Fight, Team Fight, V-Ball, Hoops, Skillshot, Hedlok Scramble, 1-on-100,
  ARMS Test, Training (the lessons), and the menu screens Badges, Gallery, Replays (the test matches were recorded),
  Dashboard / Stats, Get ARMS (asks for coins, as it should with this save) and Ranked ("win the Grand Prix to
  unlock", as it should). All run at 60 fps without errors. The variants these runs added (145) are in the shipped
  list: Hedlok Scramble went from 121 first-visit shaders on a fresh cache to 4, and on a cold cache a new
  combination in play costs 30-50 ms once instead of 120-200 ms.
- **Local crashed the game.** `ldn:u` (local wireless) fell to the generic logging stub, which answered
  CreateUserLocalCommunicationService with no object: a null session. `runtime/services/net.c` now has ldn:u as on a
  console in airplane mode (Initialize: result 203-23, GetState: Error); the game shows its empty local lobby list.
- **Online and system screens say why they go back.** Party Match / Friends ask for a connection (`nifm` answers
  "none", now logged), Help opens the web browser, the eShop screens are library applets: none exist here, and the
  game went back without a word. `menu_notice` (`runtime/ui/menu.mm`) shows a 5-second line at the bottom of the
  screen ("Online play isn't available in ARMS Recomp.", local wireless, web pages, eShop). The game's own quiet
  connection check while it starts shows nothing.
- **Crash dialog:** `crash_explain.c` compared the game's file count with MUA3's (3836), so every ARMS crash said "Your
  game files are incomplete". The count isn't checked for ARMS (its versions differ; 5.4.0 has 3449 files).

What this project changed relative to [LAS Recomp](https://github.com/peiikai/LASRecomp) (its recompiler and Switch
runtime), why, and where. It also lists the debugging switches, the test routes, the known gaps and the next steps.
It started as DOAX3 Recomp (the sections from "Project setup" to "Test routes" are that work, which MUA3 inherits:
both games are Koei Tecmo's engine) and was retargeted at MUA3 on the `mua3` branch.

## Retargeted at MARVEL ULTIMATE ALLIANCE 3 4.0.1 (`mua3` branch)

- **The game:** title 010060700AC50000, the update's code (4.0.1, NintendoSDK 6.5.0, main build ID E853C44FDF18B88F).
  The DLC characters and stories ship in the update; the Expansion Pass (010060700AC51001) is an entitlement whose
  data is one empty file.
- **`tools/nsp_extract.py`:**
  - `--update`: the update's exefs, and its RomFS as a Bktr patch over the base game's (indirect storage remapping
    ranges to the base or the patch, and AesCtrEx: the patch's counter generation by range);
  - `--dlc`: each add-on's RomFS image to `extracted/aoc/<id>.romfs`;
  - `extracted/romfs.src` instead of an 18.4 GB copy: where the image is in the NSPs, the keys and both tables
    (`--romfs extract` still copies every file; `--files <text>` copies the matching ones). Checked against the RomFS's
    own SHA-256 level: 602 random 16 KB blocks from both storages, no mismatch.
- **`runtime/romfs_nca.c`:** reads the image in place (AES-128-CTR on AES-NI); `runtime/services/fs.c` uses it when
  `romfs.src` is there.
- **Module layout:** `main` is 0x4fc1000 bytes, so `sdk` moves to 0x7006000000 and glslc to 0x7007000000. glslc is
  `subsdk1` here (`GLSLC_NSO`); `subsdk0` is the multimedia library, still host code.
- **Services:** aoc reports the add-ons whose images were extracted and fsp-srv's OpenDataStorageByDataId serves
  them; GetDisplayVersion reads `extracted/control/control.nacp` (the title shows "Ver. 4.0.1").
- **Movie:** `movie::Buffer::SetNvnDev` / `SetNvnSync` are no-ops (the host decoder copies into guest memory).
- **Shader translator:** TLD4S (2D gather, with .AOFFI / .DC), HSETP2's constant-buffer and immediate forms, and VOTE
  (subgroup ballot / all / any / all-equal; needs `GL_KHR_shader_subgroup_vote` / `_ballot`).

## Project setup (ae4fa76)

- **Retargeted at DOAX3 Scarlet 1.0.0** (title 01009CC00C97C000, NintendoSDK 6.4.2). The Link's Awakening specific
  code is removed (warp, mirror mode, widescreen camera, pots, pets, randomizer, save editor, extra flags).
- **Game hooks:** `runtime/hle/game_hooks.c` and `runtime/ui/game_panels.mm`. Each hook point the shared runtime calls
  answers "nothing special" until a DOAX3 counterpart is written. `game_in_play()` returns 0, because the game's
  in-play state isn't located yet.
- **Three recompiled modules** (`Makefile`: `make gen`, then `make -j16`):

  | Module | What it is | Output |
  |---|---|---|
  | `main` | the game | `generated/game` |
  | `sdk` | NintendoSDK | `generated/sdk` |
  | `subsdk0` | NVIDIA glslc, base 0x7005000000 | `generated/glslc` |

  `subsdk1` (multimedia) is not loaded; `movie::` is host code instead.
- **Tools:**
  - `tools/nsp_extract.py`: extracts exefs, romfs and control from an NSP using your own `prod.keys`. It handles a
    base game only (no update/BKTR patch support yet).
  - `tools/sym.py` and `tools/addr2sym.py`: name guest addresses.
  - `tools/run.sh [secs]`: runs the game; the log goes to `doax3.log`.

## Kernel, HLE and services (ae4fa76)

- **Fibers:** `nn::os` fibers, in `runtime/hle/fiber.c`.
- **Offline network** (`runtime/services/net.c`), as on a console with no connection:
  - nifm turns every connection request down;
  - bsd fails to create sockets;
  - ssl hands out contexts that never get used.
- **Library applets** (controller strap guide, error dialogs): skipped and logged, in `runtime/hle/applets.c`.
- **`movie::MediaData::FindRect`**, in `runtime/hle/movie.c`.
- **Ids:** the program id and the add-on content base id (0x01009CC00C97D000) are DOAX3's, in
  `runtime/kernel.c` (GetInfo) and `runtime/services/misc.c` (aoc). aoc reports no DLC: the base dump is the *Basic Free Edition*.

## Audio (`runtime/services/audio.c`)

- **ae4fa76:**
  - The audren work buffer is sized as the SDK expects.
  - Splitter routing works; voices routed through splitters were silent before.
  - The BufferMixer effect is implemented.
- **3aceb94: Aux effect (type 2)**, ported from the SDK module's own code (`nn::audio::server::AuxInfo::Update`,
  `CommandGenerator::GenerateAuxCommand`, `dsp::WriteAuxBufferDsp` / `ReadAuxBufferDsp`):
  - The effect's input goes into the send ring.
  - Its output is replaced by what the game wrote into the return ring.
  - A disabled effect resets both rings and passes the input through dry.
  - Verified: the return is the send delayed by 2400 samples, correlation ~1.0, no dropouts.

  Parameter layout:

  | Offset | Field |
  |---|---|
  | 0 | in[24] |
  | 0x18 | out[24] |
  | 0x30 | channels |
  | 0x34 | rate |
  | 0x38 | ring sample count |
  | 0x40 | send info address |
  | 0x50 | return info address |

  Ring state is at info+0x40 (read offset, then write offset); int32 samples start at info+0x80.

## Graphics: NVN on Vulkan, Maxwell shader translator (ae4fa76)

- **Shader translator** (`runtime/nvn/maxwell/translate.c`):
  - separable programs;
  - shared memory (LDS/STS);
  - image load/store (SULD/SUST);
  - stencil sampling, in compute shaders only (`ShaderKey.tex_stencil_mask`).
- **Textures** (`runtime/nvn/vulkan/textures.c`):
  - storage image views (`vk_storage_view`);
  - buffer textures (target 10, made a 1D image);
  - 32-bit integer formats viewed as float;
  - a 1x1 2D texture bound in a cube slot becomes a 1x1 cube of its texel (`cube_of_1x1`); before, it was a black dummy.
- **Present pacing:** `present_pace` in `runtime/nvn/objects.c` paces to the game's present interval (2, so 30 fps).
- **Resolution:** native 1x by default. Scaled targets broke the deferred lighting compute pass, which stores at the
  game's pixel coordinates.

## Shader preparation at startup (efffec6)

- **Problem:** LAS Recomp's preparation (build every pipeline the game used before, ahead of the game) never ran.
  - It started from LA's launcher.
  - It looked for shader code where DOAX3 doesn't keep it; DOAX3's shaders are inside its packed data archive.
- **Code store:** each stage's code is kept in `cache/shader_code/<hash>.bin` the first time the game builds it.
  This is on the player's machine only and never committed.
- **Preparation:** it runs at device init from `cache/shader_code` and `cache/shader_seen.txt`.
- **Compute records** carry their stencil slots (` st=`), so the lighting shader is prepared too.
- **Result:** a second visit to the hotel prepares 40 variants in 0.24 s, with no first-visit compiles. Before, the
  first visit skipped ~1250 draws over 2 s.

## GPU waits the game can't observe (b18b47d, `runtime/nvn/vulkan/exec.c`)

- **Problem:** DOAX3's loader copies each texture out of a staging texture and calls `nvnQueueFinish` ~350 times per
  load.
- **Fix:** the copies snapshot guest data when translated. So the game's waits (QueueFinish, SyncWait) now stop at the
  latest submission with observable work: draws, dispatches or ReportCounter.
  - `obs_seq` records that submission; `note_observable()` updates it; `observable_limit()` applies it.
- **Result:** load-screen stalls dropped from 115–240 ms to 60–110 ms.
- **Also:** `runtime/nvn/slowframe.c` can log slow frames during loads.

## Opening movie (67d7a94, `runtime/hle/movie.c`)

- **Problem:** the opening started black, then crashed on its first frame. SDK 6.4.2's movie API differs from LA's.
- **Method:** the vtable order came from resolving `subsdk1`'s `_ZTVN5movie*` relocations.
- **Extractor:** slot 2 is `SetDataSource(const char *path)`, with the path in x1.
- **Decoder:**
  - The game creates its textures and buffers on the "format changed" event (event 2). So the first video frame
    signals event 2, and output is held until the game has called `GetOutputFormat`.
  - The format includes `stride`.
  - Frames are copied as packed NV12 (Y at stride = width, then UV) into the buffer the game passes to
    `GetOutputBuffer`.
  - Slot 17, `GetOutputBuffer(int, Buffer*, MediaData*)`, is implemented.
- **Debugging aids added:**
  - `LARECOMP_TRACE_CALL` (`runtime/dispatch.c`).
  - `LARECOMP_FNPROF` now writes its profile on a crash (`runtime/fnprof.c`, `runtime/platform/windows.c`).
- **When it plays:** only on a fresh save, when the first vacation starts.

## Test routes (7443143, `tools/route.sh`)

DOAX3's routes, since replaced by MUA3's (see "MUA3: into the first level").

`tools/route.sh <route> [secs] [shot frames]` runs the game with scripted input; shots go to `build/frames/`.

| Route | Save | Path | Time |
|---|---|---|---|
| `menus` | existing | title, hotel room, casino | ~160 s |
| `volleyball` | fresh | opening movie, Kasumi's arrival, Niki Beach, Beach Volleyball | ~420 s |
| `relax` | fresh | as `volleyball`, then the shower scene, evening, night | ~330 s |

Fresh-save routes move `save/` to `save.route-backup/` and put it back afterwards.

## Debugging switches (environment variables)

Added in this project:

| Variable | Effect |
|---|---|
| `LARECOMP_FENCE_ALL=1` | full GPU waits (turns off the observable-work shortcut) |
| `LARECOMP_SLOW_FRAME_ALL=1` | log slow frames during loads too |
| `LARECOMP_TRACE_CALL=<hex>` | log a guest function's arguments x0–x3 and result (first 200 calls) |
| `LARECOMP_AUX_DRY=1` | leave the Aux effect out |
| `LARECOMP_AUX_LOG=1` | Aux send/return levels every 1000 frames |
| `LARECOMP_AUX_CAPTURE=<file>` | save Aux channel 0's input and output |
| `LARECOMP_AUDIO_LEVEL=1` | output peak and RMS about once a second |
| `LARECOMP_LOG_STAGES=<n>` | the first n shaders' headers and their stages |
| `LARECOMP_SHADER_SURVEY=1` | log every unknown instruction in a shader |
| `LARECOMP_PRESENT_PACE=0` | uncapped frame rate |

Inherited from LAS Recomp and used here:

| Variable | Effect |
|---|---|
| `LARECOMP_INPUT="frame:BUTTON:frames,..."` | scripted input |
| `LARECOMP_SHOT_AT=<frames>` | screenshots at those frames, to `build/frames/` |
| `LARECOMP_COPY_TRACE=<frame>` | per-draw and per-copy trace of one frame |
| `LARECOMP_SAVE_MSL=1` | keep translated shaders in `build/glsl/` |
| `LARECOMP_FNPROF=<from>:<to>` | guest function-entry profile |
| `LARECOMP_SHADER_PREP=0` | no shader preparation at startup |
| `LARECOMP_SYNC_SHADERS=1` | compile on first use, blocking, instead of skipping draws for a few frames |

## MUA3: into the first level (2026-11-01)

- **Saving:** the game's saves never reached disk: the stub `flags_ext_save_written` hook (`runtime/hle/game_hooks.c`)
  returned false, which `fs.c` takes as a failed write of any file named `*savedata*` (MUA3's `savedatainfo`).
- **Shader translator:**
  - LEA / LEA.HI (64-bit address math);
  - CAL / RET: a short subroutine without control flow of its own is inlined at the call, others go through a return
    stack in the `switch (pc)` machine (the big effect compute programs have ~3000 calls);
  - RED (global reduction, through the ATOM path);
  - DMUL / DADD / DFMA / DSETP: GLSL doubles in register pairs (`shaderFloat64` is enabled).
- **BRX jump tables in programs over 64 KB:** the table reader took any constant offset of 0x10000 or more for "no
  constants known", so the effect compute programs (~90 KB) had every instruction as a `switch` case (9000 cases) and
  the driver never finished building them; the startup preparation waited on them forever (no window). Now 214
  cases, built in ~7 s once, cached after.
- **Movies from the romfs image:** with the game data read in place from the NSPs there is no host file for FFmpeg
  to open. `romfs_file_find` / `romfs_image_read` (`fs.c`) give the movie player the file's range in the image, read
  through an FFmpeg custom reader. The opening (EV_0101_01.mp4, H.264 + AAC, ~3.5 minutes) plays; + pauses it and X
  skips it.
- **Indirect draws:** `DrawArraysIndirect` / `DrawElementsIndirect` were recorded and then dropped. They now draw
  with their arguments read on the GPU from the imported heap (`vkCmdDraw(Indexed)Indirect`, the index buffer bound in
  place without the vertex cache). `LARECOMP_VK_INDIRECT_CPU=1` reads them on the CPU at replay instead.
- **Also replayed now:** `ClearBuffer` and `SignalEvent` (both were dropped). The no-op command buffer stubs log
  their first call ("first call, not implemented").
- **`tools/route.sh`:** the MUA3 routes `menus` and `kree` (fresh save to the first level, the Kree ship, in ~130 s).

### The dark first level: a geometry shader that was dropped

The level first ran with black character silhouettes and no surface detail (only fog, the spotlight's volume and
emissive screens). The cause:

- The frame is a depth-only pass (456 direct draws), a G-buffer pass with five colour targets, then lighting. In the
  G-buffer pass each object is `SetRenderEnableConditional(mode 1, record)` + `DrawElementsIndirect(record + 0x10)` +
  `SetRenderEnable(1)` (game `f_70004360f0` -> `f_7000840e70`), and the indirect draws' counts and index buffers
  were all zero.
- The depth-only draws' program (vs 72b4b92f00, fs 72b4b94e00) has a **geometry shader** (72b4b94f00) that emits no
  vertices: per triangle it tests visibility, reserves room with a warp-aggregated atomic add on storage buffer 0
  (the object's record: the indirect count) and stores the triangle's three vertex indices (`gl_VertexIndex`, the
  vertex shader's output 0) to storage buffer 1 (the indirect draw's index buffer). The Vulkan backend draws only
  the vertex and fragment stages, so it was silently dropped.
- **Fix:** a geometry shader that stores to memory and has no OUT is replayed on the CPU when its draw is replayed:
  the draw's indices plus base vertex go to storage buffer 1 and the count is set (every triangle kept: no culling),
  and the draw rasterizes nothing, as the shader emits nothing. `LARECOMP_GS_CULL_EMU=0` turns it off. Programs
  whose geometry / tessellation stages are dropped are logged once each.
- Diagnostics added on the way, all off by default: `LARECOMP_LOG_INDIRECT=1` (indirect arguments and indices at
  replay, a record scan every 2 s), `LARECOMP_WATCH_INDIRECT=1` / `=2` (write-protect / no-access the first record's
  page and log each writer's / the game's readers' module and stack), `LARECOMP_DEBUG_OPS=1` (with
  `LARECOMP_DEBUG_FRAME`: every non-state command of the debug frame), `LARECOMP_LOG_DEVICE_INTEGER=1`,
  `LARECOMP_DUMP_GS=1` (the first dropped geometry shader's code to `build/gs_code.bin`).

## Frame rate, launcher, window modes, icon (2026-11-01)

### Frame rate setting (30 / 60 / 120)

- **How the game paces itself:** MUA3 never asks for the vsync event. At the title `present_pace` (interval 2) holds it
  at 30; in levels its own limiter (f_7000839000) sleeps until frame start + 1 / its target rate (30.0 at
  `0x70045a7158 + 0x98`, a global frame object; its logic frame count at + 0x68).
- **Its time step:** `dt = max(measured, step)` capped at 1/30 s, where `step` is 1/60 s for its frame interval 1 and
  1/30 s for 2 (a table at 0x70019f33d8; the interval at `[[0x7002188a00] + 0x60]`, 2 in MUA3). Just running faster
  therefore played at double speed (every frame still advanced 1/30 s). A separate step count (`0x1e566c` in the
  world, forced to 2) only sets the present interval and a render-thread wait timeout.
- **The setting** (`frame_rate` in larecomp.ini; the launcher; "Frame rate" on the menu's Display tab;
  `LARECOMP_FPS=30/60/120` overrides; an old `fps60 = 1` reads as 60): the display period is half the frame time
  (`g_vsync_period`, runtime/hle/vi.c: `present_pace` keeps its interval-2 grid), and every frame
  (game_hooks.c `game_frame_rate`) the limiter target becomes the rate, the frame interval 1 above 30, and at 120
  the interval-1 step 1/120 s.
- **Measured** (`LARECOMP_LOG_GAMETIME=1`: game time per 4 s of real time, from the logic frames): 3.90 s at 30
  (117 frames), 3.88 s at 60 (228), 3.78-3.86 s at 120 (~105 logic frames a second, ~65-100 shown: see Known gaps).
  Six minutes of play at 60 had no crash.
- **Render thread costs cut on the way:** CPU-written linear textures (six 1024x1024 RGBA8, two a frame) are copied
  to their images by the GPU from the imported heap and not hashed when the OS's write tracking says they changed;
  buffer textures (the light lists) are copied the same way instead of hashed. Texture time 525 -> 58 ms per 2 s,
  render thread ~10-12 -> ~5 ms a frame.
- **Test switches:** `LARECOMP_FPS_AT=<frame>:<hz>` changes the display rate at a presented frame (a frame-scripted
  route still gets through the loads); `LARECOMP_LOG_SLEEPS=1` (the game's sleeps and timed waits of 5 ms or more),
  `LARECOMP_LOG_LIMITER=1`, `LARECOMP_LOG_DYNTEX=1`.

### Launcher (`launcher/launcher.cpp`, `build/mua3launcher.exe`)

- SDL 3 + Dear ImGui (SDL renderer). Window mode (windowed / borderless fullscreen / exclusive fullscreen),
  resolution (the window's size, or the display mode at its highest refresh rate for exclusive), render resolution,
  upscaler, anti-aliasing, aspect ratio, frame rate, the FPS counter; Play saves and starts `mua3recomp.exe` from the
  data folder (the one with `extracted/`, next to the launcher or one up) as `tools/run.sh` does.
- It changes only its own keys in larecomp.ini and keeps every other line.
- Test switches: `MUA3_LAUNCHER_SHOT=<file.bmp>` (save its window, quit), `MUA3_LAUNCHER_PLAY=1` (press Play).

### Window modes (runtime/nvn/vulkan/device.c `ui_apply_window_mode`)

- New keys `exclusive_fullscreen`, `window_width`, `window_height` next to `fullscreen`: windowed at the chosen size;
  borderless (the desktop's mode); exclusive (the display mode closest to the chosen size, the desktop's if none, at
  that size's highest refresh rate). F11 / Alt+Enter still toggle fullscreen, in the chosen fullscreen kind.
- Checked: windowed 1600x900 from the launcher's settings. The fullscreen kinds weren't run here (they take the
  display over); they're SDL's own calls.

### Icon

- `tools/make_icon.py` makes `build/icon.png` (the window's and the launcher's, loaded at run time) and
  `build/icon.ico` (in both executables, `windres`) from `extracted/control/icon_*.dat`: the game's own home-menu icon
  from your dump, never committed. The window's title is "Marvel Ultimate Alliance 3 Recomp".

## Status (MUA3, 2026-11-01)

- **Runs at 29-30 fps at 1920x1080** (i9-10900, RTX 3080 Ti) from the title through save creation, the opening movie
  and the first level (the Kree ship), with saving working. No crash in runs of up to 8.5 minutes, six of them
  playing (`tools/route.sh play`: moving, attacking, jumping, switching heroes).
- **Checked correct:** title, update history, save slot list and dialog, difficulty, save creation, the opening movie,
  loading tips, the first level's rendering (characters, surfaces, lighting, spotlight, effects), HUD and dialogue.

## Known gaps and next steps

- **Culling:** the geometry shader's triangle culling isn't done (every triangle is drawn). A compute version of it
  would cut the G-buffer pass's work if the GPU ever becomes the limit.
- **Frame rate:** the title and menus hold 29-30 fps when nothing else uses the GPU. The 3-15 fps stretches seen in
  testing lined up with other recomps running on the same GPU (~60 ms GPU time slices in `LARECOMP_GPUPROF_SLOW`
  profiles), so check for them before trusting a measurement.
- **Compute:** all three title-screen compute shaders now translate (ATOMS, ATOM, imageSize through TXQ; lane id and
  lane masks were 0). The tiled light culling shader (`cs 72b4b35500`) takes 1.5-10 ms; its light lists are read by
  the lighting pass from the guest heap in place, which is worth moving to device-local memory. The lane masks
  assume 32-lane subgroups (NVIDIA); AMD's wave64 would need subgroup size control.
- **Frame rate:** 30 / 60 / 120 are a setting (above). At 120 the RTX 3080 Ti shows ~70-100 fps in the first level
  while other GPU work runs alongside: the GPU (~8-11 ms a frame, the tiled light culling 3.3 ms of it: its 8.4 MB
  light lists go through the dispatch's device-local copy and back every frame) is the limit now.
- **Build time:** `generated/glslc/glslc_188.c` alone took over 15 minutes at -O1; splitting that chunk would cut the
  first build a lot.
- From DOAX3, still open: `game_in_play` and level hooks (slow-frame classification treats every frame as a load),
  stencil sampling in graphics stages, 8- and 16-bit integer texture formats, swkbd offsets.
