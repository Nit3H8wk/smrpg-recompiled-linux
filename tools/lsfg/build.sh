#!/bin/bash
# tools/lsfg/build.sh <windows|android-arm64-v8a|android-x86_64>: the port's Lossless Scaling Frame Generation library
# (tools/lsfg/lsfg_smrpg.h): build/lsfg/windows/lsfg.dll or build/lsfg/android-<abi>/liblsfg.so.
# From the code of lsfg-vk-android (GameNative, release branch, commit 437208a, with its submodules: dxbc, pe-parse,
# volk) in ../deps/src/lsfg-vk-android (LSFG_SRC for another place):
#   git clone --branch release https://github.com/GameNative/lsfg-vk-android ../deps/src/lsfg-vk-android
#   git -C ../deps/src/lsfg-vk-android submodule update --init --depth 1
# Windows: in MSYS2 UCRT64 (clang). Android: the NDK (NDK, or ANDROID_SDK/ndk/29.0.14206865: from the environment or from
# local.env, see local.env.example). Without CMake: the same file lists as its CMakeLists (framegen, dxbc, pe-parse, volk)
# and tools/lsfg/lsfg_smrpg.cpp.
set -euo pipefail
cd "$(dirname "$0")/../.."
. tools/local_env.sh
T=${1:?target: windows, android-arm64-v8a or android-x86_64}
S=${LSFG_SRC:-../deps/src/lsfg-vk-android}
[ -f "$S/framegen/public/lsfg_3_1.hpp" ] && [ -f "$S/thirdparty/dxbc/CMakeLists.txt" ] ||
    { echo "lsfg: the code of lsfg-vk-android is missing in $S (see the top of this script)"; exit 1; }
O=build/lsfg/$T
mkdir -p "$O/obj"
case "$T" in
windows)
    CC=clang CXX=clang++
    OUT=$O/lsfg.dll
    TFLAGS="-DNOMINMAX -DWIN32_LEAN_AND_MEAN"
    LDFLAGS="-shared -fuse-ld=lld -static -static-libgcc -static-libstdc++ -Wl,--exclude-all-symbols"
    UNICODE=unicode_codecvt  # (the winapi one does not compile on MinGW: char16_t against wchar_t)
    ;;
android-*)
    ABI=${T#android-}
    NDK=${NDK:-${ANDROID_SDK:+$ANDROID_SDK/ndk/29.0.14206865}}
    [ -n "$NDK" ] || { echo "lsfg: the NDK is missing: put ANDROID_SDK or NDK in local.env (see local.env.example) or in the environment"; exit 1; }
    TC=$NDK/toolchains/llvm/prebuilt/windows-x86_64
    case "$ABI" in arm64-v8a) TRIPLE=aarch64-linux-android33 ;; x86_64) TRIPLE=x86_64-linux-android33 ;;
        *) echo "lsfg: unknown ABI $ABI"; exit 1 ;; esac
    # (the NDK's path may have a space: its compilers go through the PATH, ahead of MSYS2's)
    export PATH="$TC/bin:$PATH"
    CC="clang.exe --target=$TRIPLE" CXX="clang++.exe --target=$TRIPLE"
    OUT=$O/liblsfg.so
    TFLAGS="-fPIC -DVK_USE_PLATFORM_ANDROID_KHR"  # (the AHardwareBuffer types of its Android path)
    LDFLAGS="-shared -static-libstdc++ -Wl,-soname,liblsfg.so -Wl,--no-undefined -llog -landroid -Wl,-z,max-page-size=16384"
    UNICODE=unicode_codecvt
    ;;
*) echo "lsfg: unknown target $T"; exit 1 ;;
esac
INC="-I$S/framegen/include -I$S/framegen/public -I$S/framegen/v3.1_include -I$S/framegen/v3.1p_include \
     -I$S/thirdparty/dxbc/include/dxbc -I$S/thirdparty/dxbc/include/spirv -I$S/thirdparty/dxbc/include/util \
     -I$S/thirdparty/dxbc/include/dxvk -I$S/thirdparty/dxbc/include \
     -I$S/thirdparty/pe-parse/pe-parser-library/include -I$S/thirdparty/volk -I$S/src/extract -Itools/lsfg"
COMMON="-O2 -fvisibility=hidden -fvisibility-inlines-hidden -DVK_NO_PROTOTYPES -w $TFLAGS"
# (the objects are only compared with their .cpp: if the sources of lsfg-vk change -another commit, or changes without
# one-, or the API header, or the mipmap shader that goes in, or the options, everything is compiled from scratch)
EXPECTED=437208a  # (the tested commit; MSYS2 has no git: Git for Windows', if it is there)
GIT=$(command -v git || ls "/c/Program Files/Git/cmd/git.exe" 2>/dev/null || true)
if [ -n "$GIT" ] && [ "$("$GIT" -C "$S" rev-parse --short=7 HEAD 2>/dev/null)" != "$EXPECTED" ]; then
    echo "lsfg: warning: $S is not at commit $EXPECTED, the tested one"
fi
# (the fingerprint: all its sources and headers, with their size and time; without depending on git)
STAMP=$({ find "$S/framegen" "$S/thirdparty/dxbc" "$S/thirdparty/pe-parse/pe-parser-library" "$S/thirdparty/volk" \
            "$S/src/extract" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cpp' -o -name '*.inc' \) \
            -printf '%p %s %T@\n' 2>/dev/null | sort
          cat tools/lsfg/lsfg_smrpg.h
          echo "$CC|$CXX|$COMMON|$INC|$LDFLAGS|${NDK:-}"; } | sha256sum | cut -c1-16)
if [ "$(cat "$O/stamp" 2>/dev/null)" != "$STAMP" ]; then
    rm -rf "$O/obj" && mkdir -p "$O/obj" && echo "$STAMP" > "$O/stamp"
fi
# the lists (as in its CMakeLists)
{
    find "$S/framegen/src" "$S/framegen/v3.1_src" "$S/framegen/v3.1p_src" -name '*.cpp' | sed 's/^/c++20 /'
    ls "$S"/thirdparty/dxbc/src/dxbc/*.cpp "$S"/thirdparty/dxbc/src/spirv/*.cpp "$S"/thirdparty/dxbc/src/util/*.cpp | sed 's/^/c++17 /'
    for f in buffer parse $UNICODE; do echo "c++17 $S/thirdparty/pe-parse/pe-parser-library/src/$f.cpp"; done
    echo "c $S/thirdparty/volk/volk.c"
    echo "c++20 tools/lsfg/lsfg_smrpg.cpp"
} > "$O/sources.txt"
compile() {  # <std> <source>: its object (only if it changed)
    local std=$1 src=$2 obj
    obj=$O/obj/$(echo "$src" | sed 's|^\.\./||; s|[/.]|_|g').o
    [ -f "$obj" ] && [ "$obj" -nt "$src" ] && { echo "$obj"; return; }
    if [ "$std" = c ]; then $CC $COMMON $INC -c "$src" -o "$obj" || return 1
    else $CXX -std=$std $COMMON $INC -c "$src" -o "$obj" || return 1; fi
    echo "$obj"
}
export -f compile
export O CC CXX COMMON INC
xargs -P "${JOBS:-$(nproc)}" -L1 bash -c 'compile "$0" "$1"' < "$O/sources.txt" > "$O/objects.txt" || { echo "lsfg: compilation failed"; exit 1; }
[ "$(wc -l < "$O/objects.txt")" = "$(wc -l < "$O/sources.txt")" ] || { echo "lsfg: compilation failed"; exit 1; }
# shellcheck disable=SC2046
$CXX -o "$OUT" $(cat "$O/objects.txt") $LDFLAGS
echo "lsfg: $OUT ($(stat -c %s "$OUT") bytes)"
