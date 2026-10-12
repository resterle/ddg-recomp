#!/bin/zsh
# Benchmark: scripted run into the Yamanote Line driving scene, prints game/render ms per tick for
# the 10 s windows ending at 70, 80 and 90 s tick time (driving, standing at the platform).
# usage: tools/bench.sh   (run from anywhere; needs DENGO_FINAL_DIR or a saved game folder)
cd "${0:A:h}/.."
. tools/drive_keys.sh
# fixed settings (640x480, draw distance 0.6) independent of the player's graphics.ini;
# saves go to a scratch folder
mkdir -p build/bench-prefs && cp tools/bench-prefs/graphics.ini build/bench-prefs/
export RECOMP_PREF_DIR="$PWD/build/bench-prefs"
if [ -z "$DENGO_FINAL_DIR" ]; then       # the player's saved game folder
  DENGO_FINAL_DIR=$(sed -n 's/^gamedir=//p' "$HOME/Library/Application Support/TAITO/DengoFinal/game.ini" 2>/dev/null)
  export DENGO_FINAL_DIR
fi
RECOMP_TRACE_TICKS=1 RECOMP_KEYS="$K" perl -e 'alarm 95; exec @ARGV' ./perfect_recomp --play > build/bench.log 2>&1
grep -a "^t=\(70\|80\|90\)s" build/bench.log | sed -E 's/.*\| game ([0-9.]+) render ([0-9.]+) ms\/tick.*work\/tick avg ([0-9.]+).*/game \1 render \2 total \3 ms/'
