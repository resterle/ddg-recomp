#!/bin/sh
# Builds the shareable Linux x86-64 program: RECOMP_JIT (no game code; the program translates the
# player's perfect.exe and compiles it on the first start, host/jit.cpp, host/pyembed.cpp) and packs
#   recomp/build/ddg-recomp-linux-x64.tar.xz: ddg-recomp/{perfect_recomp, libs/libSDL3.so.0,
#   README.txt, SOURCE.txt, LICENSES/}
# On Ubuntu 22.04 (glibc 2.35) with release-deps.sh's tools; used by .github/workflows/release.yml.
# Downloads (SDL3, python-build-standalone, capstone) are kept in recomp/build/release-cache.
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
root=$(dirname "$recomp")
cache=$recomp/build/release-cache
out=$recomp/build/release-linux-x64
LLVM=23
SDL_VERSION=3.4.16
PY_RELEASE=20250612
PY_VERSION=3.12.11
CAPSTONE=5.0.9
RENDERER=${RENDERER:-gl}
mkdir -p "$cache"
cd "$recomp"

# SDL3 (shared, loads X11/Wayland/GL/Vulkan/audio libraries at run time, as the desktop image's)
if [ ! -f "$cache/sdl3/lib/libSDL3.so.0" ]; then
    curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" | tar -xz -C "$cache"
    cmake -S "$cache/SDL3-$SDL_VERSION" -B "$cache/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang-$LLVM -DCMAKE_INSTALL_PREFIX="$cache/sdl3" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_LIBDECOR=ON -DSDL_KMSDRM=ON -DSDL_VULKAN=ON \
        -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_GPU=ON \
        -DSDL_ALSA=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF >/dev/null
    cmake --build "$cache/sdl-build" >/dev/null
    cmake --install "$cache/sdl-build" >/dev/null
    cp "$cache/SDL3-$SDL_VERSION/LICENSE.txt" "$cache/sdl3/LICENSE.txt"
fi

# the static Python (python-build-standalone "full": libpython3.12.a, LLVM bitcode) and capstone
pbs=cpython-$PY_VERSION+$PY_RELEASE-x86_64-unknown-linux-gnu-pgo+lto-full.tar.zst
if [ ! -d "$cache/pbs/python" ]; then
    curl -fsSL -o "$cache/$pbs" "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/$(echo "$pbs" | sed 's/+/%2B/')"
    mkdir -p "$cache/pbs" && tar -I zstd -xf "$cache/$pbs" -C "$cache/pbs" && rm "$cache/$pbs"
fi
if ! ls "$cache"/capstone/*.whl >/dev/null 2>&1; then
    python3 -m pip download "capstone==$CAPSTONE" --only-binary=:all: --no-deps --python-version 3.12 \
        --platform manylinux2014_x86_64 --platform manylinux1_x86_64 -d "$cache/capstone" -q
fi

# the program
llvm=/usr/lib/llvm-$LLVM
rm -f "$out/perfect_recomp"
make -j"$(nproc)" RECOMP_JIT=1 RENDERER="$RENDERER" CC=clang-$LLVM CXX=clang++-$LLVM \
    LLVM_PREFIX=$llvm LLVM_SYSLIBS="$($llvm/bin/llvm-config --link-static --system-libs)" \
    PY_PREFIX="$cache/pbs/python" CAPSTONE_WHEEL="$(ls "$cache"/capstone/*.whl | head -1)" \
    SDL_PREFIX="$cache/sdl3" SDL_LIBS="-L$cache/sdl3/lib -lSDL3" OBJ=build/obj-release-linux-x64 \
    TARGET="$out/perfect_recomp" LDEXTRA="-fuse-ld=lld -Wl,-rpath,\\\$\$ORIGIN/libs"

# the package
pkg=$out/pkg/ddg-recomp
rm -rf "$out/pkg" && mkdir -p "$pkg/libs" "$pkg/LICENSES"
llvm-strip-$LLVM -o "$pkg/perfect_recomp" "$out/perfect_recomp"
cp -L "$cache/sdl3/lib/libSDL3.so.0" "$pkg/libs/"
cp "$here/README-release.txt" "$pkg/README.txt"
cp "$root/LICENSE" "$pkg/LICENSES/ddg-recomp-GPL-3.0.txt"
cp "$cache/sdl3/LICENSE.txt" "$pkg/LICENSES/SDL3-zlib.txt"
cp "$recomp/data/ddgmodernizer-LICENSE.md" "$pkg/LICENSES/DDGModernizer-MIT.md"
cp "$cache/pbs/python/install/lib/python3.12/LICENSE.txt" "$pkg/LICENSES/Python-PSF.txt"
[ -d "$cache/pbs/python/licenses" ] && cp -r "$cache/pbs/python/licenses" "$pkg/LICENSES/python-build-standalone"
python3 -c "import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.stdout.buffer.write(z.read(next(n for n in z.namelist() if '.dist-info/LICENSE' in n)))" "$(ls "$cache"/capstone/*.whl | head -1)" > "$pkg/LICENSES/capstone-BSD.txt"
curl -fsSL "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/LICENSE.TXT" -o "$pkg/LICENSES/LLVM-Apache-2.0-with-LLVM-exception.txt"
commit=$(git -c safe.directory="$root" -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
cat > "$pkg/SOURCE.txt" <<EOF
ddg-recomp is free software under the GNU General Public License v3 (LICENSES/).
This program was built from https://github.com/resterle/ddg-recomp, commit $commit,
with LLVM/clang $($llvm/bin/llvm-config --version), Python $PY_VERSION (python-build-standalone $PY_RELEASE),
capstone $CAPSTONE and SDL $SDL_VERSION.
EOF
tar -C "$out/pkg" -cJf "$recomp/build/ddg-recomp-linux-x64.tar.xz" ddg-recomp
ls -la "$recomp/build/ddg-recomp-linux-x64.tar.xz"
