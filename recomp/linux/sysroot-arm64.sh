#!/bin/sh
# Exports the aarch64 sysroot (headers, libraries, SDL3 built for the handhelds) from the
# ddg-recomp-arm64 image (Dockerfile; built once under qemu) into recomp/build/sysroot-arm64.
# Used by build-cross.sh and builder/make-kit.py. Does nothing when it exists already.
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
sysroot=$recomp/build/sysroot-arm64
[ -d "$sysroot/opt/sdl3" ] && exit 0
docker image inspect ddg-recomp-arm64 >/dev/null 2>&1 || docker build --platform linux/arm64 -t ddg-recomp-arm64 "$here"
rm -rf "${sysroot:?}" && mkdir -p "$sysroot"
cid=$(docker create --platform linux/arm64 ddg-recomp-arm64 true)
# what compiling and linking need: headers, libraries and the SDL install
docker export "$cid" | tar -x -C "$sysroot" usr/include usr/lib lib opt/sdl3
docker rm "$cid" >/dev/null
# absolute symlinks (libgcc_s.so -> /lib/...) would point into the host: make them relative
find "$sysroot" -type l -lname '/*' | while read -r l; do
    t=$(readlink "$l"); d=$(dirname "${l#$sysroot}")
    ln -sfn "$(realpath -sm --relative-to="$d" "$t")" "$l"
done
