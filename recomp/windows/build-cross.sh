#!/bin/sh
# Builds the Windows x86-64 version (with the game code from build/game, like build-desktop.sh:
# for your own use, not for passing on) on Linux with llvm-mingw:
#   build/win-x64/perfect_recomp.exe and SDL3.dll (SDL GPU renderer on Vulkan)
# llvm-mingw and SDL3's MinGW development package are downloaded into build/win-cache once
# (or LLVM_MINGW=<llvm-mingw dir>, SDL3_MINGW=<SDL3-x.y.z/x86_64-w64-mingw32>).
# Test under wine: wine build/win-x64/perfect_recomp.exe 'C:\path\to\perfect' --play
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
cd "$recomp"
LLVM_MINGW_RELEASE=20261006
SDL_VERSION=3.4.16
cache=build/win-cache
out=build/win-x64
mkdir -p "$cache" "$out"
if [ -z "$LLVM_MINGW" ]; then
    LLVM_MINGW=$cache/llvm-mingw-$LLVM_MINGW_RELEASE-ucrt-ubuntu-22.04-x86_64
    [ -d "$LLVM_MINGW" ] || curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_RELEASE/llvm-mingw-$LLVM_MINGW_RELEASE-ucrt-ubuntu-22.04-x86_64.tar.xz" | tar -xJ -C "$cache"
fi
if [ -z "$SDL3_MINGW" ]; then
    SDL3_MINGW=$cache/SDL3-$SDL_VERSION/x86_64-w64-mingw32
    [ -d "$SDL3_MINGW" ] || curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-devel-$SDL_VERSION-mingw.tar.gz" | tar -xz -C "$cache"
fi
bin=$(cd "$LLVM_MINGW/bin" && pwd)
rm -f "$out/perfect_recomp.exe"
make -j"$(nproc)" CC="$bin/x86_64-w64-mingw32-clang" CXX="$bin/x86_64-w64-mingw32-clang++" \
    SDL_PREFIX="$SDL3_MINGW" SDL_CFLAGS="-I$SDL3_MINGW/include" OBJ=build/obj-win-x64 \
    TARGET="$out/perfect_recomp.exe" LDEXTRA="-static"
cp "$SDL3_MINGW/bin/SDL3.dll" "$out/"
echo "built $out/perfect_recomp.exe"
