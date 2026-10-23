#!/bin/sh
# Builds the shareable Windows x86-64 program on Linux: RECOMP_JIT (no game code; the program
# translates the player's perfect.exe and compiles it on the first start):
#   recomp/build/ddg-recomp-windows-x64.zip: ddg-recomp/perfect_recomp.exe, SDL3.dll,
#   python312.dll, vcruntime140*.dll, README.txt, LICENSES/, SOURCE.txt
# For x86_64-pc-windows-msvc, the ABI of LLVM's and python-build-standalone's Windows releases:
# compiled with llvm-mingw's clang and lld-link (any clang 23 would do) against the MSVC C
# runtime and Windows SDK that xwin downloads from Microsoft (XWIN_ACCEPT_LICENSE=1 accepts
# Microsoft's licence for them; the release workflow sets it). The static C runtime (/MT), as
# LLVM's libraries are built with it. Used by .github/workflows/release.yml.
# Test under wine: wine build/release-win-x64/ddg-recomp/perfect_recomp.exe
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
root=$(dirname "$recomp")
cache=$recomp/build/release-cache-win
out=$recomp/build/release-win-x64
LLVM_MINGW_RELEASE=20261006
XWIN_VERSION=0.10.0
SDL_VERSION=3.4.16
PY_RELEASE=20250612
PY_VERSION=3.12.11
CAPSTONE=5.0.9
ZLIB_VERSION=1.3.1
ZSTD_VERSION=1.5.7
LLVM_VERSION=23.1.4                     # host/jit.cpp is written for LLVM 23
mkdir -p "$cache"
cd "$recomp"

# the compiler: llvm-mingw's clang, lld and llvm-nm (Linux binaries)
mingw=$cache/llvm-mingw-$LLVM_MINGW_RELEASE-ucrt-ubuntu-22.04-x86_64
[ -d "$mingw" ] || curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_RELEASE/llvm-mingw-$LLVM_MINGW_RELEASE-ucrt-ubuntu-22.04-x86_64.tar.xz" | tar -xJ -C "$cache"
bin=$mingw/bin

# the MSVC C runtime and the Windows SDK
xwin=$cache/xwin
if [ ! -d "$xwin/crt/lib/x86_64" ]; then
    [ "$XWIN_ACCEPT_LICENSE" = 1 ] || { echo "set XWIN_ACCEPT_LICENSE=1 to accept Microsoft's licence for the C runtime and the Windows SDK"; exit 1; }
    curl -fsSL "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz" | tar -xz -C "$cache"
    "$cache/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl/xwin" --accept-license --arch x86_64 \
        --cache-dir "$cache/xwin-dl" splat --output "$xwin"
    rm -rf "$cache/xwin-dl"
fi

# LLVM (its static libraries and headers; 260 MB download, 3 GB unpacked)
llvm=$cache/llvm
if [ ! -f "$llvm/lib/LLVMOrcJIT.lib" ]; then
    rm -rf "$llvm" "$cache/clang+llvm-$LLVM_VERSION-x86_64-pc-windows-msvc"
    curl -fsSL "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/clang+llvm-$LLVM_VERSION-x86_64-pc-windows-msvc.tar.zst" \
        | zstd -dc --long=31 | tar -x -C "$cache"
    mv "$cache/clang+llvm-$LLVM_VERSION-x86_64-pc-windows-msvc" "$llvm"
    rm -rf "$llvm/bin" "$llvm/libexec" "$llvm/share" "$llvm/lib/clang"      # Windows programs: not needed
fi

# Python (python312.dll and its standard library) and capstone
pbs=cpython-$PY_VERSION+$PY_RELEASE-x86_64-pc-windows-msvc-pgo-full.tar.zst
if [ ! -d "$cache/pbs/python" ]; then
    mkdir -p "$cache/pbs"
    curl -fsSL "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/$(echo "$pbs" | sed 's/+/%2B/')" \
        | zstd -dc | tar -x -C "$cache/pbs"
fi
py=$cache/pbs/python/install
if ! ls "$cache"/capstone/*.whl >/dev/null 2>&1; then
    python3 -m pip download "capstone==$CAPSTONE" --only-binary=:all: --no-deps --python-version 3.12 \
        --platform win_amd64 -d "$cache/capstone" -q
fi

# SDL3 (Visual C++ development package)
sdl=$cache/SDL3-$SDL_VERSION
if [ ! -f "$sdl/lib/x64/SDL3.lib" ]; then
    curl -fsSL -o "$cache/sdl3-vc.zip" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-devel-$SDL_VERSION-VC.zip"
    (cd "$cache" && python3 -m zipfile -e sdl3-vc.zip . && rm sdl3-vc.zip)
fi

# the compiler flags for x86_64-pc-windows-msvc with the static C runtime
# (the system headers after clang's own, as clang-cl orders them)
target="--target=x86_64-pc-windows-msvc -fms-runtime-lib=static -fms-compatibility-version=19.40 -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS"
for d in crt/include sdk/include/ucrt sdk/include/um sdk/include/shared; do
    target="$target -Xclang -internal-isystem -Xclang $xwin/$d"
done

# zlib and zstd: LLVM's libraries are built with them, which its release does not include
zlibs=$cache/zlibs
if [ ! -f "$zlibs/zstd.lib" ]; then
    mkdir -p "$zlibs/obj-z" "$zlibs/obj-zstd"
    curl -fsSL "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz" | tar -xz -C "$zlibs"
    curl -fsSL "https://github.com/facebook/zstd/releases/download/v$ZSTD_VERSION/zstd-$ZSTD_VERSION.tar.gz" | tar -xz -C "$zlibs"
    for f in "$zlibs/zlib-$ZLIB_VERSION"/*.c; do
        "$bin/clang" $target -O2 -c "$f" -o "$zlibs/obj-z/$(basename "$f" .c).o"
    done
    for f in "$zlibs/zstd-$ZSTD_VERSION"/lib/common/*.c "$zlibs/zstd-$ZSTD_VERSION"/lib/compress/*.c "$zlibs/zstd-$ZSTD_VERSION"/lib/decompress/*.c; do
        "$bin/clang" $target -O2 -DZSTD_DISABLE_ASM -c "$f" -o "$zlibs/obj-zstd/$(basename "$f" .c).o"
    done
    "$bin/llvm-lib" /out:"$zlibs/zlib.lib" "$zlibs"/obj-z/*.o
    "$bin/llvm-lib" /out:"$zlibs/zstd.lib" "$zlibs"/obj-zstd/*.o
fi

# the program (SDL GPU on Vulkan), with its manifest as a resource (lld-link would need mt.exe to
# merge it into its own) and 16 MB of stack for the main thread (1 MB by default), on which the
# translator and clang run when the settings window is skipped (host/jit.cpp kStack)
rm -f "$out/perfect_recomp.exe"
mkdir -p "$out"
printf '1 24 "%s"\n' "$here/perfect_recomp.manifest" > "$out/perfect_recomp.rc"
"$bin/llvm-rc" -no-preprocess -fo "$out/perfect_recomp.res" "$out/perfect_recomp.rc"
make -j"$(nproc)" RECOMP_JIT=1 RENDERER=gpu CC="$bin/clang $target" CXX="$bin/clang++ $target" NM="$bin/llvm-nm" \
    LLVM_PREFIX="$llvm" LLVM_SYSLIBS="$zlibs/zlib.lib $zlibs/zstd.lib -lpsapi -lshell32 -lole32 -luuid -ladvapi32 -lws2_32 -lntdll -lversion" \
    PY_PREFIX="$cache/pbs/python" PY_RUN=python3 CAPSTONE_WHEEL="$(ls "$cache"/capstone/*.whl | head -1)" \
    SDL_PREFIX="$sdl" SDL_CFLAGS="-I$sdl/include" SDL_LIBS="$sdl/lib/x64/SDL3.lib" OBJ=build/obj-release-win-x64 \
    TARGET="$out/perfect_recomp.exe" LTO=0 \
    LDEXTRA="-fuse-ld=lld -L$xwin/crt/lib/x86_64 -L$xwin/sdk/lib/um/x86_64 -L$xwin/sdk/lib/ucrt/x86_64 -L$py/libs $out/perfect_recomp.res -Wl,/manifest:no -Wl,/stack:16777216"

# the package
pkg=$out/ddg-recomp
rm -rf "$pkg" && mkdir -p "$pkg/LICENSES"
"$bin/llvm-strip" -o "$pkg/perfect_recomp.exe" "$out/perfect_recomp.exe" 2>/dev/null || cp "$out/perfect_recomp.exe" "$pkg/"
cp "$sdl/lib/x64/SDL3.dll" "$py/python312.dll" "$py/vcruntime140.dll" "$py/vcruntime140_1.dll" "$pkg/"
sed 's/$/\r/' "$recomp/windows/README-release.txt" > "$pkg/README.txt"
lic=$pkg/LICENSES
cp "$root/LICENSE" "$lic/ddg-recomp-GPL-3.0.txt"
cp "$sdl/LICENSE.txt" "$lic/SDL3-zlib.txt"
cp "$recomp/data/ddgmodernizer-LICENSE.md" "$lic/DDGModernizer-MIT.md"
cp "$py/LICENSE.txt" "$lic/Python-PSF.txt"
[ -d "$cache/pbs/python/licenses" ] && cp -r "$cache/pbs/python/licenses" "$lic/python-build-standalone"
python3 -c "import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.stdout.buffer.write(z.read(next(n for n in z.namelist() if '.dist-info/LICENSE' in n)))" "$(ls "$cache"/capstone/*.whl | head -1)" > "$lic/capstone-BSD.txt"
curl -fsSL "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$LLVM_VERSION/llvm/LICENSE.TXT" -o "$lic/LLVM-Apache-2.0-with-LLVM-exception.txt"
commit=$(git -c safe.directory="$root" -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
cat > "$pkg/SOURCE.txt" <<EOF
ddg-recomp is free software under the GNU General Public License v3 (LICENSES/).
This program was built from https://github.com/resterle/ddg-recomp, commit $commit,
with LLVM/clang $LLVM_VERSION, Python $PY_VERSION (python-build-standalone $PY_RELEASE),
capstone $CAPSTONE and SDL $SDL_VERSION; the Microsoft C runtime is linked in.
EOF
rm -f "$recomp/build/ddg-recomp-windows-x64.zip"
(cd "$out" && python3 -m zipfile -c "$recomp/build/ddg-recomp-windows-x64.zip" ddg-recomp)
ls -la "$recomp/build/ddg-recomp-windows-x64.zip"
