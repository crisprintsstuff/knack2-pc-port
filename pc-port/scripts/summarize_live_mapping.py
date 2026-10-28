"""Join read-only shader snapshots, cached variants and diagnostic GPU samples."""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parents[1]
MAPPING = ROOT / 'evidence/mapping'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('run', type=Path)
    args = ap.parse_args()
    associations = defaultdict(set)
    anonymous = defaultdict(set)
    snapshots, errors = [], []
    for path in sorted(MAPPING.glob('live-shaders-06-*.json')):
        data = json.loads(path.read_text(encoding='utf-8'))
        if Path(data['run']).resolve() != args.run.resolve():
            continue
        programs = {}
        for origin in ('records', 'particle_programs'):
            for row in data.get(origin, []):
                if row.get('error'):
                    errors.append({'snapshot': path.name, 'origin': origin, **row})
                if 'binary_info_hash' not in row:
                    continue
                info = bytes.fromhex(row['binary_info_hex'])
                if info[:7] != b'OrbShdr' or struct.unpack_from('<Q', info, 16)[0] != int(row['binary_info_hash'], 16):
                    raise ValueError('Snapshot header/hash mismatch')
                if struct.unpack_from('<I', info, 8)[0] >> 8 != row['code_length']:
                    raise ValueError('Snapshot header/length mismatch')
                if any(row.get(k) is False for k in ('stable', 'resource_stable', 'metadata_stable', 'program_backlink_valid', 'program_backlink_stable')):
                    errors.append({'snapshot': path.name, 'origin': origin, 'reason': 'unstable or backlink mismatch', **row})
                    continue
                if origin == 'records' and not data['registry_stable']:
                    continue
                key = (row['asset_id'], row['binary_info_hash'], 'x'.join(map(str, row['local_size'])))
                if row['asset_id'] == '0xffffffffffffffff':
                    anonymous[(row['program'], row['resource'], row['binary_info_hash'], key[2])].add(path.name)
                else:
                    associations[key].add(path.name + ':' + origin)
                if row.get('program') != '0x0':
                    programs[row['program']] = row['binary_info_hash']
        listings = []
        for listing in data.get('particle_lists', []):
            hashes = {field: dict(Counter(programs.get(r.get(field), 'unresolved')
                        for r in listing['objects'] if r.get('stable'))) for field in ('program_a', 'program_b')}
            addresses = [int(r['feedback'], 16) for r in listing['objects'] if r.get('stable') and int(r.get('feedback', '0'), 16)]
            listings.append({'manager_offset': listing['manager_offset'], 'objects': len(listing['objects']),
                             'head_stable': listing['head_stable'], 'stable_objects': sum(r.get('stable', False) for r in listing['objects']),
                             'program_hash_object_counts': hashes, 'feedback_records': len(addresses),
                             'feedback_pages_4k': len({a // 4096 for a in addresses}),
                             'records_crossing_4k_page': sum(a // 4096 != (a + 47) // 4096 for a in addresses)})
        snapshots.append({'file': path.name, 'time': data['time'], 'registry': data['registry'],
                          'decoded_registry_headers': data['decoded_headers'], 'particle_lists': listings})
    cached = defaultdict(list)
    for path in (ROOT / 'out/user/gpu-14/CUSA00004').glob('*.spv'):
        match = re.fullmatch(r'(0x[0-9a-fA-F]+)_(\d+)\.spv', path.name)
        if match:
            cached[f'0x{int(match[1], 16):016x}'].append(path.name)
    rows = [dict(asset_id=a, binary_info_hash=h, local_size=s,
                 cached_variants=';'.join(sorted(cached[h])), observations=';'.join(sorted(obs)))
            for (a, h, s), obs in sorted(associations.items())]
    if not rows:
        raise ValueError('No accepted live associations')
    with (MAPPING / 'live-asset-shader-links.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    anonymous_rows = [dict(program=p, resource=r, binary_info_hash=h, local_size=s,
                           asset_id_status='sentinel -1; unresolved asset identity',
                           observations=';'.join(sorted(obs)))
                      for (p, r, h, s), obs in sorted(anonymous.items())]
    if anonymous_rows:
        with (MAPPING / 'live-particle-program-links.csv').open('w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=list(anonymous_rows[0]))
            writer.writeheader()
            writer.writerows(anonymous_rows)
    measured = defaultdict(list)
    pattern = re.compile(r'Perf GPU compute shader ([0-9a-f]+): ([\d.]+) ms/s, (\d+) dispatches/s, ([\d.]+) us avg')
    with (args.run / 'stderr.log').open(encoding='utf-8', errors='replace') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                h, ms, count, us = match.groups()
                measured[f'0x{int(h, 16):016x}'].append({'ms_per_reported_second': float(ms),
                                                       'dispatches_per_reported_second': int(count), 'us_avg': float(us)})
    report = {'run': str(args.run.resolve()), 'association_count': len(rows),
              'distinct_asset_ids': len({r['asset_id'] for r in rows}),
              'distinct_shader_hashes': len({r['binary_info_hash'] for r in rows}),
              'links_with_cached_variants': sum(bool(r['cached_variants']) for r in rows),
              'anonymous_particle_program_links': len(anonymous_rows),
              'anonymous_particle_shader_hashes': len({r['binary_info_hash'] for r in anonymous_rows}),
              'total_shader_hashes': len({r['binary_info_hash'] for r in rows + anonymous_rows}),
              'snapshots': snapshots, 'errors_or_unstable': errors,
              'gpu_profile': {h: {'samples': len(v), 'last': v[-1],
                                  'linked_assets': [r['asset_id'] for r in rows if r['binary_info_hash'] == h],
                                  'anonymous_programs': [r['program'] for r in anonymous_rows if r['binary_info_hash'] == h]}
                              for h, v in sorted(measured.items())},
              'limitations': ['Snapshot object counts are not per-frame dispatch counts.',
                             'GPU log lists selected hot shaders, not all shaders per frame.',
                             'Resource membership and matching hashes do not prove a dispatch dependency.',
                             'Captures and GPU/deep profiling add overhead; this run is not an A/B benchmark.',
                             'Double reads cannot exclude ABA or mutations between separately read objects.']}
    (MAPPING / 'live-mapping-summary-06.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if isinstance(v, int)}))


if __name__ == '__main__':
    main()
