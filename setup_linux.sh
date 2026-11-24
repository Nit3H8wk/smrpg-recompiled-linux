#!/usr/bin/env bash
# ============================================================================
# SMRPG Recomp (Linux) — setup_linux.sh
#
# Guides you through supplying your own legally dumped Switch files:
#   1. prod.keys        (auto-checks ~/.switch/prod.keys, or pick it with a
#                        GUI picker / terminal prompt)
#   2. extracted game   (the folder with exefs/ and romfs/, v1.0.0 only)
# Then it validates both, builds the game from source, and assembles one
# portable folder you can run as-is:
#
#     dist/SuperMarioRPG/smrpg
#
# Your keys and game dumps stay outside the repository forever (see .gitignore).
# No game data is bundled — bring your own legal copies only.
# Based on smrpg-recompiled (ElKiro1311) running on LASRecomp (Peiikai).
#
# Usage:
#   ./setup_linux.sh                          # interactive (GUI or terminal)
#   ./setup_linux.sh [ROM_FOLDER] [PROd_KEYS] # skip the pickers
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

EXPECTED_ID="e968832cade2ad7cbd85b6c6dfe1d9c1"  # Super Mario RPG v1.0.0
DEFAULT_KEYS="$HOME/.switch/prod.keys"
DIST_DIR="dist/SuperMarioRPG"

# ---------------------------------------------------------------------------
pick_file() {   # $1 = label, $2 = hint path
  local out=""
  if command -v kdialog >/dev/null 2>&1; then
    out=$(kdialog --title "SMRPG Recomp" --file-open "$2" 2>/dev/null) || true
  elif command -v zenity >/dev/null 2>&1; then
    out=$(zenity --file-selection --filename="$2/" 2>/dev/null) || true
  else
    read -r -p "Terminal mode: enter path to $1 [$2]: " out
    [[ -n "$out" ]] || out="$2"
  fi
  printf '%s\n' "$out" | tr -d '"'
}

pick_dir() {    # $1 = label, $2 = hint path
  local out=""
  if command -v kdialog >/dev/null 2>&1; then
    out=$(kdialog --title "SMRPG Recomp" --select-folder "$2" 2>/dev/null) || true
  elif command -v zenity >/dev/null 2>&1; then
    out=$(zenity --file-selection --directory=true --filename="$2/" 2>/dev/null) || true
  else
    read -r -p "Terminal mode: enter path to $1 [$2]: " out
    [[ -n "$out" ]] || out="$2"
  fi
  printf '%s\n' "$out" | tr -d '"'
}

# ---------------------------------------------------------------------------
echo "== SMRPG Recomp — Linux setup =="

for t in clang lld make pkg-config glslc python3; do
  command -v "$t" >/dev/null || { echo "error: missing $t. Install your distro's packages first (see README.md)." >&2; exit 1; }
done

# --- 1. prod.keys -----------------------------------------------------------
if [[ $# -ge 2 ]]; then KEYS="$2"; elif [[ -f "$DEFAULT_KEYS" ]]; then KEYS="$DEFAULT_KEYS"; else
  echo "-- Step 1: your Nintendo 'prod.keys'"
  KEYS=$(pick_file "your prod.keys file" "$HOME")
fi
[[ -s "$KEYS" ]] || { echo "error: no valid prod.keys at $KEYS (file missing or empty)." >&2; exit 1; }
echo "-- prod.keys found: $KEYS (kept outside the repo, never copied in)"

# --- 2. the extracted game files ---------------------------------------------
if [[ $# -ge 1 ]]; then ROM="$1"; else
  echo "-- Step 2: your extracted game (folder with exefs/ and romfs/)"
  ROM=$(pick_dir "the Super Mario RPG folder with exefs/ and romfs/" "$HOME")
fi
ROM="${ROM%/}"
[[ -f "$ROM/exefs/main" ]] || { echo "error: $ROM/exefs/main not found — pick the folder that CONTAINS exefs/." >&2; exit 1; }
[[ -d "$ROM/romfs" ]]      || { echo "error: $ROM/romfs not found." >&2; exit 1; }

# --- 3. validate: NSO build ID must be v1.0.0 ---------------------------------
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
  echo "       Only v1.0.0 is supported." >&2
  exit 1
}
echo "-- game files validated: Super Mario RPG 1.0.0 ✓"

# --- 4. put everything in one portable folder ---------------------------------
rm -rf "$DIST_DIR" extracted
mkdir -p "$DIST_DIR/extracted"
if [[ "$(stat -c %d "$ROM")" == "$(stat -c %d .)" ]]; then
  cp -a -l "$ROM/exefs" "$ROM/romfs" "$DIST_DIR/extracted/"   # hard links: instant, zero extra space
else
  echo "-- copying game files (different disks)..."
  cp -r --reflink=auto "$ROM/exefs" "$ROM/romfs" "$DIST_DIR/extracted/"
fi
ln -s "$PWD/$DIST_DIR/extracted" extracted                     # dev-mode link for the build

# --- 5. generate and build ----------------------------------------------------
echo "-- setting up Python tools..."
python3 -m venv tools/venv && tools/venv/bin/pip install --quiet lz4 capstone unicorn
echo "-- translating the game binary to C..."
make gen
echo "-- building (this takes a while)..."
make -j"$(nproc)"

# --- 6. finish the portable folder --------------------------------------------
cp build/lasrecomp "$DIST_DIR/smrpg"
cp build/lasgame.so build/lasgame.so.id "$DIST_DIR/"
for f in third_party/fonts/*/*.ttf; do cp "$f" "$DIST_DIR/"; done

echo
echo "== Done. Run it:"
echo "   cd $PWD/$DIST_DIR && ./smrpg"
echo
echo "That folder is fully portable: move or zip it anywhere (needs a Vulkan 1.3 driver)."
