"""GPU black box report (gpu/gpu_blackbox.h): explains a GPU crash of the port and compares crashes.

    python scripts/gpu_crash_report.py              # the latest run
    python scripts/gpu_crash_report.py <run folder>
    python scripts/gpu_crash_report.py --all        # every captured crash, what repeats

Reads gpu-crash.json (device lost) and gpu-stall-N.json (the GPU stopped for 1 s, written before
the driver reset) from the run folder, the Windows driver reset events (LiveKernelEvent 141, TDR)
and the Radeon GPU Detective dumps of the same minutes.
"""
import argparse
import collections
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'out'
def _documents():
    """The user's Documents folder (where Windows has it, e.g. inside OneDrive)."""
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(260)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf) == 0:  # CSIDL_PERSONAL
            return Path(buf.value)
    except (AttributeError, OSError):
        pass
    return Path.home() / 'Documents'


RGD_DIRS = [Path(os.environ['ISS_RGD_DIR'])] if os.environ.get('ISS_RGD_DIR') else \
    [_documents() / 'rgd_dumps/Infamous']
KINDS = ['desenho', 'desenho indexado', 'desenho INDIRETO', 'desenho indexado INDIRETO',
         'dispatch', 'dispatch INDIRETO', 'detile', 'tile', 'SEGMENTO NATIVO', 'desenho nativo']
PATHS = ['CP (caminho original)', 'NATIVO (segmento)', 'NATIVO (desenho do segmento)']


def wall(text):
    return datetime.datetime.strptime(text, '%Y-%m-%d %H:%M:%S.%f')


def windows_resets(when):
    """Driver resets (LiveKernelEvent 141/117) logged within 3 minutes of `when`."""
    start = (when - datetime.timedelta(minutes=3)).strftime('%Y-%m-%dT%H:%M:%S')
    end = (when + datetime.timedelta(minutes=3)).strftime('%Y-%m-%dT%H:%M:%S')
    script = (
        f"$s=[datetime]'{start}';$e=[datetime]'{end}';"
        "Get-WinEvent -FilterHashtable @{LogName='Application';Id=1001;StartTime=$s;EndTime=$e} "
        "-ErrorAction SilentlyContinue | Where-Object { $_.Message -match 'LiveKernelEvent' } | "
        "ForEach-Object { $code=([regex]'P1: (\\w+)').Match($_.Message).Groups[1].Value; "
        "$_.TimeCreated.ToString('HH:mm:ss') + ' codigo ' + $code }")
    try:
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                text=True, timeout=60)
        return sorted(set(line.strip() for line in result.stdout.splitlines() if line.strip()))
    except (OSError, subprocess.TimeoutExpired):
        return []


def rgd_dumps(when):
    found = []
    for folder in RGD_DIRS:
        if not folder.is_dir():
            continue
        for dump in folder.glob('*.rgd'):
            t = datetime.datetime.fromtimestamp(dump.stat().st_mtime)
            if abs((t - when).total_seconds()) < 180:
                found.append(dump)
    return found


def describe(c):
    kind = KINDS[c['kind']] if c['kind'] < len(KINDS) else '?'
    path = PATHS[c['path']] if c['path'] < len(PATHS) else '?'
    return f"#{c['seq']} {path}: {c['text']}" + (' [no pause]' if c.get('paused') else '')


def report(file):
    data = json.loads(file.read_text(encoding='utf-8'))
    when = wall(data['wall'])
    print(f"=== {file.parent.name} / {file.name}")
    print(f"{data['reason']}")
    print(f"hora {data['wall']}, {data['time']:.1f} s de jogo, pause: {'sim' if data['paused'] else 'nao'}")
    for q in data['queues']:
        pending = q['submitted'] > q['done']
        print(f"  fila {q['slot']} ({q['name']}): concluido ate #{q['done']}, iniciado #{q['started']},"
              f" enviado #{q['submitted']}{'  <- PENDENTE' if pending else ''}")
    if data['culprits']:
        print('Culpado provavel (comandos em andamento quando a GPU parou):')
        for c in data['culprits']:
            print('  ' + describe(c))
    else:
        print('Nenhum comando marcado em andamento (o travamento pode estar em trabalho sem marcador).')
    events = data.get('events', [])
    compiles = [e for e in events if e.get('category') == 1 and data['time'] - e['time'] < 5]
    if compiles:
        print(f'Compilacoes nos 5 s anteriores ({len(compiles)}):')
        for e in compiles[-8:]:
            print(f"  -{data['time'] - e['time']:.2f} s  {e['text']}")
    targets = {c[k] for c in data['culprits'] for k in ('color0', 'depth') if c.get(k) not in (None, '0')}
    images = [e for e in events if e.get('category') == 2 and data['time'] - e['time'] < 3]
    touching = [e for e in images if any(t in e['text'] for t in targets)]
    if touching:
        print('Imagens criadas/apagadas nos alvos do culpado (3 s antes):')
        for e in touching[-10:]:
            print(f"  -{data['time'] - e['time']:.2f} s  {e['text']}")
    elif images:
        print(f'{len(images)} imagens criadas/apagadas nos 3 s anteriores (nenhuma nos alvos do culpado).')
    pauses = [e for e in events if e.get('category') == 0 and 'PAUSE' in e['text']]
    if pauses:
        e = pauses[-1]
        print(f"Ultimo evento de pause: {e['text']} ({data['time'] - e['time']:.1f} s antes)")
    resets = windows_resets(when)
    print('Windows (reset do driver): ' + (', '.join(resets) if resets else 'nenhum registrado'))
    dumps = rgd_dumps(when)
    if dumps:
        print('Dumps do RGD do mesmo momento: ' + ', '.join(str(d) for d in dumps))
    print(f"Detalhe completo: {file.with_suffix('.txt')}")
    print()
    return data


def all_crashes():
    files = sorted(OUT.glob('runs/*/gpu-crash.json'))
    if not files:
        print('Nenhum crash capturado pela caixa-preta ainda.')
        return
    rows = []
    for f in files:
        data = json.loads(f.read_text(encoding='utf-8'))
        for c in data['culprits'][:1] or [{}]:
            rows.append((f.parent.name, data, c))
    print(f'{len(rows)} crashes capturados:')
    for run, data, c in rows:
        if not c:
            print(f"  {run}: {data['wall']} sem comando marcado em andamento")
            continue
        print(f"  {run}: {data['wall']} {'PAUSE ' if data['paused'] else ''}"
              f"{PATHS[c['path']] if c['path'] < len(PATHS) else '?'} vs {c['vs']} ps {c['ps']}"
              f" alvo {c['color0']} blend {c['blend']}")
    print('\nO que se repete:')
    for label, key in (('caminho', lambda c: PATHS[c['path']] if c['path'] < len(PATHS) else '?'),
                       ('shader (vs/ps)', lambda c: f"{c['vs']}/{c['ps']}"),
                       ('alvo de cor', lambda c: c['color0']),
                       ('blend', lambda c: 'com blend' if c['blend'] else 'sem blend'),
                       ('pause', lambda c: 'no pause' if c.get('paused') else 'fora do pause')):
        counts = collections.Counter(key(c) for _, _, c in rows if c)
        top = ', '.join(f'{k} ({n}x)' for k, n in counts.most_common(4))
        print(f'  {label}: {top}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', nargs='?')
    ap.add_argument('--all', action='store_true')
    args = ap.parse_args()
    if args.all:
        all_crashes()
        return 0
    run = Path(args.run) if args.run else Path(json.loads((OUT / 'latest-run.json').read_text())['run_dir'])
    files = sorted(run.glob('gpu-stall-*.json')) + sorted(run.glob('gpu-crash.json'))
    if not files:
        print(f'{run}: sem relatorio da caixa-preta (nenhum travamento da GPU nesse run).')
        return 0
    for f in files:
        report(f)
    return 0


if __name__ == '__main__':
    sys.exit(main())
