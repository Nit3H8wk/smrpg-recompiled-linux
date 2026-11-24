#!/usr/bin/env bash
# ============================================================================
# setup.sh — one-time bootstrap inside smrpg-linux.AppImage
#
# Runs on first launch only. Guides the user through selecting their own
# prod.keys and extracted game folder (v1.0.0) using native file browsers,
# copies the assets into the local data folder, and compiles lasgame.so
# offline from the bundled source. Once $DATA/lasgame.so.id exists, AppRun
# skips this entirely. No Nintendo files are ever bundled in the AppImage.
# ============================================================================
set -euo pipefail

DATA="" ROM_ARG="" KEYS_ARG=""
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
[[ -f "$DATA/lasgame.so.id" ]] && exit 0

echo "== SMRPG Recomp — first-time setup =="

die() { echo "error: $1" >&2; exit 1; }

# Status indicator: write to file + desktop notification
STATUS_FILE="$DATA/build_status.txt"
status() {
  local msg="$1"
  echo "$msg" | tee -a "$STATUS_FILE"
  # Desktop notification (if available)
  if command -v notify-send >/dev/null 2>&1; then
    notify-send "SMRPG Recomp" "$msg" || true
  fi
}

# Native instruction dialog (falls back to plain text)
info() {
  local msg="$1" title="$2"
  echo ""
  echo "--- $title ---"
  echo "$msg"
  if command -v kdialog >/dev/null 2>&1; then
    kdialog --title "SMRPG Recomp — Setup" --msgbox "$msg" >/dev/null 2>&1 || true
  elif command -v zenity-qt >/dev/null 2>&1; then
    zenity-qt --info --title="SMRPG Recomp — Setup" --text="$msg" >/dev/null 2>&1 || true
  elif command -v zenity >/dev/null 2>&1; then
    zenity --info --title="SMRPG Recomp — Setup" --text="$msg" >/dev/null 2>&1 || true
  fi
}

# Try every common native browser; fall back to terminal prompt.
pick_file() {  # $1 = hint path
  local out="" tool found=0
  for tool in kdialog zenity-qt zenity; do
    command -v "$tool" >/dev/null 2>&1 || continue
    found=1
    case "$tool" in
      kdialog)          out=$($tool --title "SMRPG Recomp — Setup" --file-open "$1" 2>/dev/null) || true ;;
      zenity-qt|zenity)  out=$($tool --file-selection --filename="$1/" 2>/dev/null) || true ;;
    esac
    break
  done
  [[ $found -eq 1 ]] || read -r -p "Terminal mode — enter path to prod.keys [$1]: " out
  printf '%s\n' "${out:-$1}" | tr -d '"'
}

pick_dir() {     # $1 = hint path
  local out="" tool found=0
  for tool in kdialog zenity-qt zenity; do
    command -v "$tool" >/dev/null 2>&1 || continue
    found=1
    case "$tool" in
      kdialog)          out=$($tool --title "SMRPG Recomp — Setup" --select-folder "$1" 2>/dev/null) || true ;;
      zenity-qt|zenity)  out=$($tool --file-selection --directory=true --filename="$1/" 2>/dev/null) || true ;;
    esac
    break
  done
  [[ $found -eq 1 ]] || read -r -p "Terminal mode — enter path to the game folder [$1]: " out
  printf '%s\n' "${out:-$1}" | tr -d '"'
}

# --- Step 1: prod.keys ------------------------------------------------------
# Check if RecompLauncher (or user) already provided it
if [[ -n "$KEYS_ARG" ]]; then KEYS="$KEYS_ARG"
elif [[ -f "$DATA/prod.keys" ]]; then KEYS="$DATA/prod.keys"
elif [[ -f "${LARECOMP_KEYS:-}" ]]; then KEYS="$LARECOMP_KEYS"
elif [[ -f "$DEFAULT_KEYS" ]]; then KEYS="$DEFAULT_KEYS"
else
  info "Select your Nintendo 'prod.keys' file. This is the key file exported from your Switch console (e.g. via Tinfoil, Riivolution, or a sysdump tool). It lets the game decrypt its own files." "Step 1 of 2: prod.keys"
  KEYS=$(pick_file "$HOME")
fi
[[ -s "${KEYS:-}" ]] || die "no valid prod.keys found at: $KEYS"
echo "-- prod.keys OK: $KEYS (stays where it is — never copied into the app)"

# --- Step 2: extracted game folder ------------------------------------------
# Check if RecompLauncher already put assets in data/extracted/
if [[ -f "$DATA/extracted/exefs/main" && -d "$DATA/extracted/romfs" ]]; then
  ROM="$DATA/extracted"
  echo "-- assets already in $ROM — skipping picker"
elif [[ -n "$ROM_ARG" ]]; then ROM="$ROM_ARG"
else
  info "Select the Super Mario RPG folder. This must be the folder that CONTAINS both 'exefs/' and 'romfs/' subfolders (version 1.0.0). If you dumped from an .nsp/.xci, extract it first — then pick the parent folder." "Step 2 of 2: Game Files"
  ROM=$(pick_dir "$HOME")
fi
ROM="${ROM%/}"
[[ -f "$ROM/exefs/main" ]] || die "$ROM/exefs/main not found — pick the folder that CONTAINS exefs/"
[[ -d "$ROM/romfs" ]]      || die "$ROM/romfs not found — pick the folder that CONTAINS romfs/"

# --- Step 3: validate NSO build ID -------------------------------------------
BUILD_ID=$(python3 - "$ROM/exefs/main" <<'EOF' 2>/dev/null
import sys
with open(sys.argv[1], "rb") as f:
    if f.read(4) != b"NSO0":
        raise SystemExit("bad header")
    f.seek(0x40)
    print(f.read(16).hex())
EOF
) || die "$ROM/exefs/main is not a valid NSO binary"
echo "-- main build ID: $BUILD_ID"
[[ "$BUILD_ID" == "$EXPECTED_ID" ]] || { echo "expected $EXPECTED_ID (v1.0.0)" >&2; die "wrong game version"; }
echo "-- validated: Super Mario RPG v1.0.0 ✓"

# --- Step 4: copy assets into the data folder ---------------------------------
EXTRACTED="$DATA/extracted"
if [[ ! -d "$EXTRACTED/exefs" ]]; then
  echo "-- copying exefs/ and romfs/ into $EXTRACTED ..."
  mkdir -p "$EXTRACTED"
  cp -r "$ROM/exefs" "$EXTRACTED/exefs" || die "failed to copy exefs"
  cp -r "$ROM/romfs" "$EXTRACTED/romfs" || die "failed to copy romfs"
else
  echo "-- assets already in $EXTRACTED — skipping copy"
fi

# --- Step 5: offline build from bundled source ---------------------------------
SRC="$INSTALL/src_bundle"
[[ -f "$SRC/Makefile" ]] || die "bundled source missing ($SRC) — AppImage is corrupt"

BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/smrpg_build.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT
echo "-- preparing build directory: $BUILD_DIR"
cp -r "$SRC/." "$BUILD_DIR/" || die "failed to extract source bundle"
ln -sfn "$EXTRACTED" "$BUILD_DIR/extracted"

for t in clang lld make pkg-config glslc python3; do
  command -v "$t" >/dev/null 2>&1 || die "missing build tool: $t — install it from your distro and relaunch"
done

echo "-- installing Python build deps (lz4, capstone, unicorn)..."
status "Installing Python build dependencies..."
cd "$BUILD_DIR"
python3 -m venv tools/venv && tools/venv/bin/pip install --quiet lz4 capstone unicorn \
  || die "Python dependency install failed — check your internet connection"

echo "-- translating game binary to C (this takes 10-20 minutes)..."
status "Translating game binary to C... 0%"
# Run make gen, capture output, and parse progress
GEN_LOG=$(mktemp)
trap 'rm -f "$GEN_LOG"' EXIT
if make gen > "$GEN_LOG" 2>&1; then
  while read -r line; do
    echo "$line"
    if [[ "$line" =~ progress[[:space:]]([0-9]+)/([0-9]+) ]]; then
      current="${BASH_REMATCH[1]}"
      total="${BASH_REMATCH[2]}"
      pct=$((current * 100 / total))
      status "Translating game binary to C... ${pct}%"
    fi
  done < "$GEN_LOG"
else
  cat "$GEN_LOG" >&2
  die "'make gen' failed — see messages above"
fi
echo "-- compiling for this CPU ($(nproc) jobs)..."
status "Compiling for this CPU ($(nproc) jobs)..."
make -j"$(nproc)" || die "'make' failed — see messages above"

cp build/lasgame.so "$DATA/" || die "failed to copy lasgame.so"
cp build/lasgame.so.id "$DATA/" || die "failed to copy lasgame.so.id"

status "Build complete! Launching the game..."
echo ""
echo "== Setup complete! Launching the game..."
exit 0
