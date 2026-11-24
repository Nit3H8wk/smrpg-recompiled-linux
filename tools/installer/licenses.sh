#!/bin/bash
# tools/installer/licenses.sh <folder that is distributed>: <folder>/licenses with the license texts of everything that goes
# with the program (LICENSES.txt lists them) and, in licenses/sources, the source code of what requires giving it: Unicorn
# 2.1.4 (GPL-2.0, PyPI's sdist checked by its sha256), which the generator loads into the installer's process, and
# therefore also that of the installer and its builder (tools/installer). The generator's sources go as they are in
# builder/python/gen. From the Makefile (`make installer`), in MSYS2 UCRT64.
set -euo pipefail
cd "$(dirname "$0")/../.."
D=$1/licenses
mkdir -p "$D/sources"
U=/ucrt64/share/licenses
VENV=build/gen-venv/Lib/site-packages

cp LICENSE "$D/LAS-Recomp-LICENSE.txt"
cp tools/installer/LICENSES.txt "$D/README-LICENSES.txt"
cp third_party/amd-fsr/AMD-FSR-SDK-LICENSE.md "$D/"
# (SMRPG) lsfg.dll, Lossless Scaling frame generation (LSFG 3.1): the code of lsfg-vk-android (release branch) with its
# submodules (DXVK's DXBC translator, pe-parse and volk); the player's Lossless.dll NEVER goes with the installer
LSFGV=${LSFG_SRC:-../deps/src/lsfg-vk-android}  # (as tools/lsfg/build.sh)
cp "$LSFGV/LICENSE.md" "$D/lsfg-vk-android-LICENSE.txt"
cp "$LSFGV/thirdparty/dxbc/LICENSE.md" "$D/DXVK-dxbc-LICENSE.md"
cp "$LSFGV/thirdparty/pe-parse/LICENSE" "$D/pe-parse-LICENSE.txt"
cp "$LSFGV/thirdparty/volk/LICENSE.md" "$D/volk-LICENSE.txt"
cp build/deps-win/src/ffmpeg/COPYING.LGPLv2.1 "$D/FFmpeg-LGPLv2.1.txt"
cp build/deps-win/src/ffmpeg-9.0.2.tar.xz "$D/sources/"  # (LGPL: its source goes with the binary; sha256 in build_deps_windows.sh)
cp third_party/ryujinx/LICENSE.txt "$D/Ryujinx-LICENSE.txt"  # (runtime/services/audio.c: the mixer and the reverb, ported)
cp third_party/fonts/Baloo2/OFL.txt "$D/Baloo2-OFL.txt"
cp third_party/fonts/BIZUDPGothic/OFL.txt "$D/BIZUDPGothic-OFL.txt"
cp third_party/fonts/DejaVu/LICENSE "$D/DejaVu-LICENSE.txt"
cp third_party/imgui/LICENSE.txt "$D/DearImGui-LICENSE.txt"
cp third_party/smaa/LICENSE.txt "$D/SMAA-LICENSE.txt"
# the MSYS2 DLLs that go with the program and with the builder (and zstd, inside the installer)
cp $U/SDL3/LICENSE.txt "$D/SDL3-LICENSE.txt"
cp $U/opus/COPYING "$D/Opus-COPYING.txt"
cp $U/shaderc/LICENSE "$D/shaderc-LICENSE.txt"
cp $U/glslang/LICENSE.txt "$D/glslang-LICENSE.txt"
cp $U/spirv-tools/LICENSE "$D/SPIRV-Tools-LICENSE.txt"
cp $U/libiconv/COPYING.LIB "$D/libiconv-COPYING.LIB.txt"
cp $U/winpthreads/COPYING "$D/winpthreads-COPYING.txt"
cp $U/gcc/COPYING.RUNTIME "$D/GCC-COPYING.RUNTIME.txt"
cp $U/gcc/COPYING3 "$D/GCC-COPYING3.txt"
cp $U/crt/COPYING.MinGW-w64-runtime.txt "$D/MinGW-w64-runtime-COPYING.txt"
cp $U/headers/COPYING "$D/MinGW-w64-headers-COPYING.txt"
cp $U/zstd/LICENSE "$D/zstd-LICENSE.txt"
cp $U/vulkan-memory-allocator/LICENSE.txt "$D/VMA-LICENSE.txt"  # (all of it is inside smrpg-recomp.exe: vma.cpp)
cp $U/llvm/LICENSE "$D/LLVM-LICENSE.txt"      # (LLVM, clang and lld: Apache 2.0 with the LLVM exceptions)
cp $U/libffi/LICENSE "$D/libffi-LICENSE.txt"
cp $U/libxml2/COPYING "$D/libxml2-COPYING.txt"
cp $U/zlib/LICENSE "$D/zlib-LICENSE.txt"
# the generator: the embedded Python, Capstone, lz4, Unicorn
build/gen-venv/Scripts/python.exe -I -c "import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read('LICENSE.txt'))" \
    build/sources/python-3.12.10-embed-amd64.zip > "$D/Python-LICENSE.txt"
cp $VENV/capstone-5.0.9.dist-info/LICENSE.TXT "$D/Capstone-LICENSE.txt"
cp "$(ls $VENV/lz4-4.4.5.dist-info/licenses/* | head -1)" "$D/lz4-LICENSE.txt"
# (the one above is that of the Python bindings; the LZ4 library in C inside them is BSD-2: its notice, from its source
# code, PyPI's sdist checked by its sha256)
L4=build/sources/lz4-4.4.5.tar.gz
L4SHA=5f0b9e53c1e82e88c10d7c180069363980136b9d7a8306c4dca4f760d60c39f0
[ -f $L4 ] || { mkdir -p build/sources; curl -fsSL -o $L4 https://files.pythonhosted.org/packages/source/l/lz4/lz4-4.4.5.tar.gz; }
echo "$L4SHA  $L4" | sha256sum -c --quiet
tar -xzOf $L4 lz4-4.4.5/lz4libs/lz4.h | awk 'NR==1,/\*\//' > "$D/LZ4-library-BSD-2.txt"
# where exactly what is distributed from MSYS2 comes from (its source packages: what the GPL/LGPL licenses of libgcc,
# libstdc++, libiconv... ask for)
MSYS2_SRC=$D/MSYS2-sources.txt
{
    echo "MSYS2 UCRT64 packages that the libraries coming with the program and with builder\\ are taken from,"
    echo "and their full source code (MSYS2 source packages, with the patches and the build recipe):"
    for base in $(for p in libstdc++ libgcc gcc crt headers libwinpthread libiconv llvm-libs clang-libs lld libffi libxml2 zlib zstd \
                      sdl3 opus shaderc glslang spirv-tools; do
                      d=$(ls -d /var/lib/pacman/local/mingw-w64-ucrt-x86_64-$p-[0-9]* 2>/dev/null | head -1)
                      [ -n "$d" ] && echo "$(awk '/%BASE%/{getline; print}' $d/desc)-$(awk '/%VERSION%/{getline; print}' $d/desc)"
                  done | sort -u); do
        echo "  $base: https://repo.msys2.org/mingw/sources/$base.src.tar.zst"
    done
} > "$MSYS2_SRC"
grep -q "mingw-w64-gcc-" "$MSYS2_SRC" && grep -q "mingw-w64-llvm-" "$MSYS2_SRC" || { echo "licenses: packages missing in $MSYS2_SRC"; exit 1; }
S=build/sources/unicorn-2.1.4.tar.gz
SHA=00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff  # (pypi.org/pypi/unicorn/2.1.4/json, the sdist)
[ -f $S ] || { mkdir -p build/sources; curl -fsSL -o $S \
    https://files.pythonhosted.org/packages/b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/unicorn-2.1.4.tar.gz; }
echo "$SHA  $S" | sha256sum -c --quiet
cp $S "$D/sources/"
tar -xzOf $S unicorn-2.1.4/src/qemu/COPYING > "$D/Unicorn-GPL-2.0.txt"
# the installer and its builder (Unicorn is loaded into their process): their sources
build/gen-venv/Scripts/python.exe -I - "$(cygpath -w "$D/sources/installer-source.zip")" <<'EOF'
import glob, sys, zipfile
fs = sorted(glob.glob('tools/installer/*')) + ['LICENSE', 'Makefile', 'docs/icon/icon.ico',
                                                  'runtime/game_addresses/romfs_files.h']  # (installer.c includes it)
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for f in fs:
        z.write(f, 'installer-source/' + f.replace('\\', '/'))
EOF
echo "licenses: $(ls "$D" | wc -l) files, sources: $(ls "$D/sources" | tr '\n' ' ')"
