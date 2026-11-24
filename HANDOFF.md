# SMRPG Recomp Linux — AppImage Bootstrap Handoff

## Goal
Self-contained `smrpg-linux.AppImage`: launch → guided one-time setup (user picks their own dumps/keys via native file browser) → assets copied into local data folder → offline compile of `lasgame.so` → game runs. Integrated with RecompLauncher. Nothing copyrighted is bundled; only code.

## Current State
- **Repo**: `/home/nit3h8wk/source/smrpg-github/` → GitHub `Nit3H8wk/smrpg-recompiled-linux`, master @ `83a62d5`
- **Release live**: `v0.53.0` (https://github.com/Nit3H8wk/smrpg-recompiled-linux/releases/tag/v0.53.0) — assets: `build.sh`, `recomplauncher.json`, `smrpg-linux.AppImage`. **BUT this AppImage is broken** (see Bugs). Must be replaced with a rebuild once fixed.
- **`src_bundle/` PREPARED**: `/home/nit3h8wk/source/smrpg-github/src_bundle/` (~15MB, untracked): `runtime/`, `tools/` (venv removed), `third_party/imgui/`, `Makefile`. This is the offline build source — must be copied into AppDir by `make_appimage.sh`.
- **Launcher cache cleared** on this machine: `~/.local/share/com.recomplauncher.app/cache/releases/Nit3H8wk__smrpg-recompiled-linux.json*` (was serving stale asset digests → checksum errors).

## Key Paths
| Thing | Path |
|---|---|
| Repo | `/home/nit3h8wk/source/smrpg-github/` |
| Built binary | `build/lasrecomp` (renamed to `smrpg` in AppDir) |
| Source bundle | `src_bundle/` (runtime/, tools/, third_party/imgui/, Makefile) |
| Test workspace | `/tmp/bt_test/` (smrpg bin, fonts, old AppRun — disposable) |
| ROM root (test) | `/mnt/roms/roms/switch/Super Mario RPG` (has `exefs/main`, `romfs/`) |
| prod.keys (test) | `/mnt/roms/roms/windows/KartRecomp/userdata/data/kartrecomp/keys/prod.keys` |
| Expected NSO build ID | `e968832cade2ad7cbd85b6c6dfe1d9c1` (SMRPG v1.0.0, at offset `0x40` of `exefs/main`) |

## Bugs in Current v0.53.0 AppImage (all to fix)
1. **No window after setup**: `setup.sh` does `git clone https://github.com/Nit3H8wk/smrpg-recompiled-linux.git` at runtime — needs network/toolchain; if build fails, `AppRun` does `|| exit $?` → process dies silently with no GUI error.
2. **Asks every time**: because #1 never completes, `$DATA/lasgame.so.id` is never written → bootstrap re-runs on every launch.
3. **Wrong env pin**: `AppRun` sets `LARECOMP_DATA_HOME`, but `runtime/paths.c` `rt_paths_init()` uses `LARECOMP_DATA_ROOT` for a direct data-root override (DATA_HOME only sets the "platform home" base). Set **both** or at least `LARECOMP_DATA_ROOT`.
4. **Symlinks instead of copies**: user wants `exefs/` + `romfs/` **copied into the game folder** (`$DATA/extracted/`), not symlinked to their ROM dir.

## To Do — `setup.sh` (rewrite)
Requirements:
- **Guided UI**: before each picker, show an instruction dialog (kdialog `--msgbox` / zenity `--info`, fallback echo) telling the user exactly what to select:
  - prod.keys: "Select your Nintendo prod.keys file"
  - ROM folder: "Select the Super Mario RPG folder that CONTAINS exefs/ and romfs/ subfolders (v1.0.0)"
- **Pickers**: cascade `kdialog` → `zenity-qt` → `zenity` → terminal `read`. Single folder pick for ROM root; validate `$ROM/exefs/main` + `$ROM/romfs` exist.
- **Validation**: NSO header check (`NSO0`) + build ID at offset 0x40 == `e968832cade2ad7cbd85b6c6dfe1d9c1`.
- **Copy assets** (no symlinks in final data): `cp -r "$ROM/exefs" "$DATA/extracted/exefs"` and same for romfs.
- **Offline build**: extract `$INSTALL/src_bundle/*` into a temp build dir (`mktemp -d`); symlink/copy `extracted/` there; `python3 -m venv tools/venv && pip install lz4 capstone unicorn`; `make gen && make -j$(nproc)`; copy `build/lasgame.so` + `lasgame.so.id` → `$DATA/`. No git, no network except pip.
- **Success marker**: write `$DATA/lasgame.so.id` only after build succeeds.
- **Errors**: always print human-readable reason to stderr before exiting non-zero.

## To Do — `make_appimage.sh` (update)
- Copy `src_bundle/` → `AppDir/src_bundle/`.
- New `AppRun`:
  - `DATA=$DIR/data` if writable else XDG fallback (`~/.local/share/lasrecomp`)
  - If no `$DATA/lasgame.so.id`: run `setup.sh --data="$DATA"`; on failure show GUI error (kdialog/zenity) + stderr, then exit — never silent.
  - Launch: `LARECOMP_DATA_ROOT="$DATA" LARECOMP_GAME_LIB="$DATA/lasgame.so" exec "$DIR/smrpg" "$@"`

## To Do — Release/Push Sequence
1. Write updated `setup.sh`, `make_appimage.sh`.
2. `./make_appimage.sh` → new `smrpg-linux.AppImage`.
3. Quick smoke test: run AppImage headless with positional args (ROM + keys paths) pointing at `/mnt/roms/...`; confirm `data/lasgame.so.id` appears.
4. `gh release upload v0.53.0 smrpg-linux.AppImage --clobber` (or bump to v0.54.0).
5. Commit + push; verify no keys/dumps tracked (`git status`).

## Constraints (do not violate)
- `.gitignore` must keep `*.keys`, `prod.keys`, `romfs/`, `exefs/`, `main` ignored.
- Never commit Nintendo files or key data.
- Credits: original `ElKiro1311/smrpg-recompiled` + LASRecomp (`peiikai/LASRecomp`). State no game data is included.
- Binary name inside AppImage: `smrpg`.
