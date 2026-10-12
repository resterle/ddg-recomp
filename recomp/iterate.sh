#!/bin/sh
# Run the recompiled game; while it stops at an unknown call target (recorded in
# build/discovered.txt), regenerate, rebuild and run again. Stops at any other trap.
OPT_FLAGS="${OPT_FLAGS:--O1 -DRT_CHECK_STACK}"
for i in $(seq 1 ${1:-20}); do
    perl -e 'alarm 40; exec @ARGV' ./perfect_recomp --play > build/iter.log 2>&1
    trap_line=$(grep -a "recomp trap" build/iter.log | tail -1)
    echo "run $i: ${trap_line:-no trap (exit or timeout)}"
    case "$trap_line" in
        *"unknown address"*)
            .venv/bin/python build_game.py > build/gen.log 2>&1 || { cat build/gen.log; exit 1; }
            make -j10 OPT="$OPT_FLAGS" 2>&1 | grep -E " error|Undefined" && exit 1 ;;
        *) break ;;
    esac
done
grep -av "^stub\|linkd\|WindowTab\|Connection\]" build/iter.log | tail -12
