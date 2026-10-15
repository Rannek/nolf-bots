#!/usr/bin/env bash
# Build the player download: NOLF-Bots-<version>.zip
#   linux/make_release.sh <version> [out-dir]      (run ninja first)
# Packs the deployed server DLL (BotsRez/Object.lto) into Custom/BOTS.REZ
# with LithRez, adds what the bots know about Hydro (map scan, learned trouble
# spots, trained fighting values), release/README.txt and the Windows
# launcher (release/Play NOLF with Bots.bat).  The zip unpacks
# straight into a NOLF folder.  bots.cfg isn't in it: the game writes the
# defaults the first time someone hosts, so an update never overwrites a
# player's own settings.
set -euo pipefail

VERSION=${1:?usage: make_release.sh <version> [out-dir]}
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "${2:-$SRC/release/out}" && cd "${2:-$SRC/release/out}" && pwd)
GAME=${NOLF_PREFIX:-$HOME/Games/nolf-1}/drive_c/Games/NOLF
ZIP="$OUT/NOLF-Bots-$VERSION.zip"

export WINEPREFIX="${NOLF_PREFIX:-$HOME/Games/nolf-1}"
export PROTONPATH="$HOME/.local/share/Steam/compatibilitytools.d/GE-Proton11-7-x86_64"
export GAMEID=umu-default
export STORE=none

[ -f "$GAME/BotsRez/Object.lto" ] || { echo "No $GAME/BotsRez/Object.lto: build first" >&2; exit 1; }
for f in 07HYDRO_AM.bwm 07HYDRO_AM.nav genome.txt; do
  [ -f "$GAME/Bots/$f" ] || { echo "Missing $GAME/Bots/$f" >&2; exit 1; }
done

# Staged inside the source tree (on the prefix's drives) for LithRez
STAGE="$SRC/release/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/rez" "$STAGE/pkg/Custom" "$STAGE/pkg/Bots"
cp "$GAME/BotsRez/Object.lto" "$STAGE/rez/"

(cd "$STAGE" && timeout 180 umu-run "$SRC/TOOLS/lithrez.exe" c 'pkg\Custom\BOTS.REZ' rez >/dev/null 2>&1) || true
[ -s "$STAGE/pkg/Custom/BOTS.REZ" ] || { echo "LithRez didn't make BOTS.REZ" >&2; exit 1; }

for f in 07HYDRO_AM.bwm 07HYDRO_AM.nav genome.txt; do
  cp "$GAME/Bots/$f" "$STAGE/pkg/Bots/"
done
sed -e "s/@VERSION@/$VERSION/" -e 's/$/\r/' "$SRC/release/README.txt" > "$STAGE/pkg/README-NOLF-Bots.txt"
sed -e 's/$/\r/' "$SRC/release/Play NOLF with Bots.bat" > "$STAGE/pkg/Play NOLF with Bots.bat"

rm -f "$ZIP"
bsdtar --format zip -cf "$ZIP" -C "$STAGE/pkg" README-NOLF-Bots.txt "Play NOLF with Bots.bat" Custom Bots
rm -rf "$STAGE"
echo "$ZIP"
bsdtar -tvf "$ZIP"
