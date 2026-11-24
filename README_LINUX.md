# SMRPG Recomp — Linux build

An unofficial static recompilation of *Super Mario RPG* (Switch, **v1.0.0 only**) to a native
Linux x86-64/aarch64 executable with Vulkan. It needs your own legally dumped and extracted copy
of the game (`exefs/` and `romfs/`); nothing from the game is included here.

## Requirements

- Linux with glibc 2.35+ (Ubuntu 22.04+, Debian 12+, Arch, Fedora, CachyOS...)
- A Vulkan 1.3 driver (Mesa RADV/ANV/NVK/Turnip, or NVIDIA's)
- An x86-64 CPU with AVX2 + FMA (2013 or newer)

Build dependencies (Ubuntu/Debian naming):

```bash
sudo apt install clang lld make pkg-config python3-venv libsdl3-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libvulkan-dev libshaderc-dev glslc vulkan-tools
```

## Quick start

```bash
./setup.sh
```

The script asks for the folder of your extracted game (the one with `exefs` and `romfs`), checks
that it is Super Mario RPG **1.0.0**, links it into `extracted/`, sets up the Python tools,
translates the binary to C, and builds everything (`build/lasrecomp` + `build/lasgame.so`).

Pass the folder directly (skips the prompt), or use `--copy` to copy instead of linking:

```bash
./setup.sh /path/to/your/Super\ Mario\ RPG --copy
```

## Manual build (if you prefer)

```bash
python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
ln -s /path/to/your/Super\ Mario\ RPG extracted   # or cp -r its contents into extracted/
make gen        # once: translate the game's binaries to C
make -j         # build build/lasrecomp and build/lasgame.so
```

## Running

From this folder (it finds `extracted/` right here):

```bash
./build/lasrecomp
```

Controls: **WASD** move, **K** talk/confirm, **J** jump/cancel, **L** items, **I** specials,
**Enter** game menu, **Esc/F1** in-game settings (Video, Controls...). Saves and settings are kept
in the folder (`save/`, `larecomp.ini`).
