"""Statistical CPU profiler for the live iss-probe process (read-only, diagnostic).

Samples the busiest threads, unwinds each stack with the modules' x64 unwind tables
(.pdata) and reports exclusive (leaf) and inclusive time per function. iss-gpu.dll
symbols come from a linker map produced for the identical image (see --map).

    python profile_threads.py --seconds 20 --map PATH\\iss-gpu.map --out OUT.json
"""
import argparse
import bisect
import collections
import ctypes as C
import json
import hashlib
import re
import struct
import time
from ctypes import wintypes as W
from pathlib import Path

K = C.WinDLL('kernel32', use_last_error=True)
K.OpenProcess.restype = W.HANDLE
K.OpenThread.restype = W.HANDLE
K.CreateToolhelp32Snapshot.restype = W.HANDLE
K.SuspendThread.argtypes = [W.HANDLE]
K.ResumeThread.argtypes = [W.HANDLE]
K.GetThreadContext.argtypes = [W.HANDLE, C.c_void_p]
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
K.GetThreadTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
K.CloseHandle.argtypes = [W.HANDLE]
K.GetThreadDescription.argtypes = [W.HANDLE, C.POINTER(C.c_wchar_p)]
K.Thread32First.argtypes = [W.HANDLE, C.c_void_p]
K.Thread32Next.argtypes = [W.HANDLE, C.c_void_p]
K.Module32FirstW.argtypes = [W.HANDLE, C.c_void_p]
K.Module32NextW.argtypes = [W.HANDLE, C.c_void_p]
K.Process32FirstW.argtypes = [W.HANDLE, C.c_void_p]
K.Process32NextW.argtypes = [W.HANDLE, C.c_void_p]


class THREADENTRY32(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD),
                ('th32OwnerProcessID', W.DWORD), ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG),
                ('dwFlags', W.DWORD)]


class MODULEENTRY32W(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD),
                ('GlblcntUsage', W.DWORD), ('ProccntUsage', W.DWORD), ('modBaseAddr', C.c_void_p),
                ('modBaseSize', W.DWORD), ('hModule', W.HMODULE), ('szModule', C.c_wchar * 256),
                ('szExePath', C.c_wchar * 260)]


class PROCESSENTRY32W(C.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ProcessID', W.DWORD),
                ('th32DefaultHeapID', C.c_void_p), ('th32ModuleID', W.DWORD), ('cntThreads', W.DWORD),
                ('th32ParentProcessID', W.DWORD), ('pcPriClassBase', W.LONG), ('dwFlags', W.DWORD),
                ('szExeFile', C.c_wchar * 260)]


# CONTEXT offsets (x64)
CTX_SIZE = 1232
OFF_FLAGS = 48
REG_OFF = {0: 120, 1: 128, 2: 136, 3: 144, 4: 152, 5: 160, 6: 168, 7: 176}  # rax..rdi
for i in range(8, 16):
    REG_OFF[i] = 184 + (i - 8) * 8
OFF_RIP = 248


def find_pid(name):
    snap = K.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32W()
    e.dwSize = C.sizeof(e)
    ok = K.Process32FirstW(snap, C.byref(e))
    found = None
    while ok:
        if e.szExeFile.lower() == name.lower():
            found = e.th32ProcessID
        ok = K.Process32NextW(snap, C.byref(e))
    K.CloseHandle(snap)
    return found


class Module:
    def __init__(self, name, base, size, path):
        self.name, self.base, self.size, self.path = name, base, size, path
        self.funcs = None  # sorted list of (begin, end, unwind_rva)
        self.begins = None
        self.data = None
        self.sections = []

    def load(self):
        if self.funcs is not None:
            return
        self.funcs, self.begins = [], []
        try:
            d = Path(self.path).read_bytes()
        except OSError:
            return
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        exc_rva, exc_size = struct.unpack_from('<II', d, opt + 112 + 3 * 8)
        so = opt + optsz
        for i in range(nsec):
            _, vs, va, rs, ro = struct.unpack_from('<8sIIII', d, so + 40 * i)
            self.sections.append((va, max(vs, rs), ro, rs))
        self.data = d
        for i in range(exc_size // 12):
            b, e, u = struct.unpack_from('<III', self.rva_bytes(exc_rva + i * 12, 12), 0)
            if b:
                self.funcs.append((b, e, u))
        self.funcs.sort()
        self.begins = [f[0] for f in self.funcs]

    def rva_bytes(self, rva, n):
        for va, vs, ro, rs in self.sections:
            if va <= rva < va + vs:
                off = rva - va
                return self.data[ro + off: ro + off + n] if off + n <= rs else bytes(n)
        return bytes(n)

    def lookup(self, rva):
        self.load()
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        return None


def unwind_frame(mod, rip, regs, read_qword):
    """Applies one function's unwind info; returns the caller's rip or None."""
    rva = rip - mod.base
    fn = mod.lookup(rva)
    if fn is None:  # leaf function: return address on top of stack
        ret = read_qword(regs[4])
        regs[4] += 8
        return ret
    begin, _, uw = fn
    offset_in_func = rva - begin
    first = True
    while True:
        hdr = mod.rva_bytes(uw, 4)
        ver_flags, prolog, count, frame = hdr
        flags = ver_flags >> 3
        frame_reg, frame_off = frame & 15, (frame >> 4) * 16
        codes = mod.rva_bytes(uw + 4, count * 2)
        # frame pointer establishes the base when present (only if past the prolog point)
        i = 0
        while i < count:
            code_off = codes[i * 2]
            op = codes[i * 2 + 1] & 15
            info = codes[i * 2 + 1] >> 4
            applies = (not first) or code_off <= offset_in_func
            n = 1
            if op == 0:  # PUSH_NONVOL
                if applies:
                    regs[info] = read_qword(regs[4]); regs[4] += 8
            elif op == 1:  # ALLOC_LARGE
                if info == 0:
                    size = struct.unpack_from('<H', codes, (i + 1) * 2)[0] * 8; n = 2
                else:
                    size = struct.unpack_from('<I', codes, (i + 1) * 2)[0]; n = 3
                if applies:
                    regs[4] += size
            elif op == 2:  # ALLOC_SMALL
                if applies:
                    regs[4] += info * 8 + 8
            elif op == 3:  # SET_FPREG
                if applies:
                    regs[4] = regs[frame_reg] - frame_off
            elif op == 4:  # SAVE_NONVOL
                off = struct.unpack_from('<H', codes, (i + 1) * 2)[0] * 8; n = 2
                if applies:
                    regs[info] = read_qword(regs[4] + off)
            elif op == 5:  # SAVE_NONVOL_FAR
                off = struct.unpack_from('<I', codes, (i + 1) * 2)[0]; n = 3
                if applies:
                    regs[info] = read_qword(regs[4] + off)
            elif op == 8:
                n = 2
            elif op == 9:
                n = 3
            elif op == 10:  # PUSH_MACHFRAME
                if applies:
                    base = regs[4] + (8 if info else 0)
                    rip_m = read_qword(base); regs[4] = read_qword(base + 24)
                    return rip_m
            i += n
        if flags & 4:  # chained
            idx = (count + 1) & ~1
            chained = mod.rva_bytes(uw + 4 + idx * 2, 12)
            begin, _, uw = struct.unpack('<III', chained)
            first = False
            continue
        break
    ret = read_qword(regs[4])
    regs[4] += 8
    return ret


def load_map(path):
    # lld-link /MAP (MSVC layout): " 0001:0004bd60  ?Name@...  000000018004cd60  file.obj"
    pat = re.compile(r'^\s*0001:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s')
    syms = []
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        m = pat.match(line)
        if m and not m.group(1).startswith('$'):
            syms.append((int(m.group(2), 16), 0, m.group(1)))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=20)
    ap.add_argument('--hz', type=float, default=1000)
    ap.add_argument('--threads', type=int, default=3)
    ap.add_argument('--pid', type=int)
    ap.add_argument('--thread-name', help='Only sample threads containing this name')
    ap.add_argument('--map')
    ap.add_argument('--out', required=True)
    ap.add_argument('--depth', type=int, default=48)
    ap.add_argument('--timeline', help='Write every sample (perf_counter_ns, tid, stack) to this TSV: '
                    'same clock as the runtime timeline (QPC), to look inside hitch windows')
    a = ap.parse_args()

    pid = a.pid or find_pid('iss-probe.exe')
    if not pid:
        raise SystemExit('iss-probe.exe not running')
    proc = K.OpenProcess(0x410, False, pid)

    snap = K.CreateToolhelp32Snapshot(0x18, pid)
    me = MODULEENTRY32W(); me.dwSize = C.sizeof(me)
    mods = []
    ok = K.Module32FirstW(snap, C.byref(me))
    while ok:
        mods.append(Module(me.szModule, me.modBaseAddr, me.modBaseSize, me.szExePath))
        ok = K.Module32NextW(snap, C.byref(me))
    K.CloseHandle(snap)
    mods.sort(key=lambda m: m.base)
    mod_bases = [m.base for m in mods]

    def module_of(addr):
        i = bisect.bisect_right(mod_bases, addr) - 1
        if i >= 0 and addr < mods[i].base + mods[i].size:
            return mods[i]
        return None

    gpu = next((m for m in mods if m.name.lower() == 'iss-gpu.dll'), None)
    symbol_identity=None
    if a.map:
        identity_path=Path(a.map).with_suffix('.symbols.json')
        if not gpu or not identity_path.is_file():
            raise SystemExit('A matching DLL/map identity manifest is required')
        symbol_identity=json.loads(identity_path.read_text(encoding='utf-8'))
        if (hashlib.sha256(Path(gpu.path).read_bytes()).hexdigest()!=symbol_identity['dll_sha256'] or
            hashlib.sha256(Path(a.map).read_bytes()).hexdigest()!=symbol_identity['map_sha256']):
            raise SystemExit('Profiler DLL/map identity mismatch')
    syms = load_map(a.map) if a.map else []
    sym_vas = [s[0] for s in syms]
    image_base_map = 0x180000000

    def symbolize(addr):
        m = module_of(addr)
        if m is None:
            if 0x10000000 <= addr < 0x18240000:
                return 'eboot+0x%x' % (addr - 0x10000000)
            return '?0x%x' % addr
        if m is gpu and syms:
            va = addr - m.base + image_base_map
            i = bisect.bisect_right(sym_vas, va) - 1
            if i >= 0:
                return syms[i][2]
        if m is not None:
            m.load()
            fn = m.lookup(addr - m.base)
            if fn:
                return '%s!0x%x' % (m.name, fn[0])
            return '%s+0x%x' % (m.name, addr - m.base)

    # pick busiest threads over 1 second
    snap = K.CreateToolhelp32Snapshot(4, 0)
    te = THREADENTRY32(); te.dwSize = C.sizeof(te)
    tids = []
    ok = K.Thread32First(snap, C.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            tids.append(te.th32ThreadID)
        ok = K.Thread32Next(snap, C.byref(te))
    K.CloseHandle(snap)

    def cpu_time(h):
        c, e, k, u = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
        K.GetThreadTimes(h, C.byref(c), C.byref(e), C.byref(k), C.byref(u))
        return ((k.dwHighDateTime << 32 | k.dwLowDateTime) + (u.dwHighDateTime << 32 | u.dwLowDateTime)) / 1e7

    handles = {}
    for t in tids:
        h = K.OpenThread(0x0040 | 0x0008 | 0x0002 | 0x0400, False, t)
        if h:
            handles[t] = h
    t0 = {t: cpu_time(h) for t, h in handles.items()}
    time.sleep(1.0)
    usage = {t: cpu_time(h) - t0[t] for t, h in handles.items()}
    names = {}
    for t, h in handles.items():
        p = C.c_wchar_p()
        if K.GetThreadDescription(h, C.byref(p)) >= 0 and p.value:
            names[t] = p.value
    busiest = sorted(usage, key=usage.get, reverse=True)
    report_threads = [{'tid': t, 'name': names.get(t, ''), 'cpu_1s': round(usage[t], 3)} for t in busiest[:16]]
    # --thread-name: one name part, or several separated by '|'.
    wanted = [w.lower() for w in a.thread_name.split('|')] if a.thread_name else []
    chosen = [t for t in busiest if not wanted or
              any(w in names.get(t,'').lower() for w in wanted)][:a.threads]
    if not chosen:raise SystemExit('Requested thread not found')

    raw = C.create_string_buffer(CTX_SIZE + 16)
    ctx = (C.addressof(raw) + 15) & ~15
    stack_buf = C.create_string_buffer(65536)
    n_read = C.c_size_t()

    leaf = {t: collections.Counter() for t in chosen}
    incl = {t: collections.Counter() for t in chosen}
    edges = {t: collections.Counter() for t in chosen}
    stacks = {t: collections.Counter() for t in chosen}
    samples = collections.Counter()
    period = 1.0 / a.hz
    timeline = open(a.timeline, 'w', encoding='utf-8') if a.timeline else None
    end = time.perf_counter() + a.seconds
    t_cpu0 = {t: cpu_time(handles[t]) for t in chosen}
    while time.perf_counter() < end:
        for t in chosen:
            h = handles[t]
            C.c_uint32.from_address(ctx + OFF_FLAGS).value = 0x100003
            if K.SuspendThread(h) == 0xFFFFFFFF:
                continue
            try:
                sample_ns = time.perf_counter_ns()
                if not K.GetThreadContext(h, ctx):
                    continue
                cbytes = C.string_at(ctx, CTX_SIZE)
                regs = {r: struct.unpack_from('<Q', cbytes, o)[0] for r, o in REG_OFF.items()}
                rip = struct.unpack_from('<Q', cbytes, OFF_RIP)[0]
                rsp0 = regs[4]
                K.ReadProcessMemory(proc, C.c_void_p(rsp0), stack_buf, 65536, C.byref(n_read))
                stack = stack_buf.raw[:n_read.value]
            finally:
                K.ResumeThread(h)

            def rq(addr, _s=stack, _b=rsp0):
                o = addr - _b
                if 0 <= o <= len(_s) - 8:
                    return struct.unpack_from('<Q', _s, o)[0]
                return 0
            frames = [rip]
            cur = rip
            for _ in range(a.depth):
                m = module_of(cur)
                if m is None:
                    # The game's code has no unwind tables, but its functions keep rbp frames
                    # (push rbp; mov rbp, rsp): walk the frame pointer chain.
                    if not 0x10000000 <= cur < 0x18240000:
                        break
                    rbp = regs[5]
                    ret, up = rq(rbp + 8), rq(rbp)
                    if not ret or up <= rbp:
                        break
                    regs[5], regs[4] = up, rbp + 16
                    frames.append(ret)
                    cur = ret
                    continue
                m.load()
                try:
                    nxt = unwind_frame(m, cur if len(frames) == 1 else cur - 1, regs, rq) if True else None
                except Exception:
                    break
                if not nxt or regs[4] < rsp0:
                    break
                frames.append(nxt)
                cur = nxt
            samples[t] += 1
            syms_f = [symbolize(f if i == 0 else f - 1) for i, f in enumerate(frames)]
            leaf[t][syms_f[0]] += 1
            for s in set(syms_f):
                incl[t][s] += 1
            if len(syms_f) > 1:
                edges[t][(syms_f[1], syms_f[0])] += 1
            stacks[t][';'.join(reversed(syms_f))] += 1
            if timeline:
                timeline.write('%d\t%d\t%s\n' % (sample_ns, t, ';'.join(syms_f[:16])))
        time.sleep(period)
    if timeline:
        timeline.close()
    cpu_used = {t: cpu_time(handles[t]) - t_cpu0[t] for t in chosen}
    out = {'pid': pid, 'seconds': a.seconds, 'threads_by_cpu': report_threads, 'profiles': [],
           'symbol_identity':symbol_identity,'requested_hz':a.hz,
           'method':'Brief SuspendThread/context+stack copy, unwind after resume; diagnostic overhead affects timing.'}
    for t in chosen:
        n = max(samples[t], 1)
        out['profiles'].append({
            'tid': t, 'name': names.get(t, ''), 'samples': samples[t],
            'cpu_seconds': round(cpu_used[t], 3),
            'leaf': [(s, c, round(100 * c / n, 2)) for s, c in leaf[t].most_common(80)],
            'inclusive': [(s, c, round(100 * c / n, 2)) for s, c in incl[t].most_common(120)],
            'stacks': stacks[t].most_common(4000),
        })
    Path(a.out).write_text(json.dumps(out, indent=1), encoding='utf-8')
    print(json.dumps(report_threads[:10], indent=1))
    for p in out['profiles']:
        print('==', p['tid'], p['name'], p['samples'], 'samples, cpu', p['cpu_seconds'])
        for s, c, pct in p['leaf'][:25]:
            print('  leaf %6.2f%% %s' % (pct, s[:150]))


if __name__ == '__main__':
    main()
