"""Children breakdown of a function in a profile_threads.py report.

    python analyze_profile.py PROFILE.json THREAD_SUBSTRING FUNCTION_SUBSTRING [depth]

Prints, for the samples whose stack contains the function (outermost match), the share of
the thread's samples spent in each callee below it, nested to the given depth.
"""
import collections
import json
import re
import sys


def short(name):
    m = re.match(r'\?+([^@]+)@([^@]+)@', name)
    return (m.group(2) + '::' + m.group(1)) if m else name[:80]


def main():
    path, thread, func = sys.argv[1:4]
    depth = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    report = json.load(open(path, encoding='utf-8'))
    prof = next(p for p in report['profiles'] if thread.lower() in (p['name'] + str(p['tid'])).lower())
    total = prof['samples']
    tree = collections.Counter()
    hits = 0
    for stack, count in prof['stacks']:
        frames = [short(f) for f in stack.split(';')]
        idx = next((i for i, f in enumerate(frames) if func.lower() in f.lower()), None)
        if idx is None:
            continue
        hits += count
        below = frames[idx + 1: idx + 1 + depth]
        for d in range(len(below)):
            tree[tuple(below[:d + 1])] += count
        if not below:
            tree[('<self>',)] += count
    print('%s: %.2f%% of %d samples' % (func, 100 * hits / total, total))
    for key, count in sorted(tree.items(), key=lambda kv: (kv[0][:1], -kv[1])):
        pass
    def show(prefix, level):
        children = [(k, c) for k, c in tree.items() if len(k) == level + 1 and k[:level] == prefix]
        for k, c in sorted(children, key=lambda kv: -kv[1]):
            if 100 * c / total < 0.3:
                continue
            print('%s%6.2f%% %s' % ('  ' * level, 100 * c / total, k[-1]))
            if level + 1 < depth:
                show(k, level + 1)
    show((), 0)


if __name__ == '__main__':
    main()
