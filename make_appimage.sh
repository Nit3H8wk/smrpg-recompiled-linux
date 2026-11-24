#!/usr/bin/env bash
# ============================================================================
# make_appimage.sh — package the host binary + fonts into smrpg.AppImage
#
# Requires a built `build/lasrecomp` (see setup_linux.sh or README_LINUX.md)
# and appimagetool: https://github.com/AppImage/appimagetool/releases
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")"
[[ -x build/lasrecomp ]] || { echo "Run the build first (setup_linux.sh)."; exit 1; }

command -v appimagetool >/dev/null 2>&1 \
  || { echo "Install appimagetool first."; exit 1; }

rm -rf AppDir smrpg.AppImage
mkdir -p AppDir
cp build/lasrecomp AppDir/smrpg
cp setup.sh AppDir/setup.sh
for f in third_party/fonts/*/*.ttf; do cp "$f" AppDir/; done

cat > AppDir/AppRun <<'EOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
# Writable next to us (unpacked AppImage / portable folder); XDG fallback if read-only.
if [[ -w "$DIR" ]]; then DATA="$DIR/data"; else DATA="${XDG_DATA_HOME:-$HOME/.local/share}/lasrecomp"; fi
mkdir -p "$DATA"
# One-time bootstrap: asks for dumps, validates, builds lasgame.so into $DATA.
if [[ ! -f "$DATA/lasgame.so.id" ]]; then
  "$DIR/setup.sh" --data="$DATA" || exit $?
fi
LARECOMP_DATA_HOME="$DATA" LARECOMP_GAME_LIB="$DATA/lasgame.so" exec "$DIR/smrpg" "$@"
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

appimagetool AppDir smrpg.AppImage
rm -rf AppDir
echo "Done: $(pwd)/smrpg.AppImage"
