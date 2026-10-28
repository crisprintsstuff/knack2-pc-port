"""Read-only shader registry snapshot from an explicitly selected probe run.

Does not suspend threads, patch memory, or change page protections. Double reads
reject observed changes but cannot rule out ABA races; this is mapping evidence,
not an atomic game-state snapshot. Stores metadata, never whole shader binaries.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import re
import struct
from datetime import datetime

K = C.WinDLL('kernel32', use_last_error=True)
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
K.ReadProcessMemory.restype = W.BOOL
K.CloseHandle.argtypes = [W.HANDLE]
K.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]

ROOT = Path(__file__).resolve().parents[1]
GLOBALS = {0x7D3CEE8: 'doubling_passes', 0x7D3CEF0: 'phase3_first',
           0x7D3CEF8: 'phase3_second', 0x7D3CF00: 'phase3_branch_a',
           0x7D3CF08: 'phase3_branch_b'}


class Reader:
    def __init__(self, pid):
        self.handle = K.OpenProcess(0x1010, False, pid)  # query limited + VM_READ only
        if not self.handle:
            raise C.WinError(C.get_last_error())
        name, size = C.create_unicode_buffer(32768), W.DWORD(32768)
        if not K.QueryFullProcessImageNameW(self.handle, 0, name, C.byref(size)):
            self.close()
            raise OSError('Cannot verify process image')
        if Path(name.value).resolve() != (ROOT / 'out/iss-probe.exe').resolve():
            self.close()
            raise ValueError('PID is not this workspace probe')

    def close(self):
        if self.handle:
            K.CloseHandle(self.handle)
            self.handle = None

    def read(self, address, size):
        if not 0x10000 <= address < 0x800000000000 or not 0 < size <= 131072:
            raise ValueError('Out-of-bounds diagnostic read')
        data, done = C.create_string_buffer(size), C.c_size_t()
        if not K.ReadProcessMemory(self.handle, address, data, size, C.byref(done)) or done.value != size:
            raise OSError(f'Unreadable {address:#x}+{size:#x}, error {C.get_last_error()}')
        return data.raw

    def qword(self, address):
        return struct.unpack('<Q', self.read(address, 8))[0]


def binary_info(reader, code):
    first = reader.read(code, 8)
    token, offset = struct.unpack('<II', first)
    if token != 0xBEEB03FF or offset > 0x10000:
        raise ValueError('Missing bounded BinaryInfo pointer token; no broad memory scan')
    address = code + (offset + 1) * 8
    info = reader.read(address, 28)
    if info[:7] != b'OrbShdr':
        raise ValueError('Missing OrbShdr signature at encoded offset')
    return {'binary_info_address': hex(address), 'binary_info_hex': info.hex(),
            'binary_info_hash': f'0x{struct.unpack_from("<Q", info, 16)[0]:016x}',
            'code_length': struct.unpack_from('<I', info, 8)[0] >> 8,
            'header_version': info[7]}


def snapshot(run):
    record = json.loads((run / 'run.json').read_text(encoding='utf-8'))
    stdout = (run / 'stdout.log').read_text(encoding='utf-8', errors='replace')
    match = re.search(r'Mapped eboot: base=(0x[0-9a-fA-F]+)', stdout)
    if not match:
        raise ValueError('No verified runtime base in this run log')
    base = int(match[1], 16)
    reader = Reader(record['pid'])
    result = {'time': datetime.now().astimezone().isoformat(), 'run': str(run.resolve()),
              'pid': record['pid'], 'base': hex(base), 'program': '/eboot.elf',
              'records': [], 'fixed_programs': [], 'errors': [],
              'limitations': ['Non-atomic snapshot; equal double reads do not exclude ABA.',
                             'Registry membership is not dispatch execution.',
                             'Unmapped/unreadable/protected shader code is reported, not bypassed.',
                             'Only headers with a bounded leading BinaryInfo token are decoded.']}
    try:
        # Known instruction signature prevents using these offsets on a different binary/base.
        if reader.read(base + 0x3F3EFF, 13) != bytes.fromhex('8b4004498b8fe8000000894108'):
            raise ValueError('Feedback instruction signature mismatch')
        result['feedback_signature_valid'] = True
        header = reader.read(base + 0xE17E90, 24)
        table, count, capacity = struct.unpack('<QqQ', header)
        # The count/capacity are actually 32-bit fields in 8-byte slots.
        count = struct.unpack_from('<i', header, 8)[0]
        capacity = struct.unpack_from('<i', header, 16)[0]
        if not 0 <= count <= capacity <= 16384:
            raise ValueError(f'Implausible registry count/capacity {count}/{capacity}')
        pointers = reader.read(table, count * 8) if count else b''
        result['registry'] = {'table': hex(table), 'count': count, 'capacity': capacity}
        for (ptr,) in struct.iter_unpack('<Q', pointers):
            row = {'resource': hex(ptr)}
            try:
                before = reader.read(ptr + 0x18, 32)
                asset, _, program, metadata = struct.unpack('<QQQQ', before)
                row.update(asset_id=f'0x{asset:016x}', program=hex(program), metadata=hex(metadata),
                           resource_type=struct.unpack('<I', reader.read(ptr + 0x10, 4))[0])
                if program:
                    row['program_backlink_valid'] = reader.qword(program + 8) == ptr
                if metadata:
                    meta = reader.read(metadata, 40)
                    code = (struct.unpack_from('<Q', meta, 8)[0] & ((1 << 40) - 1)) << 8
                    row.update(code_address=hex(code), metadata_hex=meta.hex(),
                               local_size=list(struct.unpack_from('<III', meta, 24)))
                    row.update(binary_info(reader, code))
                    row['metadata_stable'] = reader.read(metadata, 40) == meta
                    if program:
                        row['program_backlink_stable'] = reader.qword(program + 8) == ptr
                row['resource_stable'] = reader.read(ptr + 0x18, 32) == before
            except (OSError, ValueError) as error:
                row['error'] = str(error)
            result['records'].append(row)
        result['registry_stable'] = (reader.read(base + 0xE17E90, 24) == header and
                                     (reader.read(table, count * 8) if count else b'') == pointers)
        for offset, role in GLOBALS.items():
            row = {'global_offset': hex(offset), 'role': role}
            try:
                program = reader.qword(base + offset)
                row['program'] = hex(program)
                if program:
                    row['asset_id'] = f'0x{reader.qword(program):016x}'
                    row['resource'] = hex(reader.qword(program + 8))
                row['stable'] = reader.qword(base + offset) == program
            except (OSError, ValueError) as error:
                row['error'] = str(error)
            result['fixed_programs'].append(row)
        manager = base + 0x7D3EB10
        result['particle_arena'] = {'base': hex(reader.qword(manager)),
                                    'half_index': struct.unpack('<I', reader.read(manager + 0x28, 4))[0],
                                    'records_0x60': hex(reader.qword(manager + 0x30))}
        result['particle_lists'] = []
        particle_programs = set()
        for offset in (0x2B0, 0x2C0, 0x2D0, 0x2E0):
            head = reader.qword(manager + offset)
            listing = {'manager_offset': hex(offset), 'head': hex(head), 'objects': []}
            seen, current = set(), head
            while current and current not in seen and len(seen) < 2048:
                seen.add(current)
                row = {'object': hex(current)}
                try:
                    data = reader.read(current + 0xA0, 128)
                    row.update(program_a=hex(struct.unpack_from('<Q', data, 0)[0]),
                               program_b=hex(struct.unpack_from('<Q', data, 8)[0]),
                               feedback=hex(struct.unpack_from('<Q', data, 0x50)[0]),
                               payload_new=hex(struct.unpack_from('<Q', data, 0x58)[0]),
                               payload_old=hex(struct.unpack_from('<Q', data, 0x60)[0]))
                    row['stable'] = reader.read(current + 0xA0, 128) == data
                    particle_programs.update((int(row['program_a'], 16), int(row['program_b'], 16)))
                    current = struct.unpack_from('<Q', data, 0x20)[0]
                except (OSError, ValueError) as error:
                    row['error'] = str(error)
                    listing['objects'].append(row)
                    break
                listing['objects'].append(row)
            listing['head_stable'] = reader.qword(manager + offset) == head
            listing['stopped_at'] = hex(current)
            result['particle_lists'].append(listing)
        # The C0 phase lists do not cover every object. The CPU consumer walks
        # sparse vectors +260/+290, then node+30 and object+B0 (2929F0/3EDEF0).
        # Read the sparse arrays directly: never invoke the compacting iterator.
        result['particle_collections'] = []
        for offset in (0x260, 0x290):
            collection = {'manager_offset': hex(offset), 'objects': []}
            try:
                header = reader.read(manager + offset, 24)
                table = struct.unpack_from('<Q', header)[0]
                count = struct.unpack_from('<i', header, 8)[0]
                capacity = struct.unpack_from('<i', header, 16)[0]
                if not 0 <= count <= capacity <= 16384:
                    raise ValueError('Invalid particle collection capacity')
                pointers = reader.read(table, count * 8) if count else b''
                collection.update(table=hex(table), count=count, capacity=capacity)
                seen = set()
                for (node,) in struct.iter_unpack('<Q', pointers):
                    if not node:
                        continue
                    current = reader.qword(node + 0x30)
                    while current and current not in seen and len(seen) < 8192:
                        seen.add(current)
                        data = reader.read(current + 0xA0, 128)
                        a, b, next_object = struct.unpack_from('<QQQ', data)
                        particle_programs.update((a, b))
                        collection['objects'].append({'node': hex(node), 'object': hex(current),
                            'program_a': hex(a), 'program_b': hex(b),
                            'feedback': hex(struct.unpack_from('<Q', data, 0x50)[0]),
                            'payload_new': hex(struct.unpack_from('<Q', data, 0x58)[0]),
                            'payload_old': hex(struct.unpack_from('<Q', data, 0x60)[0]),
                            'stable': reader.read(current + 0xA0, 128) == data})
                        current = next_object
                collection['stable'] = (reader.read(manager + offset, 24) == header and
                                         (reader.read(table, count * 8) if count else b'') == pointers)
            except (OSError, ValueError) as error:
                collection['error'] = str(error)
            result['particle_collections'].append(collection)
        result['particle_programs'] = []
        for program in sorted(particle_programs - {0}):
            row = {'program': hex(program)}
            try:
                before = reader.read(program, 16)
                asset, resource = struct.unpack('<QQ', before)
                metadata = reader.qword(resource + 0x30)
                meta = reader.read(metadata, 40)
                code = (struct.unpack_from('<Q', meta, 8)[0] & ((1 << 40) - 1)) << 8
                row.update(asset_id=f'0x{asset:016x}', resource=hex(resource),
                           metadata=hex(metadata), metadata_hex=meta.hex(), code_address=hex(code),
                           local_size=list(struct.unpack_from('<III', meta, 24)))
                row.update(binary_info(reader, code))
                row['stable'] = (reader.read(program, 16) == before and
                                 reader.qword(resource + 0x30) == metadata and
                                 reader.read(metadata, 40) == meta)
            except (OSError, ValueError) as error:
                row['error'] = str(error)
            result['particle_programs'].append(row)
        # 3FED00 passes global 7F76E70 to 73CC00; that wrapper calls
        # (*(world+140)->vtable)[70]. Resolve only that known chain.
        result['world_cast_ray'] = {}
        try:
            world = reader.qword(base + 0x7F76E70)
            object_address = reader.qword(world + 0x140)
            vtable = reader.qword(object_address)
            target = reader.qword(vtable + 0x70)
            result['world_cast_ray'] = {'world': hex(world), 'object': hex(object_address),
                'vtable': hex(vtable), 'target': hex(target), 'target_offset': hex(target - base),
                'target_prefix': reader.read(target, 16).hex(),
                'stable': reader.qword(base + 0x7F76E70) == world and
                          reader.qword(world + 0x140) == object_address and
                          reader.qword(object_address) == vtable and
                          reader.qword(vtable + 0x70) == target}
        except (OSError, ValueError) as error:
            result['world_cast_ray']['error'] = str(error)
    finally:
        reader.close()
    result['decoded_headers'] = sum('binary_info_hash' in r for r in result['records'])
    result['collector_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = snapshot(args.run)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('pid', 'registry', 'registry_stable', 'decoded_headers')}))
