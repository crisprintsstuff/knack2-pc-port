"""Intersect measured intervals; report overlap, never additive causal claims."""
import argparse
import collections
import csv
import json
from pathlib import Path
import re

def union_ns(intervals):
    end = 0
    total = 0
    for a, b in sorted(intervals):
        if b > end:
            total += b - max(a, end)
            end = b
    return total

def analyze(folder, loading_seconds):
    text = (folder / 'stderr.log').read_text(encoding='utf-8', errors='replace')
    pattern = r'SHADER_SPIKE event=(\w+) start=(\d+) end=(\d+) id=(\d+) detail=(\d+) reason=(\w+)'
    events = [dict(kind=k, start=int(s), end=int(e), id=int(i), detail=int(d), reason=r)
              for k,s,e,i,d,r in re.findall(pattern, text)]
    frames = sorted((e for e in events if e['kind']=='frame'), key=lambda e:e['id'])
    csv_rows = list(csv.DictReader((folder/'frames.csv').open()))
    if not frames: raise ValueError('No diagnostic frame events')
    origin = frames[0]['end']
    first_game = next(i for i in range(len(csv_rows)-30)
                      if all(float(r['draws'])>1000 for r in csv_rows[i:i+30]))
    # Explicit conservative fixed exclusion, with sensitivity at 5/10/15 s below.
    game_origin = frames[first_game+1]['end']
    threshold = game_origin + int(loading_seconds * 1e9)
    windows = [e for e in frames if e['start']>=threshold]
    blocking = [e for e in events if e['kind']=='wait' or
                (e['kind']=='translate' and e['reason']=='cp') or
                (e['kind']=='spec' and e['reason']=='running_awaited')]
    pipeline_kinds={e['id']: e['kind'] for e in events if e['kind'] in ('graphics','compute')}
    details=[]
    for f in windows:
        ms=(f['end']-f['start'])/1e6
        if ms<=33: continue
        overlap=lambda e: (max(f['start'],e['start']), min(f['end'],e['end']))
        hits=[e for e in blocking if e['start']<f['end'] and e['end']>f['start']]
        waits=[e for e in hits if e['kind']=='wait']
        cpu=[e for e in hits if e['kind']!='wait']
        news=[e for e in events if f['start']<e['end']<=f['end'] and e['kind'] in ('graphics','compute','spec')]
        worst=max(waits,key=lambda e:overlap(e)[1]-overlap(e)[0],default={})
        row=csv_rows[f['id']-2] if 2<=f['id']<=len(csv_rows)+1 else {}
        details.append(dict(frame=f['id'], ms=ms,
            wait_ms=union_ns([overlap(e) for e in waits])/1e6,
            cp_ms=union_ns([overlap(e) for e in cpu])/1e6,
            union_ms=union_ns([overlap(e) for e in hits])/1e6,
            worst_pipeline=worst.get('id'), worst_state=worst.get('reason'),
            worst_kind=pipeline_kinds.get(worst.get('id')),
            new_graphics=sum(e['kind']=='graphics' for e in news),
            new_compute=sum(e['kind']=='compute' for e in news),
            new_shaders=sum(e['kind']=='spec' for e in news),
            known_stages=sum(e['reason']=='known_stages' for e in news),
            spec_reasons=dict(collections.Counter(e['reason'] for e in news if e['kind']=='spec')),
            readback_ms=float(row.get('readback_stall_ms',0)),
            cp_work_ms=float(row.get('command_processor_ms',0)),
            image_refresh_ms=float(row.get('image_refresh_ms',0))))
    total=sum(d['ms'] for d in details)
    covered=sum(d['union_ms'] for d in details)
    severe=[d for d in details if d['ms']>50]
    result=dict(run=str(folder), loading_excluded_s=loading_seconds,
        frames=len(windows), over33=len(details), over50=sum(d['ms']>50 for d in details),
        over100=sum(d['ms']>100 for d in details), max_ms=max((d['ms'] for d in details),default=0),
        spike_ms=total, shader_overlap_ms=covered, shader_overlap_percent=100*covered/total if total else 0,
        spike_wait_ms=sum(d['wait_ms'] for d in details),
        spike_cp_ms=sum(d['cp_ms'] for d in details),
        spike_other_ms=total-covered,
        severe_shader_overlap_percent=(100*sum(d['union_ms'] for d in severe)/sum(d['ms'] for d in severe)) if severe else 0,
        excess33_shader_upper_bound_percent=(100*sum(min(d['union_ms'],d['ms']-33) for d in details)/sum(d['ms']-33 for d in details)) if details else 0,
        recorder_wait_all_ms=sum(e['end']-e['start'] for e in events if e['kind']=='wait')/1e6,
        cp_translate_all_ms=sum(e['end']-e['start'] for e in events if e['kind']=='translate' and e['reason']=='cp')/1e6,
        pipeline_skips=sum(float(r['pipeline_skips']) for r in csv_rows),
        new_graphics=sum(e['kind']=='graphics' for e in events),
        new_compute=sum(e['kind']=='compute' for e in events),
        spec_reasons=dict(collections.Counter(e['reason'] for e in events if e['kind']=='spec')),
        first_game_csv_index=first_game, first_city_after_first_present_s=(game_origin-origin)/1e9,
        note='Temporal overlap is not proof of causality. Concurrent counters overlap. Loading exclusion is explicit.',
        spikes=details)
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('folder',type=Path); a=p.parse_args()
    reports=[analyze(a.folder,s) for s in (0,2,5)]
    (a.folder/'shader-spikes-analysis.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
    print(json.dumps([{k:v for k,v in r.items() if k!='spikes'} for r in reports],indent=2))
