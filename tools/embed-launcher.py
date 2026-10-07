import json
from pathlib import Path
root = Path(__file__).resolve().parent.parent
out = root/'build/generated/launcher.h'
out.parent.mkdir(parents=True,exist_ok=True)
port = json.loads((root/'config/network.json').read_text())['httpPort']
if type(port) is not int or not 1024 <= port <= 65535:
    raise SystemExit('httpPort must be an integer between 1024 and 65535')
manifest = json.loads((root/'launcher/sce_sys/param.json').read_text())
manifest['deeplinkUri'] = f'http://127.0.0.1:{port}/'
parts=[]
for name,data in [('launcher_manifest', (json.dumps(manifest, indent=2)+'\n').encode()), ('launcher_icon', (root/'launcher/sce_sys/icon0.png').read_bytes())]:
    parts.append('static const unsigned char '+name+'[] = {'+','.join(str(b) for b in data)+'};\n')
out.write_text(''.join(parts))
(out.parent/'network.h').write_text(f'#ifndef ATMOSPHERE_NETWORK_H\n#define ATMOSPHERE_NETWORK_H\n#define ATMOSPHERE_HTTP_PORT {port}\n#endif\n')
