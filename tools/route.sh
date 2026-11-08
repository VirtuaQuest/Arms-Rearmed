#!/bin/sh
# Scripted test routes (LARECOMP_INPUT button presses by frame), for checking the game's scenes and screenshots.
# Usage: tools/route.sh <route> [seconds] [screenshot frames, comma separated]
#   menus   fresh save: title, patch notes, save slot, difficulty -> "Save data has been created"     ~110 s
#   kree    fresh save: as menus, then the opening movie (skipped) and the first level (the Kree ship) ~170 s
#   play    fresh save: as kree, then ~6 minutes of moving, attacking, jumping and switching heroes        ~520 s
# Fresh-save routes move save/ aside first and put it back afterwards (the game autosaves).
# Frames are presented frames (30 fps); load times shift them a little between runs. Confirmation dialogs start on
# CANCEL: LEFT then A.
cd "$(dirname "$0")/.." || exit 1
route=$1 secs=$2 shots=$3
seq_presses() {  # <button> <from> <step> <to>
    for f in $(seq "$2" "$3" "$4"); do printf ',%s:%s:8' "$f" "$1"; done
}
menus="300:PLUS:8,1300:Y:8,1500:PLUS:8,1750:A:8,2100:LEFT:6,2160:A:8,2500:A:8,2600:LEFT:6,2660:A:8"
case "$route" in
menus)
    in="$menus"
    : "${secs:=110}" ;;
kree)
    in="$menus,3000:A:8,3300:PLUS:8,3400:X:8$(seq_presses A 3700 150 6000)"  # (the opening: + pauses, X skips)
    : "${secs:=170}" ;;
play)  # as kree, then ~6 minutes of play: moving in turn in eight directions, attacks, jumps, character switches
    in="$menus,3000:A:8,3300:PLUS:8,3400:X:8$(seq_presses A 3700 150 4300)"
    a=0
    for f in $(seq 4400 160 15000); do
        in="$in,$f:STICK_$a:90,$((f + 10)):Y:6,$((f + 30)):Y:6,$((f + 50)):X:12,$((f + 80)):A:6,$((f + 110)):B:6"
        [ $((f % 960)) -lt 160 ] && in="$in,$((f + 140)):RIGHT:6"  # (the next hero)
        a=$(((a + 135) % 360))
    done
    : "${secs:=520}" ;;
*)
    sed -n '2,9p' "$0"; exit 1 ;;
esac
if [ -d save.route-backup ]; then  # (never deleted here: it may be the only copy of a save)
    echo "save.route-backup exists (an interrupted run?): put it back as save/ or remove it first"; exit 1
fi
[ -d save ] && mv save save.route-backup
[ -n "$shots" ] && export LARECOMP_SHOT_AT="$shots"
LARECOMP_INPUT="$in" tools/run.sh "$secs"
rm -rf save
[ -d save.route-backup ] && mv save.route-backup save
