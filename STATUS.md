# Port status (updated with every version)

What changes in each version is in [`CHANGELOG.txt`](CHANGELOG.txt). This file says where the boot stands now, how to
test it and what is known about the game.

## Where the boot stands

- Gets past nninitStartup (heap with virtual memory), the sdk's 112 constructors and the game's 288, and enters nnMain.
- Mounts the RomFS, initializes NVN and Vulkan (device, 3-texture window, vsync), creates about 30 Unity threads and
  uploads the first textures.
- With version 11 (memory ordering of `LDAR`/`STLR`) the main thread keeps going, creates `Loading.PreloadManager` and
  starts loading the first scene, "level0".
- Versions 13-16: gets past "level0" (a lost wakeup in the condvars), creates the shaders (the console's NVN
  properties), presents frames (without Link's Awakening's hooks) and stops crashing (the sdk is no longer loaded inside
  main).
- 0.18.0-alfa: runs at 60 fps and draws the 3D scene (shader translator: conditional EXIT, bindless TEX.B…).
- **Current state (version 19):** you can see the logos, the opening movie (CRIWARE VP9 through `movie::Decoder` with
  FFmpeg), the title screen, the difficulty menu, the new-game cutscene and the scene at Bowser's Keep. Movie audio is
  decoded (Opus through the `hwopus` service with libopus).
- Test route without a controller: `LARECOMP_INPUT="1300:A:8,1600:A:8,1900:A:8,2200:A:8"` (A skips the movie, A on the
  title, in the menu and on the difficulty) and `LARECOMP_SHOT_AT="1450,1750,2600,4800"` → `build/frames/frame_N.png`
  (the game's image, not a desktop capture). `LARECOMP_AUDOUT_WAV=build/audio.wav` dumps the audio.
- 0.20.0-alfa: the characters are visible (Mario, Bowser and Peach in the cutscene and Mario on the castle bridge, frame
  ~5100 of the route); all shaders translate (0 skipped draws).
- **Versions 21-23: it is playable.** After the cutscene, Mario enters the castle with the HUD; the game autosaves
  (`save/save_0/SMR_Save_Ver0011000`), the "Load" screen loads the save (with local time since 22), Mario moves, the
  camera follows him and the first battle (two Terrapins) works: attack, damage, effects and back to the map.
- To play: `play.cmd` (with sound, controller and keyboard; the controls are in the file itself). To test without
  closing anyone's game: `tools/run_test.sh <seconds> <log> VARIABLE=value…`; with `LARECOMP_DATA_ROOT=<folder>` (with
  `extracted` and `build` linked) the test uses its own saves and settings, and `LARECOMP_HIDDEN=1` does not show the
  window.
- **0.24.0-beta:** the application is Super Mario RPG Recomp (`build/smrpg-recomp.exe`, its own icon). Menu (Esc / F1)
  in Spanish with: true widescreen (Unity draws at the monitor's aspect ratio: `runtime/hle/unity_screen.c` +
  `tools/game_overrides.c`), named internal resolutions (720p-4320p), supersampling x2-x16, sharpness (RCAS),
  anisotropic filtering, FPS limit (up to 240; at 120 movement does not speed up) and HDR10.
- Replacing a game function without regenerating: define it in `tools/game_overrides.c` (it is linked first with
  `--allow-multiple-definition`, so it also wins for the internal calls of its generated file).
- **Versions 25-31:** game language (25), texture detail (26), save thumbnails (27), menu in Spanish (28), the whole UI
  in widescreen (29: `CanvasScaler.HandleScaleWithScreenSize` replaced), shader sweep (`tools/shader_sweep.c`: 82,006 of
  the game's 82,032 translate and compile, 30) and HD textures with dumping (31: `<data>/textures` and
  `<data>/textures-dump`, `runtime/nvn/vulkan/texpack.c`).
- **Versions 32-35: AMD FSR.** "AMD FSR" anti-aliasing (native) and upscaling (Quality x1.5, Balanced x1.7, Performance
  x2, Ultra performance x3), with a choice of FSR 4.1.2, 3.1.6 or 2.3.4. The motion vectors are URP 12's (its
  MotionVectorRenderPass, forced in `UniversalRenderer.GetRenderPassInputs`, `tools/game_overrides.c`); the subpixel
  jitter goes in the viewport and FSR runs before post-processing (`runtime/nvn/vulkan/exec.c` fsr_pass_begin,
  `vk_fsr.cpp`). The AMD FSR SDK 2.4 DLLs are needed next to `build/smrpg-recomp.exe` (`amd_fidelityfx_loader_vk.dll`,
  `amd_fidelityfx_upscaler_vk.dll`; they are not in the repository: `third_party/amd-fsr/README.txt`).
- **0.38.0-beta: AMD frame generation** (neural-network FG 4.0.2 on RDNA 4, FG 3.1.6; `vk_fg.cpp`): AMD's swapchain with
  two more queues; configuration and preparation after FSR on the recorder thread; the camera comes from URP's
  view-projection (`exec.c` fsr_camera_scan). The vectors' Y is the other way round from FSR's (measured: with FSR's,
  FG 4 only blends). Without HDR, the window without the runtime's menu (HUDLessColor) keeps the FPS counter from
  warping. Testing: `LARECOMP_FG_PSHOT=N` (what was presented, to `build/fg_pres_*.png`), `LARECOMP_FG_SET`,
  `LARECOMP_FG_MV`, `LARECOMP_FG_DEBUG=2` (AMD's checker), `LARECOMP_FG_CAMDUMP=N`. Needs
  `amd_fidelityfx_framegeneration_vk.dll`.
- **0.39.0-beta:** the videos (title, movies) fill their scaled texture above 1x (before, shrunken in the corner).
  Testing: `LARECOMP_SET` changes video settings at runtime; named window captures through `LARECOMP_HOST_INPUT`.
- **0.40.0-beta:** display mode (windowed, borderless, exclusive fullscreen) and display resolution
  (`runtime/ui/menu.mm` menu_apply_window_mode_now).
- **0.41.0-beta:** the game's aspect ratio (21:9, 16:10) also follows the window at runtime, with a true wide view: AM
  message 31 on change (`runtime/hle/unity_screen.c` rt_unity_screen_follow), Unity's SetResolution (main 0x700062f1d0)
  replaced without its clamp to 1920x1080 or the custom resolution flag (`tools/game_overrides.c`), window textures
  recreated at the new size (`t->screen`, `runtime/nvn/vulkan/textures.c`). Controls guide in Menu > Controls and
  "Configure controls" in Spanish (`runtime/ui/controls_ui.mm`).
- **0.43.0-beta:** the menus' background (the screen capture from Menu.StartScreenCapture in a 1920x1080 RawImage) fills
  the wide screen: RawImage.set_texture replaced, which switches those images to their texture's aspect ratio
  (`tools/game_overrides.c`).
- **0.44.0-beta:** Menu > Video: "Performance now" (CPU, GPU, RAM, disk, video memory, all measured:
  `runtime/perf_meter.c`), the impact of each graphics setting on the five with traffic-light colors and the Save, Undo
  changes, Reset video and Restart game buttons (`runtime/ui/menu.mm`).
- **0.45.0-beta:** in Video, the panel on the right has the meter at the top, the impact of the selected setting above
  the buttons and the buttons at the bottom, always in view (`runtime/ui/option_rows.mm` opt_pane_*).
- Zones checked without playing (11-16, `tools/zone_tour.py`: the save is JSON in UTF-16 and its `_respawn_floor_name`
  decides which scene loads): some 36 zones from all over the map draw correctly and with no skipped draws (Mushroom
  Way, Mario's house, Mushroom Kingdom, Bandit's Way, Kero Sewers, Rose Town and Rose Way, Forest Maze, Moleville,
  Booster Pass and Tower, Marrymore, Star Hill, Sunken Ship, Seaside Town, Land's End, Barrel Volcano outside and
  inside, world map, beanstalk and Nimbus Land, Bowser's Keep); in others Mario appears off the map (the save's
  sentinel position) and they are inconclusive.
- **0.46.0-beta:** "True black" (URP's color grading LUT with its black at 0: `vulkan/exec.c` lut_black_fix),
  "Widescreen movies" (the movies fill 21:9: `runtime/hle/movie_fit.c`) and battles always at 60 FPS
  (`runtime/hle/battle_rate.c`).
- **0.46.1-beta:** AMD FSR works at the internal resolution: at 2160p in 21:9 (5160x2160) FSR 4 costs 3.1 ms per frame
  on an RX 9070 XT (1.4 ms at 3440x1440; FSR 3.1 2.25 ms, FSR 2.3 1.7 ms; SMAA, nothing measurable): 240 -> ~175 FPS.
  The menu's impact panel explains it; the GPU profiler (`LARECOMP_GPUPROF_AT`) gives FSR its own row.
- **0.47.0-beta:** installer (`tools/installer`, `make installer` → `build/dist/SMRPG-Recomp-Installer-<v>.exe`): the
  copy of the game + a folder → copies the program (with nothing from the game), puts the copy in extracted/, builds the
  game code on that PC (`smrpg-recomp.exe --build-game`: lasgen + slimmed-down zig, `runtime/gamebuild.c` with
  `game_overrides.c` first and `-FORCE:MULTIPLE`) and creates the shortcuts. Without --play it also goes straight to the
  game.
- **0.48.0-beta:** the installer builds the game code INSIDE its own process, with no child processes:
  `builder\builder.dll` (`tools/installer/game_builder.cpp`) carries the generator in an embedded Python 3.12 (isolated
  with `python312._pth`), MSYS2's clang 23 and lld as libraries (the same `-cc1` and link lines as `make`) and what they
  include and link (`tools/installer/toolchain.sh`); all it needs from the PC is Windows. Generated C identical to
  `make gen` and objects identical to `make`'s (except for the COFF timestamp). Threads with 64 MB of stack (with 2 MB,
  `MemorySSAUpdater::recursePhi` overflowed it), its own crash net in the compile threads (LLVM's `CrashRecoveryContext`
  was switched off by the faults unicorn causes and catches), lld with `--threads=1` (its thread pool hung process
  exit). Any x86-64 CPU: v3 if there is AVX2, otherwise v2 or x86-64 (`lasgame.dll.cpu`, checked by `runtime/gamelib.c`
  on load). Standalone test: `tools/installer/test_builder.c`. With Control Flow Guard enforced by Windows (LAS Recomp
  #337) unicorn takes the process down: the installer relaunches itself with CFG off through the process creation policy
  (to test it: `tools/installer/with_cfg.c` starts a program with CFG forced on). To test the net:
  `SMRPG_BUILD_TEST_FAIL=<chunk>:1` (access to 0) or `:2` (stack).
- Above 60 FPS: during play the game counts by time (movement is the same at 240 as at 60). Battles do not: their logic
  (BattleManager.update_common) takes one 1/60 s step per frame and at 240 FPS it ran four times faster than their
  animations and their sound; since 0.46.0 the vsync ticks at 60 Hz while a battle lasts. Scripted inputs
  (`LARECOMP_INPUT`) go by frames: at 240 FPS their numbers must be multiplied by 4 (not in battle).
- **0.48.1-beta:** the transitions' fade to black (GS.Fade: entering a zone from the map, a house...) covers the whole
  widescreen: its Image was a 16:9 rectangle and the camera's khaki background or the new zone showed at the sides
  (`runtime/hle/movie_fit.c` wraps GS.Fade.Update, like MoviePlayer.Update). Test route in 21:9: the save in wld00 (the
  map) and A, with `LARECOMP_WINDOW=1720x720`.
- **0.49.0-beta:** no black silhouettes when entering an already seen zone. The code of every shader the game uses is
  saved (cache/shader_code/<hash>.bin, `runtime/nvn/shader_seen.c`) and the background preparation at startup uses it to
  build all the pipelines in our own list before the game draws (f01out, 2nd boot: 0 skipped draws and 0 pipelines
  compiled on entry; before, 9,686 skipped in 2 s). From the android branch (0.8.1-alfa) without the Adreno parts: on PC
  depth is dynamic and the list's lines have no " z=". LARECOMP_SHADER_SEEN=0 turns off the list and the copies.
- **0.49.1-beta:** the first row and the last column of the image no longer come out dark at any resolution: the game's
  own post chain leaves the last row/column of its intermediate buffers unwritten and its final blit blends them in
  (also measured at native 1920x1080, 1:1); the "present" replicates the edge (`vulkan/shaders.c` present.frag — window
  and captures). LARECOMP_NO_EDGE_FIX=1 turns it off. `LARECOMP_SHOT_RAW=1|2` dumps the exact contents of a texture (the
  raw data, without re-rendering) and `LARECOMP_DEBUG_PASS_STRIDE/_FROM/_TO` the passes of a debug frame.
- **0.49.2-beta:** each IPC message is logged only with `LARECOMP_LOG_IPC=1`: it used to fill ~9.4 MB per minute (53,000
  lines per minute, 99% of the log); a whole test run now leaves 36 KB. From the android branch (0.7.2-alfa), without
  the rest of that change.
- **0.49.3-beta:** local memory (LDL/STL) of the size each shader's SPH asks for, instead of always 1 KB zeroed per
  pixel (`maxwell/translate.c`, from the android branch 0.3.0-alfa, only the lmem part): d01f1b's shader with lmem asks
  for 4 words and the frame's heavy passes dropped to half the ms. `LARECOMP_LOG_LMEM=1` (one line per shader with its
  size) and `LARECOMP_LMEM_FULL=1` (always 256, as before).
- **0.49.4-beta:** the installer normalizes the paths in its arguments (/ -> \ and no trailing slash, the drive root is
  kept): with forward slashes it did not create the folder and with --auto it died in 3 s (the \\?\ prefix of its long
  paths does not allow them). Tested with --auto and forward slashes: it creates the folder and builds.
- **0.49.5-beta:** from the translator (android branch): SHFL with its c operand (per-quad texture derivatives: the
  level of detail no longer shoots up), MUFU.SIN/COS with the reduced angle (rr2pi) and the "bra self" with -16
  (LutBuilderLdr out of the loop). The 6 zones: captures within boot-to-boot noise; no cost per frame. ("Clear targets"
  does not exist in the android branch: the diff gives only these three pieces.)
- **0.49.6-beta:** the RomFS table without the extractor's _filelist.json: 1934 files and 6820987323 bytes in total;
  ROMFS_FILES 1934 in launcher.mm and crash_explain.c (both were still at 3836, LAS's number), the walker and the mount
  audit skip the json, and game_romfs_bytes becomes the real sum of the 1934 (the previous "IVFC size" ALWAYS set off
  the damaged files warning). Clean audit on mount.
- **0.49.7-beta:** the fades of the transitions without GS.Fade (FadeWithEffect/DrawCaptureImage from title -> movie and
  EncountEffect from field battles) also cover the whole canvas in widescreen. The transitions still leave bars because
  of their CONTENT (the effect's 16:9 capture and the wipe's geometry), not because of the rectangles: measured at
  1720x720 (80 left / 160 center in the title wipe; 62/137/50 in the battle one); the deep layer (widening the capture)
  is still to be designed. The white peak fills the full width.
- **0.50.0-beta:** Lossless Scaling frame generation (LSFG 3.1) inside the game: Menu > Video > "Lossless Scaling frame
  generation" (x2/x3/x4, "Flow scale", "Performance mode") with its measured impact panel. Separate library
  (tools/lsfg/build.sh windows -> lsfg.dll, from GameNative's lsfg-vk-android 437208a) that the game loads on the fly;
  the shaders come from the player's Steam Lossless.dll (registry + libraries + next to the exe; NEVER in the repo or in
  the installer; without it the game runs all the same and the menu says so: "[lsfg] off", 60.0 fps, 0 skipped).
  Measured (RX 9070 XT, 240 Hz, d01f1b 1280x720): x2 flow 75% quality, GPU 1.07-1.22 ms per real frame, present 0.23 ms,
  VRAM 31.5+21.1 MB, 99.5% presented, 0 failures; x3, 1.73 ms. FIFO swapchain with N+1 extra images; presents from the
  recorder thread (rec_QueuePresentCall/rec_present_direct); does not work with HDR or with AMD's generation. Test with
  and without the DLL in build/lsfg_sin_run.log / build/lsfg_con_run.log / build/lsfg_x3_run.log; the capture of the
  same frame with and without generation does not change (mean 1.4, the zone's noise band).
- **0.50.1-beta:** the installer carries lsfg.dll (ours, from GameNative's lsfg-vk-android, MIT) next to the exe and its
  licenses in their folder (lsfg-vk-android, DXVK's DXBC, pe-parse, volk) with their entry in LICENSES.txt; the player's
  Lossless.dll NEVER. --auto installation tested: lsfg.dll and the licenses installed, and the installed game with x2
  generation finds Steam's Lossless.dll ("[lsfg] active").
- **0.50.3-beta:** the review of 0.50.2 (not released) found the bug: the plan answered 0 both for "not applicable" and
  for "frame with no step" and the caller took a step anyway — at 120/240 the logic ran at the speed of the limit. Fixed
  (-1 = original path; 0..3 = steps, 0 without calling update_common) and measured with a counter of REAL calls to
  update_common: 1395=1395 (57.9/s), 1209=1209 (57.9), 1164=1164 (57.1), 1432=1432 (57.0), 1182=1182 (56.9) at
  60/60/120/240/240; the LARECOMP_BATTLE_FIX=0 control at 60 gives 1176 calls (55.2/s: the battle itself skips 8% of the
  Update phases) — the step distribution ends up equal to or above the original at every limit. Durations of 20.4-25.1 s
  in all runs (the spread between identical runs, ±3-4 s, covers the differences between limits; it is driven by the
  ~900 ms hitch of the entry transition, the engine's own, which shifts the phase of the presses). 8 ms presses at 240
  are not lost; the opening matches (2-4.3%) and the first hit window shows up at all three (2184/2158/2186 pixels at
  t=3330). FIX=0 now logs its battle and its rate too.
- **0.50.2-beta:** battles draw at the "FPS limit" with the logic separated from the drawing: vsync is no longer forced
  to 60 in battle; BattleManager.update (replaced in game_overrides.c) hands out update_common's steps by real time
  (60/s, 0-3 per frame, debt cut off at 4 steps), and presses between steps are held in the controller snapshot
  (ControllerObserver._npadState) for the step they belong to. Measured at 60/120/240: the game draws at the limit
  inside the battle (60/120/240.0 fps), the logic at 57-58 steps/s (the jump on entering the battle trims debt), the
  captures at the same wall-clock time match (2-5%) and the timed-hit labels appear at the same moment at all three
  limits; at 240, 8 ms presses (less than a step) are not lost. BATTLE_FPS=30 and BATTLE_FIX=0 stay as before;
  battle_slow (LSFG) stays consistent. Test: a separate battle script, with its analysis. (Its measurements came from
  the build with the bug the review found: the valid ones, in 0.50.3.)
- **0.51.0-beta:** the installer asks for the already extracted copy of the game (the folder with exefs and romfs, or a
  previous installation): it checks it (main's build ID, main.npdm's title and the RomFS's 1,934 files by hash) and puts
  it in `extracted/` with hard links or by copying it. The program is called SMRPG Recomp. Tested: full installation in
  11 min 42 s with 8 threads, copying to another drive, incomplete copies and copies of another game or another version
  rejected, and the installed game (hidden): title and new-game cutscene. First version of the public repository.
- IL2CPP names (classes, methods and their addresses in main): Il2CppDumper 6.7.46 on `extracted/exefs/main` and
  `romfs/Data/Managed/Metadata/global-metadata.dat` (with `DOTNET_ROLL_FORWARD=Major` on .NET 8).
- Pending:
  - "?" chest without its wood (d02out): after 6 battles in a row in the clearing and going back to the stretch with the
    round hills, only the chest's two studded black bands and the "?" are drawn, not the wooden body. With ONE battle
    and the way back it comes out fine. Three new routes (golpe1/2/3, 2026-11-20, with the tracer for the 10 GS
    functions hooked in, LARECOMP_LOG_CHEST=1): ZERO calls in all three. Ruled out: (1) stopping on the long way back
    (3000 frames to the left after the second battle) leaves the chest in view but NOT within hammer reach — a burst of
    A there does nothing (diff. 4.6% = noise); (2) lengthening the walk to "bump into" the chest catches the enemy in
    the clearing: the burst of A starts a BATTLE (the golpe2/3 captures were left on the battle screen waiting for
    input, not on an item panel: the log went on at 60 fps, 0 skipped draws, no crash, 25,400 frames). New hypothesis:
    the broken chest is the ALREADY OPEN chest (hit before the battles), and what is not drawn is the open chest's body
    on the way back. Route still to be tested: the default script's short way back (150 frames after the first battle:
    Mario reaches the chest), A to open it, close the panel, chain more battles to the right, out to the hills and back.
    Pending: tracing the chest's GS functions (a tracer after dispatch_init in runtime/main.c, separately).
  - Sweep of ALL zones (2026-11-20, completed to the 416 scenes in the addressables catalog; 3 instances at a time, each
    with its own LARECOMP_DATA_ROOT, with extracted linked and its own build): NO real failures — 0 skipped draws and 0
    crashes in the 416, double capture in 390, and the 566 black captures + 6 dark ones are the game's OWN content (pass
    dump with LARECOMP_DEBUG_FRAME: its scene passes at a mean of 0.6 and its UI, in the same passes, at 74): rooms in a
    darkened event state where the game does not let you respawn. The 19 zones without captures are names that are not
    respawn floors and the game itself rejects them ("Your game files are incomplete": e04-e47, d19b4yuka/pp, f01fire*,
    m06kd, m07sky, m3idle...); d04b2bwater and d04b3c/d among them. With 3 instances: 60 fps, max 20-27 ms, no
    degradation. Logs: build/sweep_todo.log (and the sweep_pc*.log); captures in build/zones/.
  - Real controller and audio through the speakers, not tested by me (tests run with `LARECOMP_NO_AUDIO=1`; audio is
    checked with `LARECOMP_AUDOUT_WAV`).
  - Widescreen: the game puts its UI in a fixed 1920x1080 panel; the title background, the dialogs' dimming and the
    title-to-opening fade to white do not reach the sides (the transitions' fades to black cover them since 0.48.1).
  - 22 untranslated shaders, none of which the game uses (VR, wireframe, LookDev, CopyDepth with MSAA, 7 internal to
    main).
  - Frame generation: the game's own UI (battles, the field panel) is interpolated along with the scene (there is no
    "HUD-less" image of the game: it would take the image after post-processing and before the UI); with HDR, the FPS
    counter too. Not tested with the window visible or in fullscreen (tests run hidden).
  - Beyond the first battle, not explored: Mushroom Way uses RGBA4 (version 23, not checked in the game).

### How Unity draws on NVN (what the translator needed)

- Separate textures and samplers: the shader loads the texture word from `c[2][0x168 + 8r]` and the sampler word from
  `c[2][0x568 + 8r]`, ORs them together and samples with `TEX.B` / `TLD.B` (bindless). The translator tracks those words
  (`tex_b_words`) and the renderer puts the handle together (`shader_tex_handle`).
- Vertex shaders discard early with `VOTE.VTG` + `EXIT CC.FCSM_TR`: that EXIT must not execute.

### Link's Awakening runtime details that do not apply to SMRPG

- The `v1_0_0` address table (`runtime/game_addresses/*_v1_0_0.h`) is Link's Awakening 1.0.0's: in SMRPG those addresses
  point to nothing of its own. `g_game_las` (false) turns off whatever reads them every frame; if another failure turns
  up that goes through `GA(...)`, it goes the same way.
- The generator no longer applies LA's patches or hooks to SMRPG (`tools/game_addresses.py GAME = 'smrpg'`). The
  randomizer variants (`tools/rando/patch_variants.json`) are still generated, but without a seed they run the original
  instruction.
- Module bases: main at 0x7000000000 (up to 0x7004dbc000 with its .bss) and sdk at 0x7005000000.

### Reading the hung game's memory with gdb

Guest addresses are process addresses, so gdb reads them as they are (on Windows `long` is 32-bit: use `long long`):

```sh
PID=$(tasklist //FI "IMAGENAME eq smrpg-recomp.exe" //FO CSV //NH | head -1 | cut -d, -f2 | tr -d '"')
gdb -p $PID -batch -ex 'x/8gx 0x750f804b80+0x390' -ex detach -ex quit
```

## How to test a boot

From MSYS2 (UCRT64), in the repository folder, with the game dump linked at `extracted/`:

```sh
(export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"; make -j16)
# (if tools/lifter.py, tools/gen.py or runtime/cpu.h change: "make gen" first, and the whole game is recompiled)
export PATH="/c/msys64/ucrt64/bin:$PATH"   # without MSYS2's /usr/bin in front! (see below)
LARECOMP_HANG_DUMP=20 LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 LARECOMP_FORCE_LAUNCHER=0 \
  timeout 90 ./build/smrpg-recomp.exe --play extracted > build/boot_X.log 2>&1
```

- Gotcha: with `/c/msys64/usr/bin` first in the PATH, MSYS2's `timeout` (mixed with Git's bash) does not pass the
  environment variables to the game: no `LARECOMP_*` gets through. Compiling needs that PATH (in a subshell); launching,
  only `ucrt64/bin`.
- `LARECOMP_FNPROF=0:999999`: each hang dump writes `build/fnprof_hangN.txt` (address and number of times each game
  function ran). `LARECOMP_FNPROF_BT=<addr>,...` shows who calls each one first and with which x0.
- `LARECOMP_WATCH_ADDR=<lo>,<hi>`: which game functions touch that memory (from boot).
- `LARECOMP_CRASH_HOLD=<s>`: after a crash the process waits that many seconds before exiting (for `gdb -p`).

- `init sdk` / `init main`: the constructors. A fatal error ends with the game's stack ("guest stack scan") and the last
  32 host calls of the thread that failed.
- `LARECOMP_HANG_DUMP=<s>`: if the main thread spends that long in a wait, every thread is dumped with its Unity name,
  what it is waiting for, its stack and the kernel trace (the last 2,048 mutex and condvar events).
- Addresses 0x7000000000+ are `main`'s and 0x7005000000+ the `sdk`'s (up to version 15, 0x7004000000+).

## Analyzing the game's code

- `nxnative/dm/tools/nso2elf.py` converts `exefs/main`, `exefs/sdk` and `exefs/subsdk0` to ELF for Ghidra (they are not
  uploaded: they are game code). In Ghidra the image base has to be set to 0x7000000000 (main) or 0x7005000000 (sdk).
- The PLT stubs call through the GOT: to know which function they call, the JUMP_SLOT relocation of their GOT entry has
  to be resolved (that is how the sdk's 8,821 stubs were renamed to `plt_<symbol>`).
- Unity's NVN function pointers are in `main` variables (0x3da5xxx) that its loader fills in with
  nvnDeviceGetProcAddress; they are recognized by their use.
- The `sdk` and `subsdk0` of 1.0.0 are identical to those of 1.0.1 (same build ID), the one nxnative plays: what
  nxnative knows about this game's SDK applies as is, and its `docs/reference/` (threads, memory, rendering) covers many
  already solved Unity-on-Switch problems.

## What is known about the game

- `nn::init::Start` (sdk 0xfa3c0): nnosInitialize → (early call) → nninitStartup → (late call: the constructors of sdk
  and main) → nnMain.
- The NPDM declares 16 MB of "system resource": nn::os virtual memory (VammManager) is enabled. The game's
  `nninitStartup` (main 0x62de90) reserves (total memory − used) ≈ 3.1 GB of virtual space for its `StandardAllocator`
  (object at main 0x3d822d0) and does not use SetHeapSize. `AddressRegionManager` works on the alias region: it keeps
  the first GB and aligns anything large to 1 GB. That is why the alias region is 48 GB.
- MapPhysicalMemory only maps (and zeroes) the missing pages, and only in the alias region.
- `svcBreak` with reason 0 is "Panic" (`nn::diag::detail::AbortImpl`): before it, the game calls am SetTerminateResult.
- Unity (`main`) defines global operator new/delete that replace the sdk's weak ones (interposition, like the rtld).
- nn::ro: Unity only calls Initialize and LookupSymbol (there is no NRO); the rtld does not exist here.
- IL2CPP's GC stops the threads with SuspendThread/GetThreadContext: cooperative pause at safe points.
- NVN 55: the game's `NVNtextureView` takes up 16 bytes; `NVNtexture`, 0x80. Unity asks for 277 NVN functions; 19 are
  missing from the runtime (Barrier, SetAlphaRef, SetPointSize, CopyBufferToBuffer, "Deferred" binds…) and several are
  empty (BindSeparateSampler(s), ReportValue, DrawTexture…): they will have to be written when Unity draws.
- Memory ordering: on AArch64 `STLR` + `LDAR` are seq_cst; on x86 `STLR` has to be translated with a full barrier.
- Unity threads: MainThread, UnityGfxDeviceWorker, Loading.AsyncRead, Job.Worker 0-1, Background Job.Worker 0-15, GC
  Finalizer, AssetGarbageCollectorHelper, BatchDeleteObjects, NetworkInterfaceWrapper, LibcurlGc, an unnamed vsync
  thread (main 0x6b536c) and NxSystemThread (the SDK's).
- `main` imports 31 functions from `subsdk0` (nn::movie). The runtime does not load `subsdk0`: `runtime/hle/movie.c`
  implements them with FFmpeg (version 19: the ones CRIWARE Sofdec uses). Not implemented yet, because no calls have
  been seen: `GetH264NalUnitStartPositionAndNalSize` and the `Extractor` ones called by symbol.
- The videos are CRIWARE USM with VP9 1920x1080 at 60 frames/s and Opus audio (one stereo stream, through `hwopus`). CRI
  creates the decoder with type 3 (VP9), format 3 and mode 2 (native texture).
- CRI asks 196 times for the buffer size of a multistream Opus decoder with 2 mono streams (`hwopus` command 7) without
  opening it: that is its memory calculation, not a bug.
