#!/bin/bash
# Usage: ISS_EXE=out/experimental/<dir>/iss-probe.exe bash scripts/cold_cache_ab.sh <label> <lineage_dir> [ISS_FLAG=1 ...]
# Runs the best config for 90 s on the given cache lineage and prints compile/stall/spike totals.
cd "/c/Games/Infamous Second Son PC Port/Infamous Second Son Optmize/pc-port"
label=$1; lineage=$2; shift 2
S=""; for v in ISS_CLAMP_CACHE_FIRST=1 ISS_STAGE_MEMO=1 ISS_FAST_DRAW=1 ISS_FAST_REBIND=1 ISS_FAST_LEAN=1 ISS_FAST_SWITCH=1 ISS_SUBMIT_EVERY=1000 BB_VBLANK_HZ=240 ISS_COMPUTE_FAIRNESS=1 ISS_CP_CPU=14 ISS_STAGE_MEMO_LOOSE=1 ISS_STAGE_MEMO_TEXEL=1 "$@"; do S="$S --set $v"; done
log=out/experimental/cold-$label.log
python scripts/run_controlled_benchmark.py --exe ${ISS_EXE:-out/experimental/20261014-census/iss-probe.exe} --label $label --seconds 90 $S --cache-lineage $lineage > $log 2>&1
d=$(grep -o '"run_dir": "[^"]*"' $log | tail -1 | sed 's/.*: "//;s/"$//;s/\\/\//g;s#//#/#g')
python - "$d" "$label" <<'PY'
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1]+'/frames.csv')))
f=lambda r,k: float(r[k] or 0)
tot=lambda k: sum(f(r,k) for r in rows)
pm=[f(r,'present_ms') for r in rows if f(r,'draws')>100]
print(sys.argv[2],'frames',len(rows),'stall_ms',round(tot('pipeline_stall_ms')),'compile_ms',round(tot('pipeline_compile_ms')),'shader_ms',round(tot('shader_compile_ms')),'pipelines',int(tot('pipelines_created')),'frames>100ms',sum(1 for m in pm if m>100),'frames>50ms',sum(1 for m in pm if m>50),'max',round(max(pm)),'mean_ms',round(sum(pm)/len(pm),1))
PY
grep -a -o "GFX_TIER[^\]*" "$d/stderr.log" | tail -1
