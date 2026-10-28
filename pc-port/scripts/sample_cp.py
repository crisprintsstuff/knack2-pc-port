"""Sampling profiler for the command processor thread of a live iss-probe process.

Usage: python scripts/sample_cp.py <out.json> [--wait SECONDS] [--seconds SECONDS] [--map MAP]

Waits for iss-probe.exe, lets it run --wait seconds, picks the busiest thread (the command
processor) and records its instruction pointer plus the first code addresses found on its
stack, about a thousand times per second. The report maps them to iss-gpu.dll functions with
the linker map (inline functions count in their caller).
"""
import argparse
import bisect
import collections
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import re
import struct
import time

K = C.WinDLL('kernel32', use_last_error=True)
P = C.WinDLL('psapi', use_last_error=True)
WM = C.WinDLL('winmm')


class ThreadEntry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('tid', W.DWORD), ('pid', W.DWORD),
                ('base', W.LONG), ('delta', W.LONG), ('flags', W.DWORD)]


class ProcessEntry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD), ('heap', C.c_void_p),
                ('module', W.DWORD), ('threads', W.DWORD), ('parent', W.DWORD),
                ('priority', W.LONG), ('flags', W.DWORD), ('exe', W.WCHAR * 260)]


K.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
K.CreateToolhelp32Snapshot.restype = W.HANDLE
K.Thread32First.argtypes = [W.HANDLE, C.POINTER(ThreadEntry)]
K.Thread32Next.argtypes = K.Thread32First.argtypes
K.Process32FirstW.argtypes = [W.HANDLE, C.POINTER(ProcessEntry)]
K.Process32NextW.argtypes = K.Process32FirstW.argtypes
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE
K.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenThread.restype = W.HANDLE
K.SuspendThread.argtypes = [W.HANDLE]
K.SuspendThread.restype = W.DWORD
K.ResumeThread.argtypes = [W.HANDLE]
K.GetThreadContext.argtypes = [W.HANDLE, C.c_void_p]
K.GetThreadTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t,
                                C.POINTER(C.c_size_t)]
K.CloseHandle.argtypes = [W.HANDLE]
K.GetExitCodeProcess.argtypes = [W.HANDLE, C.POINTER(W.DWORD)]
K.GetThreadDescription.argtypes = [W.HANDLE, C.POINTER(C.c_wchar_p)]
P.EnumProcessModulesEx.argtypes = [W.HANDLE, C.POINTER(C.c_void_p), W.DWORD,
                                   C.POINTER(W.DWORD), W.DWORD]
P.GetModuleBaseNameW.argtypes = [W.HANDLE, C.c_void_p, W.LPWSTR, W.DWORD]
P.GetModuleFileNameExW.argtypes = [W.HANDLE, C.c_void_p, W.LPWSTR, W.DWORD]


class ModuleInfo(C.Structure):
    _fields_ = [('base', C.c_void_p), ('size', W.DWORD), ('entry', C.c_void_p)]


P.GetModuleInformation.argtypes = [W.HANDLE, C.c_void_p, C.POINTER(ModuleInfo), W.DWORD]


def find_pid(name='iss-probe.exe'):
    snap = K.CreateToolhelp32Snapshot(2, 0)
    e = ProcessEntry()
    e.size = C.sizeof(e)
    ok = K.Process32FirstW(snap, C.byref(e))
    pid = None
    while ok:
        if e.exe.lower() == name:
            pid = e.pid
        ok = K.Process32NextW(snap, C.byref(e))
    K.CloseHandle(snap)
    return pid


def threads_of(pid):
    snap = K.CreateToolhelp32Snapshot(4, 0)
    e = ThreadEntry()
    e.size = C.sizeof(e)
    ok = K.Thread32First(snap, C.byref(e))
    out = []
    while ok:
        if e.pid == pid:
            out.append(e.tid)
        ok = K.Thread32Next(snap, C.byref(e))
    K.CloseHandle(snap)
    return out


def cpu_time(handle):
    t = [W.FILETIME() for _ in range(4)]
    if not K.GetThreadTimes(handle, *[C.byref(x) for x in t]):
        return 0
    return sum((x.dwHighDateTime << 32 | x.dwLowDateTime) for x in t[2:])


def modules(process):
    arr = (C.c_void_p * 1024)()
    needed = W.DWORD()
    P.EnumProcessModulesEx(process, arr, C.sizeof(arr), C.byref(needed), 3)
    out = []
    for i in range(needed.value // C.sizeof(C.c_void_p)):
        name = C.create_unicode_buffer(260)
        P.GetModuleBaseNameW(process, arr[i], name, 260)
        path = C.create_unicode_buffer(1024)
        P.GetModuleFileNameExW(process, arr[i], path, 1024)
        info = ModuleInfo()
        P.GetModuleInformation(process, arr[i], C.byref(info), C.sizeof(info))
        out.append((info.base, info.size, name.value, path.value))
    return sorted(out)


def load_exports(path):
    """Sorted (rva, name) of a PE file's exported functions."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return [], []
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    exp_rva = struct.unpack_from('<I', data, opt + 112)[0]
    secs = []
    for i in range(nsec):
        o = opt + opt_size + i * 40
        va, vs, raw = struct.unpack_from('<III', data, o + 12)[0], 0, 0
        vsz, va, rawsz, rawp = struct.unpack_from('<IIII', data, o + 8)
        secs.append((va, max(vsz, rawsz), rawp))

    def off(rva):
        for va, sz, rawp in secs:
            if va <= rva < va + sz:
                return rva - va + rawp
        return None

    if not exp_rva or off(exp_rva) is None:
        return [], []
    e = off(exp_rva)
    nfun, nname, afun, aname, aord = struct.unpack_from('<IIIII', data, e + 20)
    funcs = [struct.unpack_from('<I', data, off(afun) + 4 * i)[0] for i in range(nfun)]
    out = []
    for i in range(nname):
        nrva = struct.unpack_from('<I', data, off(aname) + 4 * i)[0]
        ordi = struct.unpack_from('<H', data, off(aord) + 2 * i)[0]
        no = off(nrva)
        name = data[no:data.index(b'\x00', no)].decode('ascii', 'replace')
        out.append((funcs[ordi], name))
    out.sort()
    return [o[0] for o in out], [o[1] for o in out]


def load_map(path):
    rx = re.compile(r'^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s', re.I)
    syms = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = rx.match(line)
        if m:
            syms.append((int(m.group(2), 16) - 0x180000000, m.group(1)))
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]


def demangle(name):
    m = re.match(r'\?+([^@]+)@([^@]+)@', name)
    if m and not name.startswith('?dtor'):
        return f'{m.group(2)}::{m.group(1)}'
    return name[:90]


def symbolize(dll, rvas, total):
    """Self samples by source line and by innermost inlined function (llvm-symbolizer)."""
    import subprocess
    tool = Path(__file__).resolve().parents[2] / 'tools/llvm/bin/llvm-symbolizer.exe'
    unique = sorted({r for r in rvas if r is not None})
    text = '\n'.join(hex(r) for r in unique) + '\n'
    out = subprocess.run([str(tool), f'--obj={dll}', '--inlining', '--functions=short',
                          '--relative-address'], input=text, capture_output=True, text=True).stdout
    blocks = out.split('\n\n')
    info = {}
    for r, block in zip(unique, blocks):
        lines = [l for l in block.strip().split('\n') if l]
        frames = [(lines[i], lines[i + 1] if i + 1 < len(lines) else '')
                  for i in range(0, len(lines), 2)]
        info[r] = frames
    by_line = collections.Counter()
    by_func = collections.Counter()
    for r in rvas:
        if r is None or r not in info or not info[r]:
            continue
        func, loc = info[r][0]
        loc = re.sub(r'^.*[\/](gpu|src)[\/]', r'\1/', loc)
        by_line[f'{loc} {func}'] += 1
        chain = ' < '.join(f for f, _ in info[r][:3])
        by_func[chain] += 1
    return {
        'lines': [(k, v, round(100 * v / total, 2)) for k, v in by_line.most_common(120)],
        'inline_funcs': [(k, v, round(100 * v / total, 2)) for k, v in by_func.most_common(120)],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--wait', type=float, default=60)
    ap.add_argument('--seconds', type=float, default=20)
    ap.add_argument('--map', default=None)
    ap.add_argument('--dll', default=None, help='iss-gpu.dll with a PDB: report by source line')
    ap.add_argument('--name', default='GpuCommandProcessor', help='thread name to sample')
    ap.add_argument('--interval', type=float, default=0.0007, help='sleep between samples')
    ap.add_argument('--busy-min', type=float, default=0,
                    help='report only seconds in which the thread was at least this busy (%%)')
    a = ap.parse_args()
    pid = None
    while pid is None:
        pid = find_pid()
        time.sleep(0.5)
    start = time.time()
    time.sleep(a.wait)
    process = K.OpenProcess(0x410, False, pid)
    tids = threads_of(pid)
    handles = {t: K.OpenThread(0x4A, False, t) for t in tids}
    before = {t: cpu_time(h) for t, h in handles.items() if h}
    time.sleep(1.0)
    busy = sorted(((cpu_time(h) - before.get(t, 0), t) for t, h in handles.items() if h),
                  reverse=True)
    names = {}
    for _, t in busy:
        p = C.c_wchar_p()
        if K.GetThreadDescription(handles[t], C.byref(p)) >= 0 and p.value:
            names[t] = p.value
    tid = next((t for _, t in busy if a.name and a.name in names.get(t, '')), busy[0][1])
    for t2, h in handles.items():
        if t2 != tid and h:
            K.CloseHandle(h)
    thread = handles[tid]
    mods = modules(process)
    gpu = next(m for m in mods if m[2].lower() == 'iss-gpu.dll')
    buf = C.create_string_buffer(1250)
    ctx = (C.addressof(buf) + 15) & ~15
    stack = C.create_string_buffer(1024)
    n = C.c_size_t()
    samples = []
    # Busy percentage of the sampled thread per wall-clock second (samples keep their second).
    busy_by_second = []
    second_start, second_cpu = time.perf_counter(), cpu_time(thread)
    WM.timeBeginPeriod(1)
    t_end = time.time() + a.seconds
    while time.time() < t_end:
        now = time.perf_counter()
        if now - second_start >= 1.0:
            code = W.DWORD()
            if not K.GetExitCodeProcess(process, C.byref(code)) or code.value != 259:
                break  # the game exited (259 = STILL_ACTIVE)
            cpu = cpu_time(thread)
            busy_by_second.append(round((cpu - second_cpu) / 1e5 / (now - second_start), 1))
            second_start, second_cpu = now, cpu
        C.c_uint32.from_address(ctx + 48).value = 0x100003
        if K.SuspendThread(thread) == 0xFFFFFFFF:
            break
        try:
            if K.GetThreadContext(thread, ctx):
                raw = C.string_at(ctx, 256)
                rip = struct.unpack_from('<Q', raw, 248)[0]
                rsp = struct.unpack_from('<Q', raw, 152)[0]
                K.ReadProcessMemory(process, C.c_void_p(rsp), stack, 1024, C.byref(n))
            else:
                rip = 0
        finally:
            K.ResumeThread(thread)
        callers = []
        if rip:
            words = struct.unpack_from('<128Q', stack.raw)
            for w in words:
                if gpu[0] <= w < gpu[0] + gpu[1]:
                    callers.append(w - gpu[0])
                    if len(callers) == 6:
                        break
            samples.append((rip, callers, len(busy_by_second)))
        time.sleep(a.interval)
    WM.timeEndPeriod(1)
    all_samples = len(samples)
    if a.busy_min > 0:
        kept = {i for i, b in enumerate(busy_by_second) if b >= a.busy_min}
        samples = [s for s in samples if s[2] in kept]
    samples = [(rip, callers) for rip, callers, _ in samples]

    def where(addr):
        for base, size, name, _ in mods:
            if base <= addr < base + size:
                return name, addr - base
        return '?', addr

    exports = {}

    def ext_sym(mod, off):
        if mod not in exports:
            path = next((m[3] for m in mods if m[2] == mod), None)
            exports[mod] = load_exports(path) if path else ([], [])
        a_, n_ = exports[mod]
        i = bisect.bisect_right(a_, off) - 1
        return f'{mod}!{n_[i]}' if i >= 0 else f'[{mod}]'

    map_path = a.map or str(Path(__file__).resolve().parents[1] /
                            'out/experimental/20261014-census/iss-gpu.map')
    addrs, syms = load_map(map_path)

    def sym(rva):
        i = bisect.bisect_right(addrs, rva) - 1
        return demangle(syms[i]) if i >= 0 else hex(rva)

    self_count = collections.Counter()
    incl = collections.Counter()
    ext_callers = collections.Counter()
    for rip, callers in samples:
        mod, off = where(rip)
        leaf = sym(off) if mod.lower() == 'iss-gpu.dll' else ext_sym(mod, off)
        self_count[leaf] += 1
        if mod.lower() != 'iss-gpu.dll' and callers:
            ext_callers[f'{leaf} <- {sym(callers[0])}'] += 1
        seen = {leaf}
        for c in callers:
            s = sym(c)
            if s not in seen:
                incl[s] += 1
                seen.add(s)
        incl[leaf] += 1
    total = max(len(samples), 1)
    report = {
        'pid': pid, 'tid': tid, 'thread_names': names, 'samples': len(samples),
        'samples_before_busy_filter': all_samples, 'busy_min': a.busy_min,
        'busy_by_second': busy_by_second,
        'busy_top': [(d / 1e4, t) for d, t in busy[:6]],
        'self': [(k, v, round(100 * v / total, 2)) for k, v in self_count.most_common(80)],
        'inclusive': [(k, v, round(100 * v / total, 2)) for k, v in incl.most_common(80)],
        'external_callers': [(k, v, round(100 * v / total, 2))
                             for k, v in ext_callers.most_common(60)],
    }
    gpu_rvas = []
    for rip, callers in samples:
        mod, off = where(rip)
        gpu_rvas.append(off if mod.lower() == 'iss-gpu.dll' else
                        (callers[0] if callers else None))
    report['raw_gpu_rvas'] = gpu_rvas
    if a.dll:
        report.update(symbolize(a.dll, gpu_rvas, total))
    Path(a.out).write_text(json.dumps(report, indent=1), encoding='utf-8')
    for key in ('lines', 'inline_funcs'):
        if key in report:
            print(f'--- {key}')
            for k, v, p in report[key][:45]:
                print(f'{p:6.2f}% {k}')
    print('samples', len(samples), 'tid', tid, names.get(tid))
    for k, v, p in report['self'][:40]:
        print(f'{p:6.2f}% {k}')
    print('--- external leaves by first iss-gpu caller')
    for k, v, p in report['external_callers'][:30]:
        print(f'{p:6.2f}% {k}')
    print('--- inclusive (stack scan, heuristic)')
    for k, v, p in report['inclusive'][:40]:
        print(f'{p:6.2f}% {k}')


if __name__ == '__main__':
    main()
