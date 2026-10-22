#!/bin/sh
# Cross-builds the aarch64 Linux version on an x86-64 Linux host: clang 16 / lld 16 in an x86-64
# container (linux/cross/Dockerfile) against a sysroot exported once from the ddg-recomp-arm64 image
# (linux/Dockerfile; that image runs under qemu, where compiling the game would take hours).
# Same output as build.sh: recomp/build/arm64/ports/ and recomp/build/ddg-recomp-arm64.zip.
# Needs docker with qemu binfmt for arm64 (only to build ddg-recomp-arm64 once, sysroot-arm64.sh).
# linux/perfect.profdata is used when present (PGO_MODE=off ignores it).
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
root=$(dirname "$recomp")
sysroot=build/sysroot-arm64                       # relative to recomp/
jobs=$(nproc)
docker image inspect ddg-recomp-cross >/dev/null 2>&1 || docker build -t ddg-recomp-cross "$here/cross"
sh "$here/sysroot-arm64.sh"
pgo=''
if [ "${PGO_MODE:-auto}" != off ] && [ -f "$here/perfect.profdata" ]; then
    pgo='-fprofile-use=/src/recomp/linux/perfect.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin'
    echo "profile-guided build (linux/perfect.profdata)"
fi
docker run --rm -u "$(id -u):$(id -g)" -v "$root:/src" -w /src/recomp ddg-recomp-cross sh -ec "
    sr=/src/recomp/$sysroot
    rm -f build/arm64/perfect_recomp
    make -j$jobs RENDERER=gl OPT='-O2' TUNE='-mcpu=cortex-a35' PGOFLAGS='$pgo' OBJ=build/obj-arm64-cross \
        TARGET=build/arm64/perfect_recomp MEMBASE=0x400000000 \
        CC=\"clang-16 --target=aarch64-linux-gnu --sysroot=\$sr\" CXX=\"clang++-16 --target=aarch64-linux-gnu --sysroot=\$sr\" \
        SDL_PREFIX=\$sr/opt/sdl3 SDL_LIBS=\"-L\$sr/opt/sdl3/lib -lSDL3\" \
        LDEXTRA='--ld-path=/usr/bin/ld.lld-16 -pthread -ldl -Wl,-rpath,\\\$\$ORIGIN/libs'
"
mkdir -p "$recomp/build/arm64/libs"
cp -L "$recomp/$sysroot/opt/sdl3/lib/libSDL3.so.0" "$recomp/build/arm64/libs/"
sh "$here/package.sh"
