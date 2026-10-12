#!/bin/sh
# Builds the aarch64 Linux version (OpenGL ES renderer) in Docker and packages it for
# PortMaster-style handheld firmwares (ArkOS etc.):  recomp/linux/build.sh
# Output: recomp/build/arm64/ports/  (copy its contents to the SD card's ports folder)
# With recomp/linux/perfect.profdata present (made by pgo.sh) the build is profile-guided.
# PGO_MODE=gen builds the instrumented binary pgo.sh uses (build/arm64-pgogen, not packaged);
# PGO_MODE=off ignores the profile.
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
root=$(dirname "$recomp")
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
mode=${PGO_MODE:-auto}
tune='-mcpu=cortex-a35'
ldx=''
if [ "$mode" = gen ]; then
    # RECOMP_PGO_GEN: RECOMP_EXIT_AFTER writes the profile before quitting
    obj=build/obj-arm64-pgogen; out=build/arm64-pgogen; pgo='-fprofile-generate -DRECOMP_PGO_GEN'
elif [ "$mode" != off ] && [ -f "$here/perfect.profdata" ]; then
    obj=build/obj-arm64-pgo; out=build/arm64
    pgo='-fprofile-use=/src/recomp/linux/perfect.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin'
    echo "profile-guided build (linux/perfect.profdata)"
else
    obj=build/obj-arm64; out=build/arm64; pgo=''
fi
docker image inspect ddg-recomp-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ddg-recomp-arm64 "$here"
docker run --rm --platform linux/arm64 -v "$root:/src" -w /src/recomp ddg-recomp-arm64 sh -ec "
    rm -f $out/perfect_recomp      # always relink (make does not track LDEXTRA/PGO flags)
    make -j$jobs RENDERER=gl OPT='-O2' TUNE='$tune' PGOFLAGS='$pgo' OBJ=$obj TARGET=$out/perfect_recomp \
        CC=clang-16 CXX=clang++-16 SDL_PREFIX=/opt/sdl3 SDL_LIBS='-L/opt/sdl3/lib -lSDL3' \
        LDEXTRA='--ld-path=/usr/bin/ld.lld-16 -pthread -ldl $ldx'
    patchelf --set-rpath '\$ORIGIN/libs' $out/perfect_recomp
    mkdir -p $out/libs
    cp -L /opt/sdl3/lib/libSDL3.so.0 $out/libs/
"
[ "$mode" = gen ] || "$here/package.sh"
