"""Plays the approved profile (same build, save and cache as the launcher) with flags changed.

Diagnosis only: each --set KEY=VALUE overrides one approved flag for this session. The approved
profile, launcher and saves are not modified.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_probe  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE')
    args = parser.parse_args()
    for item in args.set:
        key, sep, value = item.partition('=')
        if not sep or key not in run_probe.APPROVED_FLAGS:
            parser.error(f'expected an approved flag KEY=VALUE, got {item!r}')
        run_probe.APPROVED_FLAGS[key] = value
        print(f'{key}={value}')
    return run_probe.run(entry=True, linked=True, gpu=True, timeout=0, approved=True)


if __name__ == '__main__':
    raise SystemExit(main())
