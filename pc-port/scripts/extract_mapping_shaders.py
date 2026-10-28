"""Extract the two investigated GCN shaders from the known package, read-only.

Validates package identity, OrbShdr hash and the leading pointer token. Does not
claim to parse the complete XPPS index; extracts only the verified shader spans.
"""
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'evidence/mapping'


if __name__ == '__main__':
    manifest = json.loads((DEST / 'all-shaders-package-summary.json').read_text())
    package = Path(manifest['file'])
    data = package.read_bytes()
    if hashlib.sha256(data).hexdigest() != manifest['sha256']:
        raise ValueError('Shader package identity changed')
    for shader_hash, offset in [(0xE902349D, 0x77DF0), (0x03E4B72E, 0x87DA8)]:
        if data[offset:offset + 7] != b'OrbShdr' or struct.unpack_from('<Q', data, offset + 16)[0] != shader_hash:
            raise ValueError('Shader identity mismatch')
        size = struct.unpack_from('<I', data, offset + 8)[0] >> 8
        starts = [p for p in range(max(0, offset - size - 256), offset, 4)
                  if data[p:p + 4] == bytes.fromhex('ff03ebbe') and
                  p + (struct.unpack_from('<I', data, p + 4)[0] + 1) * 8 == offset]
        if len(starts) != 1 or not 0 < size <= 65536 or starts[0] + size > offset:
            raise ValueError('Ambiguous/out-of-range code span')
        start = starts[0]
        raw = data[start:start + size]
        name = f'{shader_hash:08x}'
        binary = DEST / (name + '.gcn.bin')
        binary.write_bytes(raw)
        result = subprocess.run([str(ROOT / 'out/gcn-decode-build/gcn-decode.exe'), str(binary)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(f'Decoder failed: {result.returncode}: {result.stderr}')
        (DEST / (name + '.gcn.txt')).write_text(result.stdout, encoding='utf-8')
        info = {'package': str(package), 'package_sha256': manifest['sha256'],
                'header': hex(offset), 'code_start': hex(start), 'reported_code_bytes': size,
                'binary_info_hash': name, 'token_distance_valid': True,
                'code_sha256': hashlib.sha256(raw).hexdigest(), 'decoder_exit': result.returncode,
                'decoder': 'local shadPS4 decoder via gcn-decode.exe',
                'scope': 'Individual verified shader spans, not a complete package parser'}
        (DEST / (name + '.package-location.json')).write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
        print(name, 'PASS', size, 'bytes')
