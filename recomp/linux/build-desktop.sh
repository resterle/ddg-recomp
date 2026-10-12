#!/bin/sh
# Builds the x86-64 Linux desktop version with the SDL GPU renderer (Graphic.cpp, Vulkan) on
# this machine: build/linux-x64/perfect_recomp plus libs/libSDL3.so.0 (the system's SDL3, found
# through the rpath $ORIGIN/libs), packed as build/ddg-recomp-linux-x64.tar.gz (folder ddg-recomp/).
# Needs clang, SDL3 >= 3.2 (pkg-config sdl3) and a Vulkan driver at run time.
# RENDERER=gl builds the OpenGL backend instead (GraphicGL.cpp; desktop GL 3.3 / GLES 3).
# Run: build/linux-x64/perfect_recomp [<folder with perfect.exe and cddata>] [--play]
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
cd "$recomp"
out=build/linux-x64
renderer=${RENDERER:-gpu}
rm -f $out/perfect_recomp
make -j"$(nproc)" RENDERER="$renderer" OBJ=${OBJDIR:-build/obj-linux-x64-$renderer} TARGET=$out/perfect_recomp \
    LDEXTRA="${LDFLAGS:-} -Wl,-rpath,\\\$\$ORIGIN/libs"
mkdir -p $out/libs
sdl=$(pkg-config --variable=libdir sdl3)/libSDL3.so.0
cp -L "$sdl" $out/libs/
cp "$here/README-desktop.txt" $out/README.txt
tar -C build -czf build/ddg-recomp-linux-x64.tar.gz --transform 's,^linux-x64,ddg-recomp,' linux-x64
echo "built $out (renderer $renderer), package build/ddg-recomp-linux-x64.tar.gz"
