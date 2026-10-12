#!/bin/zsh
# A/B benchmark: alternates runs of two binaries (default build/perfect_A and build/perfect_B)
# under the same conditions; prints game/render ms per tick at 80 and 90 s tick time.
# usage: tools/ab.sh [binA] [binB] [rounds]
cd "${0:A:h}/.."
A=${1:-build/perfect_A}; B=${2:-build/perfect_B}; N=${3:-3}
. tools/drive_keys.sh
mkdir -p build/bench-prefs && cp tools/bench-prefs/graphics.ini build/bench-prefs/
export RECOMP_PREF_DIR="$PWD/build/bench-prefs"
[ -z "$DENGO_FINAL_DIR" ] && export DENGO_FINAL_DIR=$(sed -n 's/^gamedir=//p' "$HOME/Library/Application Support/TAITO/DengoFinal/game.ini")
for i in $(seq 1 $N); do
  for bin in $A $B; do
    RECOMP_TRACE_TICKS=1 RECOMP_KEYS="$K" perl -e 'alarm 95; exec @ARGV' ./$bin --play > build/ab.log 2>&1
    echo "$bin: $(grep -a '^t=\(80\|90\)s' build/ab.log | sed -E 's/.*\| game ([0-9.]+) render ([0-9.]+) ms\/tick \| process cpu ([0-9.]+) ms\/tick ([0-9.]+) Minstr\/tick \| game ([0-9.]+) render ([0-9.]+) Minstr.*/\1+\2 ms, Minstr game \5 render \6 (process \4)/' | tr '\n' ' ')"
  done
done
