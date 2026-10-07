"""Resolve native GL imports against SDK export names; fail on every unknown."""
from pathlib import Path
import struct, json
base=Path(__file__).resolve().parents[2]
out=base/'atmosphere-0.4.1/native-launcher/obj/gl'
req={x.split()[-1] for x in (out/'imports.txt').read_text().splitlines()}
req-={'__eh_frame_start','__eh_frame_end','__eh_frame_hdr_start','__eh_frame_hdr_end'}
def exports(path):
 b=path.read_bytes(); u=lambda fmt,o:struct.unpack_from('<'+fmt,b,o)[0]
 sh=[u('Q',40)+i*u('H',58) for i in range(u('H',60))]; names=set()
 for p in sh:
  if u('I',p+4)!=11:continue
  st=sh[u('I',p+40)];strings=u('Q',st+24)
  for o in range(u('Q',p+24),u('Q',p+24)+u('Q',p+32),24):
   if not u('H',o+6) or not u('I',o):continue
   off=strings+u('I',o);names.add(b[off:b.index(b'\0',off)].decode())
 return names
mapping={}
paths=[base/'sdk/ps5-payload-sdk/target/lib'/f'{lib}.so' for lib in ('libSceLibcInternal','libkernel','libSceVideoOut','libSceSystemService')]
paths+=list((base/'packaging-tools/ps5-opengl/ps5-opengl-sdk-1.0.1/sdk/lib').glob('*.so'))
for path in paths:
 selected=req & exports(path);req-=selected
 if selected:
  (out/f'{path.stem}.txt').write_text('\n'.join(sorted(selected))+'\n');mapping[path.stem]=len(selected)
(out/'libraries.json').write_text(json.dumps(list(mapping)))
print(mapping);print('Unmapped:',sorted(req))
if req:raise SystemExit(1)
