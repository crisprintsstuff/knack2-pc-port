"""Low-frequency Windows process metrics, separate from per-frame renderer counters."""
import csv
import ctypes as C
from ctypes import wintypes as W
import os
import threading
import time

def start_metrics(pid,path):
    stop=threading.Event()
    def monitor():
        kernel=C.WinDLL('kernel32',use_last_error=True)
        class Memory(C.Structure):
            _fields_=[('cb',W.DWORD),('faults',W.DWORD)]+[(n,C.c_size_t) for n in ('peak_ws','ws','peak_paged','paged','peak_nonpaged','nonpaged','commit','peak_commit','private')]
        class IO(C.Structure):
            _fields_=[(n,C.c_ulonglong) for n in ('reads','writes','other','read_bytes','write_bytes','other_bytes')]
        kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];kernel.OpenProcess.restype=W.HANDLE
        kernel.GetProcessTimes.argtypes=[W.HANDLE]+[C.POINTER(C.c_ulonglong)]*4
        kernel.K32GetProcessMemoryInfo.argtypes=[W.HANDLE,C.POINTER(Memory),W.DWORD]
        kernel.GetProcessIoCounters.argtypes=[W.HANDLE,C.POINTER(IO)]
        kernel.CloseHandle.argtypes=[W.HANDLE]
        handle=kernel.OpenProcess(0x410,False,pid)
        if not handle:return
        try:
            with path.open('w',newline='',encoding='utf-8') as stream:
                writer=csv.writer(stream);writer.writerow(['elapsed_s','cpu_percent_machine','cpu_core_equivalents','working_set_mb','private_commit_mb','page_faults_total','read_bytes_total','write_bytes_total'])
                started=previous_time=time.perf_counter();previous_cpu=None
                while not stop.is_set():
                    created,ended,system,user=(C.c_ulonglong() for _ in range(4));memory=Memory();memory.cb=C.sizeof(memory);io=IO()
                    if not kernel.GetProcessTimes(handle,C.byref(created),C.byref(ended),C.byref(system),C.byref(user)):break
                    if not kernel.K32GetProcessMemoryInfo(handle,C.byref(memory),C.sizeof(memory)):break
                    kernel.GetProcessIoCounters(handle,C.byref(io));now=time.perf_counter();cpu=(system.value+user.value)/1e7
                    cores=(cpu-previous_cpu)/(now-previous_time) if previous_cpu is not None else 0
                    writer.writerow([round(now-started,3),round(cores*100/(os.cpu_count() or 1),3),round(cores,3),round(memory.ws/2**20,3),round(memory.private/2**20,3),memory.faults,io.read_bytes,io.write_bytes]);stream.flush()
                    previous_cpu=cpu;previous_time=now;stop.wait(1)
        finally:kernel.CloseHandle(handle)
    thread=threading.Thread(target=monitor,name='iss-process-metrics',daemon=True);thread.start()
    return stop,thread
