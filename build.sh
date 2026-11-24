#!/usr/bin/env bash
# ============================================================================
# build.sh — RecompLauncher "prepare" step for SMRPG Recomp (Linux)
#
# Runs headless after the launcher has validated your game folder. It:
#   1. clones the source tree (no game code is shipped in the release),
#   2. links your exefs/ and romfs/ into a build workspace,
#   3. translates the NSO to C and compiles lasgame.so for this CPU,
#   4. leaves {install}/data/lasgame.so.id as the "done" marker.
#
# With keeps_copy=true the launcher may re-run us without --game once we're
# done: in that case we verify the marker and exit 0 immediately.
# ============================================================================
set -euo pipefail

GAME=""
DATA=""
SRC_OVERRIDE=""

for arg in "$@"; do
  case "$arg" in
    --game=*) GAME="${arg#--game=}";;
    --data=*) DATA="${arg#--data=}";;
    --src=*)  SRC_OVERRIDE="${arg#--src=}";;
  esac
done

INSTALL="$(cd "$(dirname "$0")" && pwd)"
DATA="${DATA:-$INSTALL/data}"
mkdir -p "$DATA"

MARKER="$DATA/lasgame.so.id"

# Already built for this game version? (keeps_copy re-run without the copy)
if [[ -f "$MARKER" ]]; then
  echo "build.sh: lasgame.so already built ($MARKER) — nothing to do."
  exit 0
fi

[[ -n "$GAME" ]] || { echo "build.sh: no --game given and nothing is built yet." >&2; exit 1; }
[[ -f "$GAME/exefs/main" ]] || { echo "build.sh: $GAME/exefs/main not found." >&2; exit 1; }
[[ -d "$GAME/romfs" ]]      || { echo "build.sh: $GAME/romfs not found." >&2; exit 1; }

# --- source tree ------------------------------------------------------------
SRC="$INSTALL/src"
if [[ -n "$SRC_OVERRIDE" ]]; then SRC="$SRC_OVERRIDE"; fi
if [[ ! -f "$SRC/Makefile" ]]; then
  echo "build.sh: getting the source tree..."
  rm -rf "$SRC"
  git clone --depth 1 https://github.com/Nit3H8wk/smrpg-recompiled-linux.git "$SRC" \
    || { echo "build.sh: couldn't clone the source; check your connection." >&2; exit 1; }
fi

# --- workspace: link your game files (never copied, never modified) ---------
cd "$SRC"
rm -rf extracted
mkdir -p extracted
ln -sfn "$GAME/exefs" extracted/exefs
ln -sfn "$GAME/romfs" extracted/romfs

# --- tools -------------------------------------------------------------------
if ! command -v clang >/dev/null 2>&1 || ! command -v glslc >/dev/null 2>&1; then
  echo "build.sh: missing toolchain (clang, glslc...). See README_LINUX.md." >&2
  exit 1
fi
python3 -m venv tools/venv && tools/venv/bin/pip install --quiet lz4 capstone unicorn

# --- translate and compile ---------------------------------------------------
echo "build.sh: translating the game binary to C..."
make gen
echo "build.sh: compiling for this CPU ($(nproc) jobs)..."
make -j"$(nproc)"

cp build/lasgame.so "$DATA/"
cp build/lasgame.so.id "$MARKER"

# Runtime expects its game files under <data>/extracted: link, don't copy.
mkdir -p "$DATA/extracted"
ln -sfn "$GAME/exefs" "$DATA/extracted/exefs"
ln -sfn "$GAME/romfs" "$DATA/extracted/romfs"

echo "build.sh: done. lasgame.so is ready in $DATA"
