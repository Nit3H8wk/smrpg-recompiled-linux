# SMRPG Recomp: quick guide

**English** · [Español](GUIDE.es.md)

An unofficial static recompilation of Super Mario RPG (Switch, 1.0.0) for PC with Vulkan. It needs the files of your own
copy of the game, already extracted (the `exefs` and `romfs` folders), which are not in this repository. A fan project,
not affiliated with Nintendo or Square Enix; Super Mario RPG is a trademark of Nintendo.

## Installing (to play without the development environment)

`SMRPG-Recomp-Installer-<version>.exe` (from the GitHub release, or `make installer` → `build/dist/`): a window where you
choose **the folder of your extracted copy of the game** (the one with `exefs` and `romfs`, version 1.0.0; an earlier
installation works too) and the **installation folder**. When you press Install:

1. it checks your copy before touching anything (Super Mario RPG 1.0.0: the build ID of `exefs/main`, the title and the
   1,934 RomFS files by path and size) and puts it in the installation's `extracted/`: with hard links if it is on the
   same drive (no extra time or space) and, if not, copied;
2. it copies the program (the executable, its DLLs and `builder\`: **nothing from the game**);
3. it builds the game code **on your PC and inside the installer itself**, without opening other processes:
   `builder\builder.dll` carries the generator (in a bundled Python), clang and lld, translates the game's program to C
   and compiles it into `extracted/lasgame.dll` for that PC's processor (with AVX2 if it has it; without it, it works
   too); about 13 minutes with 16 threads (updating an installation, seconds). Nothing else is needed besides 64-bit
   Windows 10 or 11;
4. it creates the shortcuts (desktop and Start menu) to `smrpg-recomp.exe --play`.

It asks for about 11 GB free (4 if the copy is on the same drive) and a path of at most 110 characters (the game does
not use long Windows paths). If it is interrupted, it resumes; installing over an earlier installation (with the game
closed; the installation itself works as the copy of the game) updates it without touching the saves (`save/`) or the
settings (`larecomp.ini`). Logs: `install.log` and `build.log` in the folder.

The installer is in Spanish when Windows is, and in English otherwise (`--lang en|es` forces one).

To build the installer (from MSYS2 UCRT64, after `tools/setup_windows.ps1`):
- the `mingw-w64-ucrt-x86_64-clang`, `-lld` and `-llvm` packages of MSYS2 (tested with LLVM 23) and `make deps-windows`
  (the LGPL FFmpeg);
- the three AMD FSR DLLs (`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` and
  `amd_fidelityfx_framegeneration_vk.dll`) in `build/`, from `Kits/FidelityFX/signedbin/` in AMD's FidelityFX SDK
  (`third_party/amd-fsr/README.txt`);
- `build/lsfg/windows/lsfg.dll`: the code of [lsfg-vk-android](https://github.com/GameNative/lsfg-vk-android) (release
  branch, commit 437208a, with its submodules) in `../deps/src/lsfg-vk-android`, and `sh tools/lsfg/build.sh windows`;
- and `make installer`, which makes `build/gen-venv` with `py -3.12` (python.org's Python 3.12) and downloads and checks
  the "embeddable" Python.

Testing the builder on its own: `tools/installer/test_builder.c`. Tests without a window:
`installer.exe --game G --dest D [--no-shortcuts] --auto --hidden` (exit code 0 = fine), `--copy-only`,
`--screenshot window.bmp`; `SMRPG_INSTALLER_SHORTCUTS=<folder>` leaves the shortcuts there.

## Playing

- Installed: the "SMRPG Recomp" shortcut. From the repository: `play.cmd` (with the controls explained inside the file).
- Menu: **Esc** or **F1** (also Home or Back + Start on a controller). Tabs: Controls, Video, Audio, Quit. It is in
  Spanish when the game's language is Spanish (Video > Game language) and in English otherwise.
- Menu > **Video**, the box on the right: at the top, what the game is using now (CPU, GPU, RAM, SSD, VRAM); above the
  buttons, the impact of the selected setting on each of them (green, yellow, red, with figures); at the bottom, Save,
  Undo, Reset and Restart (with a controller: Down from the last row).
- Keys and buttons: Menu > **Controls** says what each one does (on the keyboard: W A S D move, K talk / confirm, J jump /
  cancel, L items, I specials, Enter game menu); "Configure controls" changes them.

## Recommended settings (Video)

For a high refresh rate ultrawide monitor with HDR and a powerful GPU (tested: 3440x1440, RX 9070 XT):

| Setting | Value | Why |
| --- | --- | --- |
| Display mode | Borderless | Fills the monitor at its resolution without being exclusive full screen (instant Alt+Tab) |
| Widescreen | On (fill the screen) | On 21:9, more of the scene at the sides (nothing is stretched); it follows the window while running too |
| Widescreen movies | Fill the screen | Movies cover the 21:9 without stretching (they lose a little at the top and bottom); the title background stays in 16:9 |
| FPS limit | 240 (or your monitor's); with frame generation, half of it (120) | Outside battles the game goes by time; battles always run their logic at 60 steps per second, as on the console |
| True black | On | Removes the greenish veil the game's color grading leaves on black (loading screens, the sides of movies, shadows) |
| Internal resolution | Match display | Measured at 3440x1440: 3.9 ms per frame; 4K, 7.6; 8K, 13.7 (without FSR) |
| Anti-aliasing | AMD FSR | FSR 4 on Radeon RX 9000 (FSR 3.1 on the rest). It works at the internal resolution: on an RX 9070 XT, FSR 4 costs 1.4 ms per frame at 3440x1440 and 3.1 ms at 2160p in 21:9 (from 240 to ~175 FPS); SMAA, next to nothing |
| FSR quality | Native (anti-aliasing) | The other modes draw with fewer pixels and upscale: for modest GPUs |
| Frame generation | Optional | A generated frame between every two: with the limit at 120, you see 240. A bit more latency |
| Supersampling | Off (or x2/x4 if the GPU has room to spare) | Can be combined with FSR |
| Sharpness | 2-4 | Final sharpening (AMD RCAS) |
| Anisotropic filtering | 16x | |
| HDR | On, if the monitor has it | |

AMD FSR needs, next to `build/smrpg-recomp.exe`, the DLLs of AMD's FSR SDK 2.4 (`amd_fidelityfx_loader_vk.dll`,
`amd_fidelityfx_upscaler_vk.dll`; for frame generation, also `amd_fidelityfx_framegeneration_vk.dll`): see
`third_party/amd-fsr/README.txt`.

**Frame generation** (Video > Image, with Anti-aliasing set to "AMD FSR"): between every two frames of the game AMD
presents one generated with the game's motion vectors (FG 4, a neural network, on Radeon RX 9000; FG 3.1 on the rest).
Smoother, in exchange for some latency. Best with the FPS limit at half the monitor's refresh rate (120 on a 240 Hz one)
and at least 60 game frames. It pauses while the menu is open; the FPS counter says what you see ("120 fps (240 with
generation)").

**Lossless Scaling frame generation** (Video > "Lossless Scaling frame generation"): uses the `Lossless.dll` of **your**
copy of Lossless Scaling from Steam (a paid program; it is looked for in the registry and in your Steam libraries). Neither
the repository nor the installer includes or downloads it; without it, the option stays off.

## HD textures

For now, only from the repository (the installer does not include `hd-textures.cmd`). Dumped textures belong to the game:
don't share them.

1. Menu > Video > Image > **Dump textures**: On. Play for a while: each new texture is saved in `textures-dump/`.
2. Double-click **`hd-textures.cmd`**: it enlarges the dumped color textures with AI (Real-ESRGAN ncnn Vulkan,
   `realesrgan-ncnn-vulkan-*-windows.zip` from https://github.com/xinntao/Real-ESRGAN/releases, unzipped in the
   `realesrgan/` folder) and leaves them in `textures/` (x2; `hd-textures.cmd --scale 4` for x4, which takes more video
   memory).
3. **HD textures**: On (it is by default). The game uses the ones in `textures/` the next time it loads each texture.

You can also put them there by hand: any PNG in `textures/` whose name starts with the 16-digit hash of its dump, with the
same proportions and at least the same size.

## What is done

The game plays from start to finish in everything tested (movies, title screen, menus, exploring, battles, saving and
loading, areas from all over the map). The details of each version are in `CHANGELOG.txt`; the technical status, in
`STATUS.md`.
