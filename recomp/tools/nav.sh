#!/bin/zsh
# Scripted run that rebuilds on "unknown address" traps (like iterate.sh) and converts the
# screenshots to build/shots/shot<N>.png.
# usage: tools/nav.sh <RECOMP_KEYS> <RECOMP_SHOT_AFTER> <timeout seconds>
# example: . tools/drive_keys.sh; tools/nav.sh "$K" "70,80" 85
cd "${0:A:h}/.."
mkdir -p build/shots
for n in 1 2 3 4 5 6 7 8; do
  rm -f snapshot/*.bmp(N)
  RECOMP_KEYS="$1" RECOMP_SHOT_AFTER="$2" perl -e "alarm $3; exec @ARGV" ./perfect_recomp --play > build/run.log 2>&1
  if grep -aq "unknown address" build/run.log; then
    grep -a "recomp trap" build/run.log
    .venv/bin/python build_game.py > /dev/null 2>&1 && make -j10 OPT="-O1 -DRT_CHECK_STACK" 2>&1 | grep -E " error"
  else
    break
  fi
done
grep -a "recomp trap\|Fatal\|fault" -A3 build/run.log | head -8
rm -f build/shots/*.png(N)
i=0; for f in snapshot/*.bmp(N); do i=$((i+1)); sips -s format png "$f" --out build/shots/shot$i.png >/dev/null; done
ls build/shots/*.png(N)
