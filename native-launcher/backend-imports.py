"""Generate explicit native PRX imports from the installed SDK export lists.
No Linux NativeAOT compatibility shims may be linked into this FreeBSD ABI module.
"""
from pathlib import Path
import struct, json
base=Path(__file__).resolve().parents[2]
out=base/'atmosphere-0.4.1/native-launcher/obj/backend'
req={x.split()[-1] for x in (out/'imports.txt').read_text().splitlines()}
def exports(path):
 b=path.read_bytes(); u=lambda fmt,o:struct.unpack_from('<'+fmt,b,o)[0]
 sh=[u('Q',40)+i*u('H',58) for i in range(u('H',60))]; names=set()
 for p in sh:
  if u('I',p+4)!=11: continue
  st=sh[u('I',p+40)]; strings=u('Q',st+24)
  for o in range(u('Q',p+24),u('Q',p+24)+u('Q',p+32),24):
   if not u('H',o+6) or not u('I',o):continue
   off=strings+u('I',o); names.add(b[off:b.index(b'\0',off)].decode())
 return names
mapping={}
for lib in ('libSceLibcInternal','libkernel','libSceNet'):
 selected=req & exports(base/f'sdk/ps5-payload-sdk/target/lib/{lib}.so')
 req-=selected
 (out/f'{lib}.txt').write_text('\n'.join(sorted(selected))+'\n')
 mapping[lib]=len(selected)
print(json.dumps(mapping)); print('Unmapped:', sorted(req))
if req: raise SystemExit(1)
