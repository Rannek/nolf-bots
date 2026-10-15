#!/usr/bin/env bash
# Head-to-head campaign: is FEATURE worth having?
#
#   selfplay_campaign.sh [-m game-min] [-s speed] FEATURE REPS MAP [MAP...]
#
# Freezes the current bot build, then REPS times runs selfplay.sh -x FEATURE
# over the maps, two maps per batch (= 4 servers: each map with sides
# swapped), and prints one combined verdict over every match.  Nothing learned
# is saved (evaluation mode), so every match starts from the same maps.
set -u
MIN=8; SPEED=3
while getopts "m:s:" o; do
  case $o in m) MIN=$OPTARG ;; s) SPEED=$OPTARG ;; *) exit 1 ;; esac
done
shift $((OPTIND - 1))
[ $# -ge 3 ] || { echo "usage: $0 [-m min] [-s speed] FEATURE REPS MAP..." >&2; exit 1; }
FEAT=$1; REPS=$2; shift 2

GAME=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/Games/NOLF
TOOLS=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/NOLFSource/linux
export WINEPREFIX="${NOLF_PREFIX:-$HOME/Games/nolf-1}"
export PROTONPATH="$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64"
export GAMEID=umu-default STORE=none

STAMP=$(date +%Y%m%d-%H%M%S)
cd "$GAME" || exit 1
mkdir -p Bots/selfplay
FROZEN="Bots/selfplay/campaign-${STAMP}.rez"
timeout 120 umu-run ${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/NOLFSource/TOOLS/lithrez.exe c "Bots\\selfplay\\campaign-${STAMP}.rez" BotsRez >/dev/null 2>&1
[ -f "$FROZEN" ] || { echo "couldn't freeze the build" >&2; exit 3; }
export FROZEN_REZ="$GAME/$FROZEN"
echo "Campaign '$FEAT': $REPS x [$*], frozen build $FROZEN"

LOGS=()
MAPS=("$@")
for r in $(seq 1 "$REPS"); do
  for ((i = 0; i < ${#MAPS[@]}; i += 2)); do
    CHUNK=("${MAPS[@]:i:2}")
    OUT=$("$TOOLS/selfplay.sh" -x "$FEAT" -m "$MIN" -s "$SPEED" "${CHUNK[@]}" 2>&1)
    echo "$OUT" | grep -E "^Bots/selfplay/.*team[12] has the feature"
    while read -r L; do LOGS+=("$L"); done < <(echo "$OUT" | grep -oE "Bots/selfplay/[^ ]+-[0-9]+\.txt" | sort -u)
  done
done

rm -f "$FROZEN"
python3 -I "$TOOLS/selfplay_eval.py" --ab "${LOGS[@]}"
