#!/bin/sh
# Builds the shareable macOS (Apple Silicon) program: RECOMP_JIT (no game code; the program
# translates the player's perfect.exe and compiles it on the first start) as an app bundle:
#   recomp/build/ddg-recomp-macos-arm64.zip: Densha de GO! FINAL.app (Contents/MacOS/perfect_recomp,
#   Frameworks/libSDL3.0.dylib, Resources/LICENSES, README.txt, SOURCE.txt)
# Needs Homebrew's llvm and lld (clang and lld read python-build-standalone's LLVM bitcode, and
# llvm's static libraries are linked in), cmake and ninja. Used by .github/workflows/release.yml.
# Not yet tested on a Mac (only the Linux build was run): see docs/DEVELOPMENT.md.
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
root=$(dirname "$recomp")
cache=$recomp/build/release-cache-macos
out=$recomp/build/release-macos-arm64
SDL_VERSION=3.4.16
PY_RELEASE=20250612
PY_VERSION=3.12.11
CAPSTONE=5.0.9
llvm=$(brew --prefix llvm)
lld=$(brew --prefix lld)
major=$("$llvm/bin/llvm-config" --version | cut -d. -f1)
[ "$major" = 23 ] || { echo "Homebrew llvm is $major; host/jit.cpp is written for LLVM 23" >&2; exit 1; }
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-14.0}    # Homebrew's LLVM libraries
mkdir -p "$cache"
cd "$recomp"

# SDL3 (shared, in the bundle's Frameworks)
if [ ! -f "$cache/sdl3/lib/libSDL3.0.dylib" ]; then
    curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" | tar -xz -C "$cache"
    cmake -S "$cache/SDL3-$SDL_VERSION" -B "$cache/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$cache/sdl3" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF >/dev/null
    cmake --build "$cache/sdl-build" >/dev/null
    cmake --install "$cache/sdl-build" >/dev/null
    cp "$cache/SDL3-$SDL_VERSION/LICENSE.txt" "$cache/sdl3/LICENSE.txt"
fi

# the static Python and capstone
pbs=cpython-$PY_VERSION+$PY_RELEASE-aarch64-apple-darwin-pgo+lto-full.tar.zst
if [ ! -d "$cache/pbs/python" ]; then
    curl -fsSL -o "$cache/$pbs" "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/$(echo "$pbs" | sed 's/+/%2B/')"
    mkdir -p "$cache/pbs" && zstd -dc "$cache/$pbs" | tar -x -C "$cache/pbs" && rm "$cache/$pbs"
fi
if ! ls "$cache"/capstone/*.whl >/dev/null 2>&1; then
    python3 -m pip download "capstone==$CAPSTONE" --only-binary=:all: --no-deps --python-version 3.12 \
        --platform macosx_11_0_arm64 -d "$cache/capstone" -q
fi

# the program (Metal renderer)
rm -f "$out/perfect_recomp"
make -j"$(sysctl -n hw.ncpu)" RECOMP_JIT=1 RENDERER=gpu CC="$llvm/bin/clang" CXX="$llvm/bin/clang++" \
    LLVM_PREFIX="$llvm" LLVM_SYSLIBS="$("$llvm/bin/llvm-config" --link-static --system-libs)" \
    PY_PREFIX="$cache/pbs/python" CAPSTONE_WHEEL="$(ls "$cache"/capstone/*.whl | head -1)" \
    SDL_PREFIX="$cache/sdl3" SDL_LIBS="-L$cache/sdl3/lib -lSDL3" OBJ=build/obj-release-macos-arm64 \
    TARGET="$out/perfect_recomp" LDEXTRA="-fuse-ld=$lld/bin/ld64.lld -Wl,-rpath,@executable_path/../Frameworks"

# the app bundle
app="$out/Densha de GO! FINAL.app"
rm -rf "$app" && mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources/LICENSES"
"$llvm/bin/llvm-strip" -o "$app/Contents/MacOS/perfect_recomp" "$out/perfect_recomp"
cp -L "$cache/sdl3/lib/libSDL3.0.dylib" "$app/Contents/Frameworks/"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>perfect_recomp</string>
  <key>CFBundleIdentifier</key><string>io.github.resterle.ddg-recomp</string>
  <key>CFBundleName</key><string>Densha de GO! FINAL</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${DDG_VERSION:-0}</string>
  <key>LSMinimumSystemVersion</key><string>$MACOSX_DEPLOYMENT_TARGET</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
cp "$recomp/linux/README-release.txt" "$app/Contents/Resources/README.txt"
lic="$app/Contents/Resources/LICENSES"
cp "$root/LICENSE" "$lic/ddg-recomp-GPL-3.0.txt"
cp "$cache/sdl3/LICENSE.txt" "$lic/SDL3-zlib.txt"
cp "$recomp/data/ddgmodernizer-LICENSE.md" "$lic/DDGModernizer-MIT.md"
cp "$cache/pbs/python/install/lib/python3.12/LICENSE.txt" "$lic/Python-PSF.txt"
[ -d "$cache/pbs/python/licenses" ] && cp -r "$cache/pbs/python/licenses" "$lic/python-build-standalone"
python3 -c "import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); sys.stdout.buffer.write(z.read(next(n for n in z.namelist() if '.dist-info/LICENSE' in n)))" "$(ls "$cache"/capstone/*.whl | head -1)" > "$lic/capstone-BSD.txt"
curl -fsSL "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-23.1.1/llvm/LICENSE.TXT" -o "$lic/LLVM-Apache-2.0-with-LLVM-exception.txt"
commit=$(git -c safe.directory="$root" -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
cat > "$app/Contents/Resources/SOURCE.txt" <<EOF
ddg-recomp is free software under the GNU General Public License v3 (LICENSES/).
This program was built from https://github.com/resterle/ddg-recomp, commit $commit,
with LLVM/clang $("$llvm/bin/llvm-config" --version), Python $PY_VERSION (python-build-standalone $PY_RELEASE),
capstone $CAPSTONE and SDL $SDL_VERSION.
EOF
# unsigned (ad hoc): Apple Silicon runs only signed code; the player confirms the first start
codesign --force -s - "$app/Contents/Frameworks/libSDL3.0.dylib"
codesign --force -s - "$app"
rm -f "$recomp/build/ddg-recomp-macos-arm64.zip"
ditto -c -k --keepParent "$app" "$recomp/build/ddg-recomp-macos-arm64.zip"
ls -la "$recomp/build/ddg-recomp-macos-arm64.zip"
