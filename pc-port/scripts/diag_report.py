"""Report of a play_diag.py session: where the frame time goes and what limits 60 FPS.

Usage: python scripts/diag_report.py <run dir>
Writes <run dir>/diagnostico.md and prints it. Gameplay frames are the ones with more than 1000
draws (menus and loading screens are left out).
"""
import collections
import csv
import json
from pathlib import Path
import re
import sys

TARGET = 1000 / 60


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(p / 100 * len(values)))]


def mean(values):
    return sum(values) / len(values) if values else 0.0


def num(row, key):
    try:
        return float(row.get(key) or 0)
    except ValueError:
        return 0.0


def frames_section(run, out):
    rows = list(csv.DictReader(open(run / 'frames.csv', encoding='utf-8')))
    game = [r for r in rows if num(r, 'draws') > 1000]
    out.append(f'Quadros totais: {len(rows)}; quadros de gameplay (>1000 draws): {len(game)}  ')
    if not game:
        out.append('\nSem quadros de gameplay suficientes para análise.')
        return
    present = [num(r, 'present_ms') for r in game]
    total_s = sum(present) / 1000
    out.append(f'Tempo de gameplay: {total_s / 60:.1f} min\n')
    out.append('| métrica | média | p50 | p95 | p99 |')
    out.append('|---|---|---|---|---|')
    keys = [('present_ms', 'tempo de quadro (ms)'), ('command_processor_ms', 'CP (ms)'),
            ('gpu_completed_work_ms', 'GPU trabalho (ms)'), ('gpu_wait_ms', 'espera GPU (ms)'),
            ('gpu_lag_wait_ms', 'espera atraso GPU (ms)'),
            ('pipeline_stall_ms', 'stall pipeline (ms)'), ('shader_compile_ms', 'compilação shader (ms)'),
            ('readback_stall_ms', 'stall readback (ms)'), ('buffer_upload_ms', 'upload buffer (ms)'),
            ('image_refresh_ms', 'refresh imagem (ms)'), ('page_protect_ms', 'proteção página (ms)'),
            ('recorder_work_ms', 'gravador trabalho (ms)'), ('upload_worker_wait_ms', 'espera upload worker (ms)'),
            ('draws', 'draws'), ('dispatches', 'dispatches')]
    for key, label in keys:
        v = [num(r, key) for r in game]
        if any(v):
            out.append(f'| {label} | {mean(v):.2f} | {pct(v, 50):.2f} | {pct(v, 95):.2f} | {pct(v, 99):.2f} |')
    fps = len(game) / max(total_s, 1e-9)
    under = 100 * sum(1 for p in present if p <= TARGET + 0.5) / len(present)
    cp = [num(r, 'command_processor_ms') for r in game]
    draws = sum(num(r, 'draws') + num(r, 'dispatches') for r in game)
    out.append(f'\nFPS médio de gameplay: **{fps:.1f}**; quadros ≤16,7 ms: {under:.1f}%; '
               f'CP por draw: {1000 * sum(cp) / max(draws, 1):.3f} µs')
    out.append(f'Corte de CP necessário para 60 FPS (média): **{max(0, mean(cp) - TARGET):.2f} ms**'
               f' (p95: {max(0, pct(cp, 95) - TARGET):.2f} ms)\n')

    # What limits each slow frame: the largest of CP, GPU work, and the stalls.
    causes = collections.Counter()
    for r in game:
        if num(r, 'present_ms') <= TARGET + 0.5:
            continue
        cp_ms, gpu_ms = num(r, 'command_processor_ms'), num(r, 'gpu_completed_work_ms')
        causes['GPU' if gpu_ms > cp_ms else 'CP'] += 1
    slow = sum(causes.values())
    if slow:
        out.append('Quadros acima de 16,7 ms, limitados por (maior entre CP e GPU):')
        for k, v in causes.most_common():
            out.append(f'- {k}: {v} ({100 * v / slow:.1f}%)')
    # Waits recorded in the frame. Readback stalls are timed on the thread that waits (usually the
    # game thread waiting for the command processor), so they are not a slice of the CP time.
    inner = [('readback_stall_ms', 'stall readback'), ('pipeline_stall_ms', 'stall pipeline'),
             ('shader_compile_ms', 'compilação shader'), ('page_protect_ms', 'proteção página'),
             ('buffer_upload_ms', 'upload buffer'), ('gpu_lag_wait_ms', 'espera atraso GPU'),
             ('command_spin_ms', 'spin do CP')]
    out.append('\nEsperas registradas no quadro (média; o stall de readback é a thread do jogo '
               'esperando o CP, não parte do tempo do CP):')
    for key, label in inner:
        v = mean([num(r, key) for r in game])
        if v:
            out.append(f'- {label}: {v:.2f} ms')
    out.append('\nPiores 15 quadros (travadas):\n')
    out.append('| quadro | ms | CP | GPU | pipeline | shader | readback | pipelines novos | upload MB |')
    out.append('|---|---|---|---|---|---|---|---|---|')
    for r in sorted(game, key=lambda r: -num(r, 'present_ms'))[:15]:
        out.append(f"| {r['frame']} | {num(r, 'present_ms'):.1f} | {num(r, 'command_processor_ms'):.1f} | "
                   f"{num(r, 'gpu_completed_work_ms'):.1f} | {num(r, 'pipeline_stall_ms'):.1f} | "
                   f"{num(r, 'shader_compile_ms'):.1f} | {num(r, 'readback_stall_ms'):.1f} | "
                   f"{num(r, 'pipelines_created'):.0f} | {num(r, 'buffer_upload_bytes') / 2**20:.1f} |")
    # Per-minute evolution: shows whether the cost depends on the place in the city.
    out.append('\nPor minuto de gameplay:\n')
    out.append('| min | FPS | CP ms | GPU ms | draws | dispatches |')
    out.append('|---|---|---|---|---|---|')
    bucket, elapsed, minute = [], 0.0, 0
    for r in game + [None]:
        if r is not None:
            bucket.append(r)
            elapsed += num(r, 'present_ms')
        if (r is None and bucket) or elapsed >= 60000:
            n = len(bucket)
            f = lambda k: sum(num(x, k) for x in bucket) / n
            out.append(f'| {minute} | {1000 * n / max(sum(num(x, "present_ms") for x in bucket), 1):.1f} | '
                       f'{f("command_processor_ms"):.2f} | {f("gpu_completed_work_ms"):.2f} | '
                       f'{f("draws"):.0f} | {f("dispatches"):.0f} |')
            bucket, elapsed, minute = [], 0.0, minute + 1


def threads_section(run, out):
    path = run / 'threads.csv'
    if not path.is_file():
        return
    busy = collections.defaultdict(float)
    names = {}
    first = last = None
    for r in csv.DictReader(open(path, encoding='utf-8')):
        t = float(r['elapsed_s'])
        first = t if first is None else first
        last = t
        busy[r['tid']] += float(r['delta_cpu_s'] or 0)
        if r['name']:
            names[r['tid']] = r['name']
    span = max((last or 0) - (first or 0), 1)
    out.append('\n## Threads (CPU médio na sessão inteira)\n')
    out.append('| thread | ocupação |')
    out.append('|---|---|')
    for tid, s in sorted(busy.items(), key=lambda x: -x[1])[:14]:
        out.append(f'| {names.get(tid, tid)} | {100 * s / span:.1f}% |')


def profile_section(run, out):
    path = run / 'cp-lines.json'
    if not path.is_file():
        out.append('\n## Perfil do CP\n\nPerfil ausente (ver cp-lines.log).')
        return
    p = json.loads(path.read_text(encoding='utf-8'))
    busy = p.get('busy_by_second', [])
    out.append('\n## Perfil do CP por linha de código\n')
    out.append(f"Amostras usadas: {p['samples']} de {p.get('samples_before_busy_filter')} "
               f"(segundos com CP ≥{p.get('busy_min')}% ocupado: "
               f"{sum(1 for b in busy if b >= (p.get('busy_min') or 0))} de {len(busy)})\n")
    for key, title, n in (('inline_funcs', 'Funções (inline, folha < chamador)', 40),
                          ('lines', 'Linhas', 50), ('self', 'Funções (mapa)', 30),
                          ('inclusive', 'Inclusivo (heurístico)', 30),
                          ('external_callers', 'Fora da DLL (driver/OS) por chamador', 20)):
        if key not in p:
            continue
        out.append(f'### {title}\n')
        for k, v, share in p[key][:n]:
            out.append(f'- {share:.2f}% `{k}`')
        out.append('')


def counters_section(run, out):
    text = (run / 'stderr.log').read_text(encoding='utf-8', errors='replace')
    last = {}
    blocks = collections.defaultdict(list)
    for line in text.splitlines():
        m = re.match(r'(DRAW_CYCLES|SWITCH_CYCLES|DISPATCH_CYCLES|STAGE_MEMO_\w+|OBTAIN_CENSUS|'
                     r'SEGMENT_CENSUS|DRAW_CENSUS|NATIVE_DEPTH_RUN\w*|PORT_TUNING|FAST_DRAW\w*)\b', line)
        if not m:
            continue
        tag = m.group(1)
        if tag in ('OBTAIN_CENSUS', 'DRAW_CENSUS', 'SEGMENT_CENSUS', 'PORT_TUNING'):
            blocks[tag].append(line)
        else:
            last[tag] = line
    out.append('\n## Contadores (último relatório de cada um)\n')
    out.append('```')
    for tag, line in sorted(last.items()):
        out.append(line[:400])
    for tag in ('OBTAIN_CENSUS', 'SEGMENT_CENSUS', 'DRAW_CENSUS'):
        for line in blocks[tag][-24:]:
            out.append(line[:400])
    out.append('```')
    errors = [l for l in text.splitlines() if re.search(r'\b(ERROR|Critical|VALIDATION|NATIVE_VISUAL_ERROR)\b', l)]
    if errors:
        out.append(f'\nLinhas de erro no stderr: {len(errors)} (primeiras 10)\n')
        out.append('```')
        out += [l[:300] for l in errors[:10]]
        out.append('```')


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    run = Path(sys.argv[1])
    record = json.loads((run / 'run.json').read_text(encoding='utf-8'))
    out = [f'# Diagnóstico de sessão — {run.name}\n',
           f"Build: `{record.get('executable_path')}`; saída: {record.get('exit_code')}\n",
           '## Quadros\n']
    for section in (frames_section, threads_section, profile_section, counters_section):
        try:
            section(run, out)
        except Exception as error:
            out.append(f'\n(seção {section.__name__} falhou: {error})\n')
    text = '\n'.join(out) + '\n'
    (run / 'diagnostico.md').write_text(text, encoding='utf-8')
    print(text)
    print(f'Relatório: {run / "diagnostico.md"}')


if __name__ == '__main__':
    main()
