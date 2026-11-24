#!/bin/bash
# tools/installer/toolchain.sh <output>: the builder\ folder that goes with the program (`make installer`), with everything
# builder.dll needs to build the game code on the PC of whoever installs it, without anything of this PC:
#   - builder.dll (already in <output>) and its DLLs: libclang-cpp, libLLVM and what they load (from MSYS2 UCRT64);
#   - clang\include: clang's headers (all of them: they are its own, 16 MB);
#   - mingw\include: of MinGW-w64's, only the ones the game code includes (cpu.h and simd.h, which includes simd_fp.h;
#     gamelib_entry.c and game_overrides.c; with the game code's options, for x86-64-v3, -v2 and x86-64): the list comes
#     from clang -MD;
#   - mingw\lib: MinGW-w64's CRT and libgcc, as in `make`'s link line (game_builder.cpp link_lib);
#   - python\: Python 3.12 "embeddable" from python.org (its sha256 is pinned), isolated with python312._pth, with capstone,
#     unicorn and lz4 (the ones in the generator's environment, build/gen-venv) and the generator (tools/gen.py and its
#     modules and data).
# At the end it checks that a test compilation with only these headers goes well.
set -euo pipefail
cd "$(dirname "$0")/../.."
O=$1
U=/ucrt64
CLANG_VER=$(ls $U/lib/clang | sort -V | tail -1)
GCC_DIR=$(ls -d $U/lib/gcc/x86_64-w64-mingw32/* | sort -V | tail -1)
W=build/builder-work
rm -rf "$W" "$O/clang" "$O/mingw" "$O/python"
mkdir -p "$W" "$O/clang" "$O/mingw/include" "$O/mingw/lib" "$O/python"

# 1. the DLLs that builder.dll and libclang-cpp / libLLVM load (MSYS2's; those of an earlier version, out).
#    libffi-8.dll has the same name as Python's: LLVM loads the one here first and _ctypes uses it (Windows reuses the DLL
#    already loaded under that name); it is the same API, and MinGW's libffi takes the FFI_WIN64 convention that MSVC's
#    Python asks for. Python's own copy (python\libffi-8.dll) stays as a fallback.
find "$O" -maxdepth 1 -name '*.dll' ! -name builder.dll -delete
for d in $(ldd "$O/builder.dll" | awk '/ucrt64/ {print $3}' | sort -u); do cp "$d" "$O/"; done

# 2. clang's headers (all of them) and the MinGW-w64 ones the game code uses
cp -R $U/lib/clang/$CLANG_VER/include "$O/clang/include"
printf '#include "cpu.h"\n#include "simd.h"\n' > $W/probe.c  # (simd_fp.h is a template of simd.h: it does not go alone)
for cpu in x86-64-v3 x86-64-v2 x86-64; do
    for src in $W/probe.c tools/gamelib_entry.c tools/game_overrides.c; do
        clang -O1 -fno-strict-aliasing -fvisibility=hidden -D_FILE_OFFSET_BITS=64 -march=$cpu -Iruntime '-DGAMELIB_ID="0"' \
            -c $src -o $W/o.o -MD -MF $W/$cpu-$(basename $src).d
    done
done
n=0
for h in $(cat $W/*.d | tr ' ' '\n' | tr -d '\r\\' | grep -i "msys64/ucrt64/include/" | sed 's|.*/ucrt64/include/||I' | sort -u); do
    mkdir -p "$O/mingw/include/$(dirname "$h")"
    cp "$U/include/$h" "$O/mingw/include/$h"
    n=$((n + 1))
done
echo "builder: $n MinGW-w64 headers"

# 3. MinGW-w64's CRT and libgcc (`make`'s link line: clang -shared -###)
cp $U/lib/dllcrt2.o $GCC_DIR/crtbegin.o $GCC_DIR/crtend.o "$O/mingw/lib/"
for l in mingw32 moldname mingwex msvcrt pthread advapi32 shell32 user32 kernel32; do cp $U/lib/lib$l.a "$O/mingw/lib/"; done
cp $GCC_DIR/libgcc.a "$O/mingw/lib/"
if [ -f $GCC_DIR/libgcc_s.a ]; then cp $GCC_DIR/libgcc_s.a "$O/mingw/lib/"; else cp $U/lib/libgcc_s.a "$O/mingw/lib/"; fi

# 4. the embedded Python (isolated), its packages and the generator
PYZ=build/sources/python-3.12.10-embed-amd64.zip
PYSHA=4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3  # (python.org; signed by the PSF)
[ -f $PYZ ] || { mkdir -p build/sources; curl -fsSL -o $PYZ https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip; }
echo "$PYSHA  $PYZ" | sha256sum -c --quiet
build/gen-venv/Scripts/python.exe -I -m zipfile -e $PYZ "$O/python"  # (no unzip: the Python of the generator's environment)
# (what the generator does not use: network, encryption, databases, python.exe itself)
rm -f "$O/python/"{python.exe,pythonw.exe,python.cat,python312._pth,_ssl.pyd,_hashlib.pyd,libssl-3.dll,libcrypto-3.dll,_sqlite3.pyd,sqlite3.dll,_msi.pyd,_wmi.pyd,_socket.pyd,_asyncio.pyd,_overlapped.pyd,_uuid.pyd,winsound.pyd}
cp tools/installer/python312._pth "$O/python/"
mkdir -p "$O/python/Lib/site-packages" "$O/python/gen/tools/rando" "$O/python/gen/runtime/game_addresses"
for pk in capstone unicorn lz4; do cp -R build/gen-venv/Lib/site-packages/$pk "$O/python/Lib/site-packages/"; done
find "$O/python/Lib/site-packages" -name '__pycache__' -prune -exec rm -rf {} +
cp tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py \
   tools/game_addresses.py "$O/python/gen/tools/"
cp tools/rando/patch_variants.json "$O/python/gen/tools/rando/"
cp runtime/game_addresses/*.h "$O/python/gen/runtime/game_addresses/"

# 5. the check: the game code compiles with only these headers (clang, -nostdinc)
for src in $W/probe.c tools/game_overrides.c; do
    clang -O1 -fno-strict-aliasing -march=x86-64-v3 -nostdinc -isystem "$O/clang/include" -isystem "$O/mingw/include" \
        -Iruntime -c $src -o $W/o2.o || { echo "builder: $src does not compile with the copied headers"; exit 1; }
done
# 6. at the top level, only what is expected (anything else, a forgotten temporary file..., would go into the installer)
extra=$(cd "$O" && ls -A | grep -vxE 'builder\.dll|[A-Za-z0-9_+.-]+\.dll|clang|mingw|python' || true)
[ -z "$extra" ] || { echo "builder: unexpected in $O: $extra"; exit 1; }
echo "builder: $(find "$O" -type f | wc -l) files, $(du -sh "$O" | cut -f1)"
