#!/usr/bin/env bash
# ============================================================================
# make_appimage.sh — package the host binary + fonts + build sources into
# smrpg-linux.AppImage. Everything needed to run and first-time configure the
# game is bundled inside; only the user's own dumps/keys come from outside.
#
# Requires a built `build/lasrecomp` (see setup_linux.sh or README_LINUX.md)
# and appimagetool: https://github.com/AppImage/appimage.github.io
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")"
[[ -x build/lasrecomp ]] || { echo "Run the build first (setup_linux.sh)."; exit 1; }
command -v appimagetool >/dev/null 2>&1 || { echo "Install appimagetool first."; exit 1; }

rm -rf AppDir smrpg-linux.AppImage
mkdir -p AppDir
cp build/lasrecomp AppDir/smrpg
cp setup.sh AppDir/setup.sh
for f in third_party/fonts/*/*.ttf; do cp "$f" AppDir/; done

# Bundled offline build sources (runtime, tools, imgui, smaa, Makefile). No game data.
mkdir -p AppDir/src_bundle/third_party
cp -r runtime AppDir/src_bundle/runtime
cp -r tools AppDir/src_bundle/tools
rm -rf AppDir/src_bundle/tools/venv AppDir/src_bundle/tools/__pycache__
cp -r third_party/imgui AppDir/src_bundle/third_party/imgui
cp -r third_party/smaa AppDir/src_bundle/third_party/smaa
cp -r third_party/amd-fsr AppDir/src_bundle/third_party/amd-fsr
cp -r third_party/vma AppDir/src_bundle/third_party/vma
cp -r third_party/ryujinx AppDir/src_bundle/third_party/ryujinx
cp Makefile AppDir/src_bundle/Makefile

cat > AppDir/AppRun <<'EOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
# Writable next to us (unpacked AppImage / portable folder); XDG fallback if read-only.
if [[ -w "$DIR" ]]; then DATA="$DIR/data"; else DATA="${XDG_DATA_HOME:-$HOME/.local/share}/lasrecomp"; fi
mkdir -p "$DATA"

# One-time bootstrap: guided picker for dumps/keys, copies assets, builds lasgame.so.
if [[ ! -f "$DATA/lasgame.so.id" ]]; then
  if ! "$DIR/setup.sh" --data="$DATA"; then
    MSG="SMRPG Recomp setup failed — see the terminal output above for the exact reason."
    echo "$MSG" >&2
    if command -v kdialog >/dev/null 2>&1; then
      kdialog --title "SMRPG Recomp — Setup Failed" --error "$MSG" >/dev/null 2>&1 || true
    elif command -v zenity-qt >/dev/null 2>&1; then
      zenity-qt --error --title="SMRPG Recomp — Setup Failed" --text="$MSG" >/dev/null 2>&1 || true
    fi
    exit 1
  fi
fi

LARECOMP_DATA_ROOT="$DATA" LARECOMP_GAME_LIB="$DATA/lasgame.so" exec "$DIR/smrpg" "$@"
EOF
chmod +x AppDir/AppRun AppDir/setup.sh

cat > AppDir/smrpg.desktop <<'EOF'
[Desktop Entry]
Name=SMRPG Recomp
Comment=Super Mario RPG (Switch) — Linux recompiled
Exec=AppRun
Type=Application
Categories=Game;
EOF

appimagetool AppDir smrpg-linux.AppImage >/dev/null 2>&1 || appimagetool AppDir smrpg-linux.AppImage
rm -rf AppDir
echo "Done: $(pwd)/smrpg-linux.AppImage"
