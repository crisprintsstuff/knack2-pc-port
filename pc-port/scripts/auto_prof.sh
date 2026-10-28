#!/bin/sh
# Scripted run (auto_porte.py) with a timestamped sample of the command processor and recorder threads:
# sh scripts/auto_prof.sh <label> [PROF_AT=60] [PROF_S=60] [BUILD=dir] -- [auto_porte args]
# -> out/experimental/20261016-native-passes-v1/auto/<label>-{prof.json,prof.txt,samples.tsv}
ROOT="/c/Games/Infamous Second Son PC Port/Infamous Second Son Optmize/pc-port"
E="$ROOT/out/experimental/20261016-native-passes-v1"
label=$1; shift
cd "$ROOT"
B=${BUILD:-$E/base-20261018-porte-x-save}
python scripts/auto_porte.py "$label" --build "$B" --set ISS_TIMELINE=1 --set ISS_TIMELINE_HITCH_MS=${HITCH_MS:-30} "$@" \
  > "$E/auto/$label-run.txt" 2>&1 &
python -c "import time; time.sleep(${PROF_AT:-60})"
pid=$(tasklist //FI "IMAGENAME eq iss-probe.exe" //FO CSV //NH | head -1 | cut -d, -f2 | tr -d '"')
python scripts/profile_threads.py --pid "$pid" --seconds ${PROF_S:-60} --hz ${PROF_HZ:-1000} --threads ${PROF_THREADS:-2} \
  ${PROF_NAMES:+--thread-name "$PROF_NAMES"} --map "$B/iss-gpu.map" \
  --out "$E/auto/$label-prof.json" --timeline "$E/auto/$label-samples.tsv" > "$E/auto/$label-prof.txt" 2>&1
wait
tail -25 "$E/auto/$label-run.txt"
