"""Low-cost Windows CPU samples for threads owned by one process.

The clock is Python's monotonic performance counter (QPC on Windows). These
timestamps are not aligned with Vulkan/GPU timestamps.
"""

import csv
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import sys
import threading
import time


TH32CS_SNAPTHREAD = 0x00000004
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
INVALID_HANDLE_VALUE = C.c_void_p(-1).value


class ThreadEntry(C.Structure):
    _fields_ = [
        ('dwSize', W.DWORD), ('cntUsage', W.DWORD),
        ('th32ThreadID', W.DWORD), ('th32OwnerProcessID', W.DWORD),
        ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD),
    ]


def _filetime_value(value):
    return (value.dwHighDateTime << 32) | value.dwLowDateTime


def _status(path, state, detail='', winerror=0):
    message = {'status': state, 'detail': detail, 'winerror': winerror}
    try:
        path.with_name(path.stem + '.status.json').write_text(
            json.dumps(message, indent=2) + '\n', encoding='utf-8')
    except OSError:
        pass
    if state != 'ok':
        print(f'THREAD_METRICS status={state} detail={detail} winerror={winerror}',
              file=sys.stderr, flush=True)


def start_thread_metrics(pid, path):
    """Start 1 Hz per-thread CPU sampling; return (stop_event, daemon_thread).

    ``delta_cpu_s`` is zero on a thread's first sighting. Reused TIDs are
    distinguished by creation FILETIME. Missing or inaccessible threads are
    skipped; an inaccessible process or failed snapshot is reported beside the
    CSV and does not affect the target process.
    """
    path = Path(path)
    stop = threading.Event()

    def monitor():
        process = None
        try:
            kernel = C.WinDLL('kernel32', use_last_error=True)
            kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
            kernel.OpenProcess.restype = W.HANDLE
            kernel.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
            kernel.CreateToolhelp32Snapshot.restype = W.HANDLE
            kernel.Thread32First.argtypes = [W.HANDLE, C.POINTER(ThreadEntry)]
            kernel.Thread32First.restype = W.BOOL
            kernel.Thread32Next.argtypes = [W.HANDLE, C.POINTER(ThreadEntry)]
            kernel.Thread32Next.restype = W.BOOL
            kernel.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
            kernel.OpenThread.restype = W.HANDLE
            kernel.GetThreadTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
            kernel.GetThreadTimes.restype = W.BOOL
            kernel.CloseHandle.argtypes = [W.HANDLE]
            kernel.CloseHandle.restype = W.BOOL
            description = getattr(kernel, 'GetThreadDescription', None)
            if description is not None:
                description.argtypes = [W.HANDLE, C.POINTER(C.c_void_p)]
                description.restype = W.LONG  # HRESULT
                kernel.LocalFree.argtypes = [C.c_void_p]
                kernel.LocalFree.restype = C.c_void_p

            with path.open('w', newline='', encoding='utf-8') as stream:
                writer = csv.writer(stream)
                writer.writerow(['monotonic_ns', 'elapsed_s', 'tid',
                                 'creation_filetime', 'name', 'cpu_total_s',
                                 'delta_cpu_s', 'core_equiv'])
                stream.flush()
                process = kernel.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                             False, pid)
                if not process:
                    _status(path, 'open_process_failed', f'pid={pid}', C.get_last_error())
                    return

                started_ns = time.perf_counter_ns()
                previous = {}  # (tid, creation FILETIME) -> (cpu seconds, sample ns)
                _status(path, 'ok')
                while not stop.is_set():
                    sample_ns = time.perf_counter_ns()
                    snapshot = kernel.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
                    if snapshot == INVALID_HANDLE_VALUE:
                        _status(path, 'snapshot_failed', f'pid={pid}', C.get_last_error())
                        return
                    current = {}
                    try:
                        entry = ThreadEntry()
                        entry.dwSize = C.sizeof(entry)
                        found = kernel.Thread32First(snapshot, C.byref(entry))
                        if not found and C.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                            _status(path, 'snapshot_failed', 'Thread32First',
                                    C.get_last_error())
                            return
                        while found:
                            if entry.th32OwnerProcessID == pid:
                                tid = entry.th32ThreadID
                                handle = kernel.OpenThread(
                                    THREAD_QUERY_LIMITED_INFORMATION, False, tid)
                                if handle:
                                    try:
                                        created, ended, system, user = (
                                            W.FILETIME() for _ in range(4))
                                        if kernel.GetThreadTimes(
                                                handle, C.byref(created), C.byref(ended),
                                                C.byref(system), C.byref(user)):
                                            birth = _filetime_value(created)
                                            total = (_filetime_value(system) +
                                                     _filetime_value(user)) / 1e7
                                            key = (tid, birth)
                                            prior = previous.get(key)
                                            delta = max(0.0, total - prior[0]) if prior else 0.0
                                            interval = (sample_ns - prior[1]) / 1e9 if prior else 0.0
                                            name = ''
                                            if description is not None:
                                                pointer = C.c_void_p()
                                                if description(handle, C.byref(pointer)) >= 0:
                                                    try:
                                                        if pointer.value:
                                                            name = C.wstring_at(pointer.value)
                                                    finally:
                                                        if pointer.value:
                                                            kernel.LocalFree(pointer)
                                            writer.writerow([
                                                sample_ns, f'{(sample_ns-started_ns)/1e9:.6f}',
                                                tid, birth, name, f'{total:.6f}',
                                                f'{delta:.6f}',
                                                f'{delta/interval:.6f}' if interval > 0 else '0.000000',
                                            ])
                                            current[key] = (total, sample_ns)
                                    finally:
                                        kernel.CloseHandle(handle)
                            entry.dwSize = C.sizeof(entry)
                            found = kernel.Thread32Next(snapshot, C.byref(entry))
                    finally:
                        kernel.CloseHandle(snapshot)
                    previous = current
                    stream.flush()
                    stop.wait(1)
        except (OSError, AttributeError, ValueError) as exc:
            _status(path, 'monitor_failed', str(exc))
        finally:
            if process:
                kernel.CloseHandle(process)

    thread = threading.Thread(target=monitor, name='iss-thread-metrics', daemon=True)
    thread.start()
    return stop, thread
