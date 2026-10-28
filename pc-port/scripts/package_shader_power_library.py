"""Extend a private immutable library with exact recipes from one cache lineage.

Never merge conflicting permutation indices. Never copy driver binaries. Hash
every file and preserve the established library's bytes without replacing it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
sha=lambda data:hashlib.sha256(data).hexdigest()
p=argparse.ArgumentParser();p.add_argument('--capture',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
source=ROOT/'out/pipeline-library/CUSA00004-packed'
m=json.loads((source/'manifest.json').read_text(encoding='utf-8'))
packed=(source/'recipes.bin').read_bytes();assert sha(packed)==m['pack']['sha256']
profile=(source/'profile.bin').read_bytes();assert sha(profile)==m['profile']['sha256']
assert (a.capture/'profile.bin').read_bytes()==profile
files={}
for item in m['assets']:
    data=packed[item['offset']:item['offset']+item['size']]
    assert len(data)==item['size'] and sha(data)==item['sha256']
    files[item['filename']]=data
added=[]
for f in sorted(a.capture.iterdir()):
    if f.suffix not in ('.key','.meta','.spv'):continue
    assert f.is_file() and not f.is_symlink()
    data=f.read_bytes()
    if f.name in files:
        assert files[f.name]==data,('lineage conflict',f.name)
    else:
        files[f.name]=data;added.append(f.name)
# Verify every shader and key points to a supplied, consistently numbered variant.
variants={}
for name,data in files.items():
    if not name.endswith('.spv'):continue
    assert struct.unpack_from('<I',data)[0]==0x07230203 and len(data)%4==0
    shader=int(name[2:18],16);perm=int(name.split('_')[1].split('.')[0])
    identity=(shader ^ (perm+0x9e3779b9+(shader<<12)+(shader>>4)))&((1<<64)-1)
    assert identity not in variants
    variants[identity]=name
    meta=files[f'0x{identity:016x}.meta']
    assert struct.unpack_from('<Q',meta,8)[0]==identity
    assert struct.unpack_from('<Q',meta,16)[0]==perm
for name,data in files.items():
    if name.endswith('.key'):
        version,compute=struct.unpack_from('<II',data);assert version==3
        stages=struct.unpack_from('<Q' if compute else '<6Q',data,8)
        assert all(h==0 or h in variants for h in stages),name
out=a.output.resolve();assert out.is_relative_to((ROOT/'out/experimental').resolve())
out.mkdir(parents=True,exist_ok=False)
assets=[];blob=bytearray()
for name,data in sorted(files.items()):
    assets.append(dict(filename=name,offset=len(blob),size=len(data),sha256=sha(data)))
    blob.extend(data)
(out/'recipes.bin').write_bytes(blob);(out/'profile.bin').write_bytes(profile)
manifest=dict(schema=1,profile=dict(filename='profile.bin',size=len(profile),sha256=sha(profile)),
              pack=dict(filename='recipes.bin',size=len(blob),sha256=sha(blob)),assets=assets)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
selection=dict(schema='iss.shader-powers.v1',source=str(a.capture.resolve()),
               base_manifest_sha256=sha((source/'manifest.json').read_bytes()),added=added,
               added_compute=sum(n.startswith('c_') and n.endswith('.key') for n in added),
               added_graphics=sum(n.startswith('g_') and n.endswith('.key') for n in added),
               limitations=['Only exact observed variants; unseen shaders still compile.',
                            'No driver binary: portable only to a matching Shader::Profile.',
                            'No gameplay behavior, SPIR-V or pipeline state modified.'])
(out/'selection.json').write_text(json.dumps(selection,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in selection.items() if k!='added'},indent=2))
print('assets',len(assets),'bytes',len(blob),'sha256',sha(blob))
