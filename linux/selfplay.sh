#!/usr/bin/env bash
# Bot-vs-bot self-play on the NOLF dedicated server (NolfServ.exe -nowiz),
# faster than real time and several maps at once.  Same Proton setup as
# start_bots.sh (never /usr/bin/wine).
#
#   selfplay.sh [-m game-minutes] [-s speed] [-b bots] map [map...]
#     -m  game minutes per match (default 6)
#     -s  speed multiplier, engine TimeScale (default 4: 6 game min ~ 90 s)
#     -b  bots per match (default 12)
#     -e  evaluation: save nothing learned, so the same map can run several
#         times at once and runs compare fairly
#     -x FEATURE  head-to-head test: every map runs twice at once (nothing
#         saved), once with only team 1 having FEATURE, once with only team 2
#         (waves, danger, callouts, discipline, focus, recall)
#     -t  with -x: also run one training match per map that DOES save what it
#         learns (so one batch trains and tests at the same time)
#     map AssaultMap world names, one server each, run in parallel
#         (e.g. 07HYDRO_AM 02Blizzard_AM); use different maps per run: each
#         server saves what it learns into Bots/<map>.nav
#
# Logs: NOLF/Bots/selfplay/<map>-<time>-<n>.txt, evaluated at the end with
# selfplay_eval.py.  autoexec.cfg, NetHost.txt and startup.txt are restored.
set -u

# Run from a private copy: bash reads scripts lazily, so editing this file
# while a run is going would otherwise make that run execute garbage.
if [ -z "${SELFPLAY_COPY:-}" ]; then
  COPY=$(mktemp /tmp/selfplay.XXXXXX.sh) && cp "$0" "$COPY" && SELFPLAY_COPY="$COPY" exec bash "$COPY" "$@"
fi
trap 'rm -f "$SELFPLAY_COPY"' EXIT

MIN=6; SPEED=4; BOTS=12; EVAL=0; AB=""; TRAIN=0
while getopts "m:s:b:ex:t" o; do
  case $o in m) MIN=$OPTARG ;; s) SPEED=$OPTARG ;; b) BOTS=$OPTARG ;; e) EVAL=1 ;; x) AB=$OPTARG ;; t) TRAIN=1 ;; *) exit 1 ;; esac
done
# Saving is decided per server (NOLFBOT_NOSAVE): evaluation / head-to-head
# matches never save, so maps stay identical and the same map can run
# several times at once; only training matches save what they learn.
shift $((OPTIND - 1))
[ $# -ge 1 ] || set -- 07HYDRO_AM

GAME=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/Games/NOLF
TOOLS=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/NOLFSource/linux

export WINEPREFIX="${NOLF_PREFIX:-$HOME/Games/nolf-1}"
export PROTONPATH="$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64"
export GAMEID=umu-default
export STORE=none
export WINEESYNC=0 WINEFSYNC=0 PROTON_NO_ESYNC=1 PROTON_NO_FSYNC=1
export WINEDLLOVERRIDES="dmband,dmcompos,dmime,dmloader,dmscript,dmstyle,dmsynth,dmusic,dmusic32,dswave=n;dsound=b"

cd "$GAME" || exit 1

if pgrep -f '[l]ithtech\.exe|[N]OLF\.exe|[N]olfServ\.exe' >/dev/null; then
  echo "NOLF or its server is already running; not starting self-play." >&2
  exit 1
fi

STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p Bots/selfplay
cp -p autoexec.cfg "Bots/selfplay/autoexec.before-${STAMP}"
[ -f NetHost.txt ] && cp -p NetHost.txt "Bots/selfplay/NetHost.before-${STAMP}"
cp -p startup.txt "Bots/selfplay/startup.before-${STAMP}"

restore() {
  pkill -f '[N]olfServ\.exe -nowiz' 2>/dev/null
  rm -f "${SELFPLAY_COPY:-}"
  cp -p "Bots/selfplay/autoexec.before-${STAMP}" autoexec.cfg
  [ -f "Bots/selfplay/NetHost.before-${STAMP}" ] && cp -p "Bots/selfplay/NetHost.before-${STAMP}" NetHost.txt
  cp -p "Bots/selfplay/startup.before-${STAMP}" startup.txt
}
trap restore EXIT

# Our Object.lto as a real .rez (the server ignores folders), rebuilt with
# Monolith's lithrez whenever the build is newer.
if [ -n "${FROZEN_REZ:-}" ]; then
  # A campaign pins one build for all its runs (development can go on)
  cp "$FROZEN_REZ" Custom/BOTS.REZ || exit 3
elif [ ! -f Custom/BOTS.REZ ] || [ BotsRez/Object.lto -nt Custom/BOTS.REZ ]; then
  rm -f Custom/BOTS.REZ
  timeout 120 umu-run ${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/NOLFSource/TOOLS/lithrez.exe c 'Custom\BOTS.REZ' BotsRez >/dev/null 2>&1
  [ -f Custom/BOTS.REZ ] || { echo "Couldn't build Custom/BOTS.REZ" >&2; exit 3; }
fi

# NolfServ -nowiz loads what startup.txt lists: add Modernizer and our bots.
echo -n "Server resources: "
python3 -I "$TOOLS/selfplay_startup.py" "Bots/selfplay/startup.before-${STAMP}" startup.txt \
  'Custom\MODERNIZER.REZ' 'Custom\BOTS.REZ' || exit 4

# -nowiz takes its options from NetHost.txt
setopt() {
  [ -f NetHost.txt ] || return
  python3 -I -c '
import sys
name, val = sys.argv[1], sys.argv[2]
lines = open("NetHost.txt", newline="").read().split("\n")
out = [(f"{name} = {val}\r" if l.startswith(name + " = ") else l) for l in lines]
open("NetHost.txt", "w", newline="").write("\n".join(out))' "$1" "$2"
}

export NOLFBOT_SELFPLAY="$BOTS"
export NOLFBOT_TIMESCALE="$SPEED"
REAL=$(python3 -c "print(max(30, int($MIN * 60 / $SPEED)))")
LOGS=()
PORT=27999

JOBS=()   # map|team1-off|team2-off|save
for MAP in "$@"; do
  if [ -n "$AB" ]; then
    [ "$TRAIN" = 1 ] && JOBS+=("$MAP|||1")
    JOBS+=("$MAP||$AB|0" "$MAP|$AB||0")
  else
    JOBS+=("$MAP|||$([ "$EVAL" = 1 ] && echo 0 || echo 1)")
  fi
done
AB_LOGS=(); PLAIN_LOGS=()

N=0
for JOB in "${JOBS[@]}"; do
  IFS='|' read -r MAP OFF1 OFF2 SAVE <<<"$JOB"
  export NOLFBOT_OFF_T1="$OFF1" NOLFBOT_OFF_T2="$OFF2"
  if [ "$SAVE" = 1 ]; then export NOLFBOT_NOSAVE=0; else export NOLFBOT_NOSAVE=1; fi
  N=$((N + 1))
  LOG_UNIX="Bots/selfplay/${MAP}-${STAMP}-${N}.txt"
  export NOLFBOT_LOG="Bots\\selfplay\\${MAP}-${STAMP}-${N}.txt"

  setopt NetGameType "1.000000f"
  setopt NetCANumLevels "1.000000f"
  setopt NetCALevel0 "\"Worlds\\Multi\\AssaultMap\\${MAP}\""
  setopt NetEndTime "60.000000f"
  setopt NetCAGameEnd "1.000000f"
  sed -i \
    -e 's/^"NetGameType" .*/"NetGameType" "1.000000"/' \
    -e "s/^\"NetCALevel0\" .*/\"NetCALevel0\" \"Worlds\\\\Multi\\\\AssaultMap\\\\${MAP}\"/" \
    -e 's/^"NetCANumLevels" .*/"NetCANumLevels" "1.000000"/' \
    autoexec.cfg

  echo "Starting $MAP on port $PORT ($BOTS bots, x$SPEED, $MIN game min ~ ${REAL}s${OFF1:+, team1 without $OFF1}${OFF2:+, team2 without $OFF2}$([ "$SAVE" = 1 ] && echo ', training: saves')) -> $LOG_UNIX"
  # PROTON_VERB=run: start inside the running Proton session instead of
  # waiting for the previous server to exit (the default waitforexitandrun)
  PROTON_VERB=run umu-run NolfServ.exe -nowiz -NetPort "$PORT" -NetSessionName "BotSelfPlay$PORT" >"Bots/selfplay/server-${MAP}-${STAMP}.out" 2>&1 &

  # Wait until this server is up (it has read its settings) before the next
  for i in $(seq 1 90); do
    [ -s "$LOG_UNIX" ] && break
    sleep 2
  done
  if [ ! -s "$LOG_UNIX" ]; then
    echo "  $MAP: no bot log after 3 minutes, giving up on it." >&2
  else
    LOGS+=("$LOG_UNIX")
    if [ -n "$OFF1$OFF2" ]; then AB_LOGS+=("$LOG_UNIX"); else PLAIN_LOGS+=("$LOG_UNIX"); fi
  fi
  PORT=$((PORT + 1))
done

[ ${#LOGS[@]} -gt 0 ] || exit 2
sleep "$REAL"
pkill -f '[N]olfServ\.exe -nowiz'
sleep 5

for L in "${PLAIN_LOGS[@]}"; do
  python3 -I "$TOOLS/selfplay_eval.py" "$L"
done
[ ${#AB_LOGS[@]} -gt 0 ] && python3 -I "$TOOLS/selfplay_eval.py" --ab "${AB_LOGS[@]}"
