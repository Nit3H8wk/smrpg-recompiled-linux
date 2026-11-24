# SMRPG Recomp

**English** · [Español](README.es.md)

An unofficial port of **Super Mario RPG** (Nintendo Switch, 2023, version 1.0.0) to **Windows** through static
recompilation: the game's program is translated to C, compiled for your PC's processor and runs on top of the
[LASRecomp](https://github.com/peiikai/LASRecomp) runtime (system services, kernel and NVN → Vulkan).

The **Android** port has its own repository and releases:
[**smrpg-recompiled-android**](https://github.com/ElKiro1311/smrpg-recompiled-android).

> **Legal notice.** This is a non-profit fan project, **not affiliated with or endorsed by Nintendo or Square Enix**.
> *Super Mario RPG* and *Mario* are trademarks of Nintendo; the game is © Nintendo / SQUARE ENIX. Neither this
> repository nor its releases contain any of the game's files, code, graphics, music or text. To play you need the files
> of **your own copy** of the game, already extracted (the `exefs` and `romfs` folders of version 1.0.0); there are no
> instructions here on how to obtain them. Don't share your installation or any textures the game dumps: they belong to
> the game.

## Status

It plays from start to finish in everything tested: movies, title screen, menus, exploring, battles, saving and loading,
and areas from all over the map. Ultrawide (21:9 and wider), internal resolutions up to 8K, HDR, up to 240 FPS,
AMD FSR 4 / 3.1 / 2.3 with frame generation, HD textures, and menus in English and Spanish.

The current version is a beta. What changed in each one: [`CHANGELOG.txt`](CHANGELOG.txt).

## Playing

Download `SMRPG-Recomp-Installer-<version>.exe` from the
[latest release](https://github.com/ElKiro1311/smrpg-recompiled/releases/latest). In its window you choose the folder
with your extracted copy of the game (the one with `exefs` and `romfs`) and where to install. The installer checks that
it is the complete 1.0.0 version, puts it into the installation (with hard links if it is on the same drive, so it takes
no extra space) and builds the game's code there: 10 to 20 minutes. It needs 64-bit Windows 10 or 11 and a graphics card
with Vulkan 1.3.

Guide, controls and recommended settings: [`GUIDE.md`](GUIDE.md).

Lossless Scaling frame generation (Menu > Video) uses the `Lossless.dll` of **your** copy of Lossless Scaling (a paid
program on Steam): neither the repository nor the installer includes or downloads it.

## Linux

> **Credits.** This Linux port is based on the original [smrpg-recompiled](https://github.com/ElKiro1311/smrpg-recompiled) project by ElKiro1311 and runs on top of the [LASRecomp](https://github.com/peiikai/LASRecomp) runtime (by Peiikai and contributors). All credit for the foundation goes to both.
>
> **No game data is included.** You must supply your own legally dumped `prod.keys`, `exefs` and `romfs` (version 1.0.0). Nothing in this repository contains Nintendo's code, graphics, music or text.

**Linux x86-64 build** (and aarch64): run [`setup_linux.sh`](setup_linux.sh). It asks for your own
`prod.keys` (auto-checks `~/.switch/prod.keys` first, or picks it with kdialog/zenity if available) and
the folder of your extracted game (`exefs/` + `romfs/`, v1.0.0), validates the NSO build ID, builds
everything, and puts the runnable game plus all its files into one portable folder:

```bash
./setup_linux.sh                     # interactive (GUI pickers if available)
./setup_linux.sh /path/to/game       # pass paths directly: [ROM_FOLDER] [PROD_KEYS]
# then run:
cd dist/SuperMarioRPG && ./smrpg
```

Needs clang, lld, make, pkg-config, glslc, Python 3 (venv), SDL3 ≥ 3.4, FFmpeg libs, a Vulkan 1.3 driver
and shaderc (`sudo apt install ...` in [`README_LINUX.md`](README_LINUX.md)). Your keys and dumps stay
outside the repository: `*.keys`, `prod.keys`, `romfs/`, `exefs/` and `main` are git-ignored.

### With [Recomplauncher](https://github.com/elripalda/RecompLauncher)

If you use [Recomplauncher](https://recomplauncher.com), this repository includes a ready-made
[`recomplauncher.json`](recomplauncher.json). The launcher asks for your game folder and `prod.keys`
(shared), verifies the NSO build ID itself, then runs [`build.sh`](build.sh) to translate the game's
code and compile it on your PC before launching `smrpg.AppImage`. Everything lives in the install
folder; nothing is ever written back into your dumps. To use a release: download the zip with
`recomplauncher.json`, `build.sh` and `smrpg.AppImage` (built by [`make_appimage.sh`](make_appimage.sh)),
and import it through the launcher.

## Building

On Windows, with MSYS2 UCRT64: [`GUIDE.md`](GUIDE.md) and [`docs/BUILD-windows.md`](docs/BUILD-windows.md). The extracted
game goes in `extracted/` (or a link to that folder); `make gen` translates its code to C and `make -j8` builds
`build/smrpg-recomp.exe` and `build/lasgame.dll`. The paths of tools outside the repository go in your own `local.env`,
which git ignores: see [`local.env.example`](local.env.example). Technical status and how it is tested:
[`STATUS.md`](STATUS.md).

Only Windows is supported here. The macOS and Linux parts, `make dist`/`app`/`dmg`/`appimage`, the Link's Awakening
randomizer (`tools/rando`, `third_party/LAS-Randomizer`) and `tools/shot.sh`, `frames.sh` and `run.sh` come from LASRecomp
and are not used. The port starts from LASRecomp 579fdad: compare with that commit to see what it adds.

## Bugs and suggestions

Open an [issue here](https://github.com/ElKiro1311/smrpg-recompiled/issues) and attach `lasrecomp.log` from the game's
folder. Please don't report them to LASRecomp: this port isn't theirs.

## Credits and licenses

- [LASRecomp](https://github.com/peiikai/LASRecomp), by Peiikai and contributors (MIT): the recompiler, the runtime and
  the renderer. Its original README is in [`docs/README-LASRecomp.md`](docs/README-LASRecomp.md).
- [nxnative](https://github.com/seedds/nxnative), a reference for how this game behaves.
- AMD FidelityFX SDK (FSR and frame generation; its DLLs go apart: [`third_party/amd-fsr`](third_party/amd-fsr)), Vulkan
  Memory Allocator, Dear ImGui, SMAA, [lsfg-vk-android](https://github.com/GameNative/lsfg-vk-android) (Lossless Scaling
  frame generation) and what the installer distributes (SDL3, FFmpeg LGPL, Opus, shaderc, LLVM/clang/lld, Python,
  Capstone, Unicorn…): each with its own license, in `third_party/` and in the installation's `licenses\` folder.
- The code of LASRecomp and of this port is under the MIT license in [`LICENSE`](LICENSE). That license does not cover
  Super Mario RPG or anything else of Nintendo's, nor what is in `third_party/`, which keeps its own license.
