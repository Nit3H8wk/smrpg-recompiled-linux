#!/usr/bin/env bash
# ============================================================================
# setup.sh — one-time bootstrap inside smrpg.AppImage
#
# Runs on first launch only. Asks for your legally dumped game folder and
# prod.keys (GUI picker if available), validates the NSO build ID, clones the
# source, translates and compiles lasgame.so into a writable data folder.
# Once $DATA/lasgame.so.id exists, AppRun skips this entirely.
# ============================================================================
set -euo pipefail

DATA=""
ROM_ARG=""
KEYS_ARG=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --data=*) DATA="${1#--data=}";;
    *) if [[ -z "$ROM_ARG" ]]; then ROM_ARG="$1"; else KEYS_ARG="$1"; fi;;
  esac
  shift
done
[[ -n "$DATA" ]] || { echo "setup.sh: no --data given." >&2; exit 1; }
mkdir -p "$DATA"

EXPECTED_ID="e968832cade2ad7cbd85b6c6dfe1d9c1"  # Super Mario RPG v1.0.0
DEFAULT_KEYS="$HOME/.switch/prod.keys"
INSTALL="$(cd "$(dirname "$0")" && pwd)"

# Already configured?
if [[ -f "$DATA/lasgame.so.id" ]]; then
  exit 0
fi

echo "== SMRPG Recomp — first-time setup =="

for t in clang lld make pkg-config glslc python3; do
  command -v "$t" >/dev/null || { echo "error: missing $t. Install it from your distro and relaunch." >&2; exit 1; }
done

pick_file() {   # $1 = label, $2 = hint path — tries every common native browser
  local out="" tool found=0
  for tool in kdialog zenity-qt zenity; do
    if command -v "$tool" >/dev/null 2>&1; then
      found=1
      case "$tool" in
        kdialog)     out=$($tool --title "SMRPG Recomp" --file-open "$2" 2>/dev/null) || true ;;
        zenity-qt|zenity) out=$($tool --file-selection --filename="$2/" 2>/dev/null) || true ;;
      esac
      break
    fi
  done
  if [[ $found -eq 0 ]]; then
    read -r -p "No GUI file browser found — enter path to $1 [$2]: " out
    [[ -n "$out" ]] || out="$2"
  fi
  printf '%s\n' "$out" | tr -d '"'
}

pick_dir() {    # $1 = label, $2 = hint path — tries every common native browser
  local out="" tool found=0
  for tool in kdialog zenity-qt zenity; do
    if command -v "$tool" >/dev/null 2>&1; then
      found=1
      case "$tool" in
        kdialog)     out=$($tool --title "SMRPG Recomp" --select-folder "$2" 2>/dev/null) || true ;;
        zenity-qt|zenity) out=$($tool --file-selection --directory=true --filename="$2/" 2>/dev/null) || true ;;
      esac
      break
    fi
  done
  if [[ $found -eq 0 ]]; then
    read -r -p "No GUI file browser found — enter path to $1 [$2]: " out
    [[ -n "$out" ]] || out="$2"
  fi
  printf '%s\n' "$out" | tr -d '"'
}

# --- 1. prod.keys (validated for your confidence; never copied into the app) --
echo "-- Step 1: your Nintendo 'prod.keys'"
if [[ -n "$KEYS_ARG" ]]; then KEYS="$KEYS_ARG"; elif [[ -f "$DEFAULT_KEYS" ]]; then
  KEYS="$DEFAULT_KEYS"
else
  KEYS=$(pick_file "your prod.keys file" "$HOME")
fi
[[ -s "${KEYS:-}" ]] || { echo "error: no valid prod.keys found." >&2; exit 1; }
echo "-- prod.keys OK: $KEYS (stays where it is)"

# --- 2. the extracted game files ---------------------------------------------
echo "-- Step 2: your extracted game (folder with exefs/ and romfs/, v1.0.0)"
if [[ -n "$ROM_ARG" ]]; then ROM="$ROM_ARG"; else
  ROM=$(pick_dir "the Super Mario RPG folder with exefs/ and romfs/" "$HOME")
fi
ROM="${ROM%/}"
[[ -f "$ROM/exefs/main" ]] || { echo "error: $ROM/exefs/main not found — pick the folder that CONTAINS exefs/." >&2; exit 1; }
[[ -d "$ROM/romfs" ]]      || { echo "error: $ROM/romfs not found." >&2; exit 1; }

# --- 3. validate NSO build ID --------------------------------------------------
BUILD_ID=$(python3 - "$ROM/exefs/main" <<'EOF'
import sys
with open(sys.argv[1], "rb") as f:
    if f.read(4) != b"NSO0":
        sys.exit("not an NSO file")
    f.seek(0x40)
    print(f.read(16).hex())
EOF
) || { echo "error: $ROM/exefs/main is not a valid NSO binary." >&2; exit 1; }
echo "-- main build ID: $BUILD_ID"
[[ "$BUILD_ID" == "$EXPECTED_ID" ]] || {
  echo "error: not Super Mario RPG v1.0.0 (expected $EXPECTED_ID)." >&2
  exit 1
}
echo "-- game files validated: Super Mario RPG 1.0.0 ✓"

# --- 4. source tree -------------------------------------------------------------
SRC="$DATA/src"
if [[ ! -f "$SRC/Makefile" ]]; then
  echo "-- getting the source tree..."
  rm -rf "$SRC"
  git clone --depth 1 https://github.com/Nit3H8wk/smrpg-recompiled-linux.git "$SRC" \
    || { echo "error: couldn't clone the source; check your connection." >&2; exit 1; }
fi

# --- 5. link game files (never copied, never modified) --------------------------
cd "$SRC"
rm -rf extracted
mkdir -p extracted
ln -sfn "$ROM/exefs" extracted/exefs
ln -sfn "$ROM/romfs" extracted/romfs

echo "-- installing build tools..."
python3 -m venv tools/venv && tools/venv/bin/pip install --quiet lz4 capstone unicorn

# --- 6. translate and compile ----------------------------------------------------
echo "-- translating the game binary to C (10-20 minutes)..."
make gen
echo "-- compiling for this CPU ($(nproc) jobs)..."
make -j"$(nproc)"

cp build/lasgame.so "$DATA/"
cp build/lasgame.so.id "$DATA/"

# --- 7. point the runtime at your game files -------------------------------------
mkdir -p "$DATA/extracted"
ln -sfn "$ROM/exefs" "$DATA/extracted/exefs"
ln -sfn "$ROM/romfs" "$DATA/extracted/romfs"

echo
echo "== Setup complete. Launching the game..."
exit 0
