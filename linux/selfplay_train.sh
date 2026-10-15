#!/usr/bin/env bash
# Continuous self-play training on one map (default: Hydro).
#
#   selfplay_train.sh [MAP]          (env MIN=15 SPEED=4 to change)
#
# Loops until NOLF/Bots/selfplay/STOP exists.  Each cycle, at once:
#   - one TRAINING match that saves what the bots learn into Bots/<map>.nav
#     (new links, killing zones, links that don't work), and
#   - a head-to-head pair (sides swapped, nothing saved) testing the next
#     tactic from NOLF/Bots/selfplay/queue.txt (one per line, cycled).
# Every cycle uses the newest bot build.  Waits while the game itself runs.
#   Report:      NOLF/Bots/selfplay/<map>-report.txt
#   A/B tally:   python3 linux/selfplay_eval.py --abfile NOLF/Bots/selfplay/<map>-ab.txt
set -u
MAP=${1:-07HYDRO_AM}
MIN=${MIN:-15}
SPEED=${SPEED:-4}
GAME=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/Games/NOLF
TOOLS=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/NOLFSource/linux
DIR=$GAME/Bots/selfplay
mkdir -p "$DIR"
QUEUE=$DIR/queue.txt
[ -f "$QUEUE" ] || printf "waves\ndiscipline\nfocus\ndanger\ncallouts\nrecall\n" > "$QUEUE"
REPORT=$DIR/${MAP}-report.txt
ABFILE=$DIR/${MAP}-ab.txt
rm -f "$DIR/STOP"

i=0
while [ ! -f "$DIR/STOP" ]; do
  # Don't compete with a real game (or another self-play run)
  if pgrep -f '[l]ithtech\.exe|[N]OLF\.exe|[N]olfServ\.exe' >/dev/null; then
    sleep 60
    continue
  fi

  mapfile -t FEATS < <(grep -v '^[[:space:]]*$' "$QUEUE")
  FEAT=${FEATS[$(( i % ${#FEATS[@]} ))]}
  i=$((i + 1))
  echo "=== cycle $i $(date '+%m-%d %H:%M') testing '$FEAT'" >> "$REPORT"

  OUT=$("$TOOLS/selfplay.sh" -t -x "$FEAT" -m "$MIN" -s "$SPEED" "$MAP" 2>&1)
  RC=$?
  echo "$OUT" | grep -E "^Starting|giving up|has the feature|HEAD-TO-HEAD|^score|^NAV|progress|closest|why:|out of ammo" >> "$REPORT"
  echo "$OUT" | grep -E "has the feature" | sed "s/^/AB $FEAT /" >> "$ABFILE"
  [ $RC -ne 0 ] && { echo "(run failed: $RC)" >> "$REPORT"; sleep 30; }
done
echo "=== stopped $(date '+%m-%d %H:%M')" >> "$REPORT"
