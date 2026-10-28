"""Resume o censo passivo da Fase 0, sem iniciar o jogo ou alterar caches."""
import argparse
import json
import re
from pathlib import Path


def summarize(folder):
    benchmark = json.loads((folder / 'controlled-benchmark.json').read_text())
    groups = {}
    coverage = {}
    for line in (folder / 'stderr.log').read_text(errors='replace').splitlines():
        if line.startswith('HYBRID_COVERAGE '):
            row = {k: float(v) for k, v in re.findall(r'(\w+)=([0-9.]+)', line)}
            coverage[(int(row['frame']), int(row['kind']))] = row
            continue
        if not line.startswith('HYBRID_FAMILY '):
            continue
        row = {k: float(v) for k, v in re.findall(r'(\w+)=([0-9.]+)', line)}
        groups.setdefault(int(row['frame']), {})[int(row['kind'])] = row
    for frame, families in groups.items():
        for kind, row in families.items():
            row.update(coverage.get((frame, kind), {}))
    # Engine submit-done observations and presentation counters are different clocks.
    # Use the last N complete observed groups, N = presented frames in the 30s tail.
    count = benchmark['tail_30s']['frames']
    tail = sorted(groups)[-count:]
    if groups and 'ns' in groups[max(groups)][0]:
        cutoff = groups[max(groups)][0]['ns'] - 30e9
        tail = [f for f in sorted(groups) if groups[f][0]['ns'] >= cutoff]
    if not tail or any(set(groups[f]) != {0, 1, 2, 3} for f in tail):
        raise ValueError('Censo ausente ou grupos incompletos')
    result = {'run_dir': str(folder), 'valid_benchmark': benchmark['valid'],
              'observed_groups': len(tail), 'requested_groups': count,
              'window_note': 'Últimos 30 s pelo relógio do CP (v2); v1 usa últimos N grupos submit-done.',
              'families': {}}
    city = [f for f in tail if sum(r['draws'] for r in groups[f].values()) > 7500]
    timing = [f for f in city if groups[f][0].get('identities', 0) == 0]
    identity = [f for f in city if groups[f][0].get('identities', 1) == 1]
    result.update(city_groups=len(city), timing_groups=len(timing), identity_groups=len(identity))
    if not timing or not identity:
        raise ValueError('Amostra insuficiente de cidade/identidades')
    result['city_span_seconds'] = ((groups[city[-1]][0].get('ns', 0) -
                                    groups[city[0]][0].get('ns', 0)) / 1e9)
    for kind in range(4):
        rows = [groups[f][kind] for f in city]
        mean = {}
        identity_fields = {'repeat_tm', 'repeat_tmg', 'materials', 'geometries', 'unknown_material'}
        for key in rows[0]:
            if key in ('frame', 'kind', 'hz', 'ns', 'identities'):
                continue
            selected = identity if key in identity_fields else timing
            mean[key] = sum(groups[f][kind][key] for f in selected) / len(selected)
        for row in rows:
            assert row['contaminated'] <= row['draws']
            assert row['ticks'] <= row['elapsed_ticks']
            assert row['repeat_tmg'] <= row['repeat_tm'] <= row['draws']
            assert row['unknown_material'] <= row['draws']
            if 'indexed' in row:
                assert row['indexed'] + row['automatic'] + row['indirect'] == row['draws']
                assert row['empty'] <= row['indexed'] + row['automatic']
        result['families'][kind] = mean
    result['clean_interval_ms'] = sum(result['families'][k]['ms'] for k in (1, 2, 3))
    result['limitations'] = [
        'TSC mede tempo decorrido: preempção do SO e esperas síncronas ainda podem inflar custo.',
        'Intervalos com troca de corrotina são excluídos de ms e contados em contaminated.',
        'Instanciados sem BindMaterial compatível no mesmo DCB têm material desconhecido.',
        'Sem marcador, mede apenas o pacote de draw; indiretos contam pacotes, não subdraws.',
        'Contagem e hashes exatos de identidade têm overhead: comparar A/B antes de concluir.',
    ]
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.run), indent=2, ensure_ascii=False))
