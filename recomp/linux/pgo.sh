#!/bin/sh
# Profile-guided optimisation for the arm64 build. Builds an instrumented binary, plays the game
# for ~2 minutes of game time in a container (software GL, scripted menu input and driving, see
# tools/drive_keys.sh), merges the profile into linux/perfect.profdata and rebuilds the package
# with it. Needs the game data:
#   DENGO_FINAL_DIR=<folder with cddata and perfect.exe> recomp/linux/pgo.sh
# Re-run after larger code changes (functions without profile data are simply not optimised by it).
set -e
here=$(cd "$(dirname "$0")" && pwd)
recomp=$(dirname "$here")
root=$(dirname "$recomp")
game=${DENGO_FINAL_DIR:?set DENGO_FINAL_DIR to the game folder (cddata, perfect.exe)}
PGO_MODE=gen "$here/build.sh"
rm -rf "$recomp/build/pgo-raw" "$recomp/build/pgo-run"
mkdir -p "$recomp/build/pgo-raw" "$recomp/build/pgo-run"
sed 's/^fullscreen = .*/fullscreen = 0/' "$here/graphics.ini" > "$recomp/build/pgo-run/graphics.ini"
docker run --rm --platform linux/arm64 -v "$root:/src" -v "$game:/game:ro" -w /src/recomp/build/pgo-run \
    debian:bookworm-slim sh -c '
    apt-get update >/dev/null 2>&1
    apt-get install -y --no-install-recommends libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri libudev1 >/dev/null 2>&1
    . ../../tools/drive_keys.sh
    LLVM_PROFILE_FILE=/src/recomp/build/pgo-raw/%p.profraw RECOMP_EXIT_AFTER=130 RECOMP_KEYS="$K" \
    SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy DENGO_FINAL_DIR=/game RECOMP_PREF_DIR=$PWD \
        ../arm64-pgogen/perfect_recomp --play 2>&1 | grep -a "profile written\|exit (RECOMP\|trap"'
docker run --rm --platform linux/arm64 -v "$root:/src" -w /src/recomp ddg-recomp-arm64 \
    sh -c 'llvm-profdata-16 merge -o linux/perfect.profdata build/pgo-raw/*.profraw && ls -la linux/perfect.profdata'
# an empty profile would make every function look cold: refuse to use it
if [ "$(wc -c < "$here/perfect.profdata")" -lt 100000 ]; then
    echo "profile too small - not using it"; rm -f "$here/perfect.profdata"; exit 1
fi
"$here/build.sh"
