"""Brief read-only stack/register snapshots of this workspace's live iss-probe process."""
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import sys
import struct
K=C.WinDLL('kernel32',use_last_error=True)
class ThreadEntry(C.Structure):
    _fields_=[('size',W.DWORD),('usage',W.DWORD),('tid',W.DWORD),('pid',W.DWORD),('base',W.LONG),('delta',W.LONG),('flags',W.DWORD)]
K.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];K.CreateToolhelp32Snapshot.restype=W.HANDLE
K.Thread32First.argtypes=[W.HANDLE,C.POINTER(ThreadEntry)];K.Thread32Next.argtypes=K.Thread32First.argtypes
K.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];K.OpenProcess.restype=W.HANDLE
K.OpenThread.argtypes=[W.DWORD,W.BOOL,W.DWORD];K.OpenThread.restype=W.HANDLE
K.SuspendThread.argtypes=[W.HANDLE];K.ResumeThread.argtypes=[W.HANDLE]
K.GetThreadContext.argtypes=[W.HANDLE,C.c_void_p]
K.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)]
K.CloseHandle.argtypes=[W.HANDLE]
K.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)]
pid=int(sys.argv[1]);process=K.OpenProcess(0x410,False,pid)
if not process:raise OSError(C.get_last_error())
path=C.create_unicode_buffer(2048);length=W.DWORD(2048)
if not K.QueryFullProcessImageNameW(process,0,path,C.byref(length)):raise OSError(C.get_last_error())
expected=Path(__file__).resolve().parents[1]/'out/iss-probe.exe'
if Path(path.value).resolve()!=expected.resolve():raise RuntimeError('Refusing unrelated process')
def read(address,size):
    b=C.create_string_buffer(size);n=C.c_size_t()
    K.ReadProcessMemory(process,C.c_void_p(address),b,size,C.byref(n));return b.raw[:n.value]
snapshot=K.CreateToolhelp32Snapshot(4,0);entry=ThreadEntry();entry.size=C.sizeof(entry);ok=K.Thread32First(snapshot,C.byref(entry));rows=[]
while ok:
    if entry.pid==pid:
        thread=K.OpenThread(0x48|2,False,entry.tid)
        if thread:
            buf=C.create_string_buffer(1250);address=(C.addressof(buf)+15)&~15
            C.c_uint32.from_address(address+48).value=0x100003
            suspended=K.SuspendThread(thread)!=0xffffffff
            try:
                if suspended and K.GetThreadContext(thread,address):
                    raw=C.string_at(address,1232);rip=struct.unpack_from('<Q',raw,248)[0];rsp=struct.unpack_from('<Q',raw,152)[0];rbp=struct.unpack_from('<Q',raw,160)[0]
                    stack=read(rsp,2048);words=struct.unpack('<'+'Q'*(len(stack)//8),stack[:len(stack)//8*8])
                    guest=[hex(v-0x10000000) for v in words if 0x10000000<=v<0x18240000]
                    rows.append({'tid':entry.tid,'rip':hex(rip),'rsp':hex(rsp),'rbp':hex(rbp),'guest_stack_candidates':guest[:25]})
            finally:
                if suspended:K.ResumeThread(thread)
                K.CloseHandle(thread)
    ok=K.Thread32Next(snapshot,C.byref(entry))
K.CloseHandle(snapshot);K.CloseHandle(process)
out=expected.parent/'thread-snapshot.json';out.write_text(json.dumps(rows,indent=2),encoding='utf-8');print(json.dumps(rows,indent=2))
