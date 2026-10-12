#!/bin/bash
# ddg-recomp (Densha de GO! FINAL, recompiled) - launch script for ArkOS / PortMaster-style firmwares.
# Uses PortMaster's control.txt when installed (controller mapping, sudo), works without it.

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster/" ]; then
  controlfolder="${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi
if [ -f "$controlfolder/control.txt" ]; then
  source "$controlfolder/control.txt"
  get_controls
fi

GAMEDIR="$(cd "$(dirname "$0")" && pwd)/ddg-recomp"
# Installs from before the rename kept everything in ports/densha: move the game data, settings
# and saves over once (same card, so these are renames, not copies). The player's own settings
# replace the package defaults.
OLDDIR="$(dirname "$GAMEDIR")/densha"
if [ -d "$OLDDIR/gamedata/cddata" ] && [ ! -d "$GAMEDIR/gamedata/cddata" ]; then
  mkdir -p "$GAMEDIR/gamedata" "$GAMEDIR/conf"
  for f in "$OLDDIR"/gamedata/*; do
    [ -e "$GAMEDIR/gamedata/$(basename "$f")" ] || mv "$f" "$GAMEDIR/gamedata/"
  done
  for f in "$OLDDIR"/conf/*; do
    rm -rf "$GAMEDIR/conf/$(basename "$f")"
    mv "$f" "$GAMEDIR/conf/"
  done
fi
cd "$GAMEDIR" || exit 1
exec > "$GAMEDIR/log.txt" 2>&1
echo "starting $(date)"
uname -a
# graphics driver libraries on this firmware (for bug reports)
ls -l /usr/lib/aarch64-linux-gnu /usr/lib 2>/dev/null | grep -iE "egl|gles|mali|gbm|libdrm" 
free -m

# full speed for CPU and GPU while playing (restored on exit)
gov=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
gpugov=$(ls /sys/class/devfreq/*gpu*/governor 2>/dev/null | head -1)
oldgov=$(cat "$gov" 2>/dev/null)
oldgpugov=$(cat "$gpugov" 2>/dev/null)
[ -n "$oldgov" ] && echo performance | $ESUDO tee "$gov" >/dev/null
[ -n "$oldgpugov" ] && echo performance | $ESUDO tee "$gpugov" >/dev/null

export LD_LIBRARY_PATH="$GAMEDIR/libs:$LD_LIBRARY_PATH"
export DENGO_FINAL_DIR="$GAMEDIR/gamedata"
export RECOMP_PREF_DIR="$GAMEDIR/conf"
export RECOMP_TRACE_TICKS=1      # timing report every 10 s in log.txt
export RECOMP_TRACE_SPIKES=40    # break down game ticks / GL jobs that take longer than 40 ms
export SDL_GAMECONTROLLERCONFIG="${SDL_GAMECONTROLLERCONFIG:-$(cat "$GAMEDIR/conf/gamecontrollerdb.txt" 2>/dev/null)}"

chmod +x "$GAMEDIR/perfect_recomp" 2>/dev/null
"$GAMEDIR/perfect_recomp" --play
code=$?
echo "exit code $code"
if [ "$code" -ne 0 ]; then
  free -m
  # out-of-memory kills and driver errors end up in the kernel log
  (dmesg 2>/dev/null || $ESUDO dmesg 2>/dev/null) | tail -n 30
fi

[ -n "$oldgov" ] && echo "$oldgov" | $ESUDO tee "$gov" >/dev/null
[ -n "$oldgpugov" ] && echo "$oldgpugov" | $ESUDO tee "$gpugov" >/dev/null
[ -n "$ESUDO" ] && $ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" > /dev/tty0 2>/dev/null
