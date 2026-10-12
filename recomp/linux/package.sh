#!/bin/sh
# Assembles the SD card folder from build/arm64 (see build.sh):
#   build/arm64/ports/Densha de GO! FINAL.sh
#   build/arm64/ports/ddg-recomp/{perfect_recomp, libs/, conf/graphics.ini, gamedata/, README.txt}
# plus build/ddg-recomp-arm64.zip with the same contents.
set -e
here=$(cd "$(dirname "$0")" && pwd)
b=$(dirname "$here")/build/arm64
out=$b/ports
rm -rf "$out"
mkdir -p "$out/ddg-recomp/libs" "$out/ddg-recomp/conf" "$out/ddg-recomp/gamedata"
cp "$b/perfect_recomp" "$out/ddg-recomp/"
cp "$b/libs/libSDL3.so.0" "$out/ddg-recomp/libs/"
cp "$here/ddg-recomp.sh" "$out/Densha de GO! FINAL.sh"
cp "$here/graphics.ini" "$out/ddg-recomp/conf/"
cp "$here/README.txt" "$out/ddg-recomp/"
touch "$out/ddg-recomp/gamedata/PUT_cddata_AND_perfect.exe_HERE"
chmod +x "$out/ddg-recomp/perfect_recomp" "$out/Densha de GO! FINAL.sh"
(cd "$out" && rm -f ../../ddg-recomp-arm64.zip && zip -qr ../../ddg-recomp-arm64.zip .)
echo "package: $out (zip: $(dirname "$b")/ddg-recomp-arm64.zip)"
