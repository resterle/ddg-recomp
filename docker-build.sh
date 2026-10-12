#!/bin/sh
# Builds ddg-recomp (Densha de GO! FINAL, recompiled) with Docker only: generates the game code from your
# perfect.exe and compiles it.
#
#   ./docker-build.sh [desktop|handheld] /path/to/perfect.exe
#
#   desktop   x86-64 Linux, SDL GPU on Vulkan (RENDERER=gl: OpenGL)
#             -> recomp/build/linux-x64/ and recomp/build/ddg-recomp-linux-x64.tar.gz
#   handheld  aarch64 Linux handhelds (R36H/R36S, ArkOS and similar), OpenGL ES
#             -> recomp/build/arm64/ports/ and recomp/build/ddg-recomp-arm64.zip
#
# perfect.exe must be the unprotected executable (the SafeDisc-protected one from the CD cannot be
# used); a copy patched by DDGModernizer works too. The first run builds the Docker images, which
# takes a while; the handheld images need qemu for arm64 once
# (docker run --privileged --rm tonistiigi/binfmt --install arm64).
set -e
target=desktop
case "$1" in desktop|handheld) target=$1; shift ;; esac
exe=$1
if [ -z "$exe" ] || [ ! -f "$exe" ]; then
    echo "usage: $0 [desktop|handheld] /path/to/perfect.exe" >&2
    exit 1
fi
root=$(cd "$(dirname "$0")" && pwd)
exe=$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")
image=ddg-recomp-desktop

docker image inspect $image >/dev/null 2>&1 || docker build -t $image "$root/recomp/linux/desktop"

# Runs a command in the desktop image as the calling user, with the sources at /src and the exe
# read-only at /exe/perfect.exe.
run() {
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e RENDERER="${RENDERER:-gpu}" \
        -v "$root:/src" -v "$exe:/exe/perfect.exe:ro" -w /src/recomp $image sh -ec "$1"
}

echo "== generating the game code from $exe"
run "PERFECT_EXE=/exe/perfect.exe /opt/venv/bin/python build_game.py"

case $target in
desktop)
    echo "== building the desktop version"
    run "OBJDIR=build/obj-docker-x64-\${RENDERER} linux/build-desktop.sh"
    ;;
handheld)
    echo "== building the handheld version"
    "$root/recomp/linux/build-cross.sh"
    ;;
esac
