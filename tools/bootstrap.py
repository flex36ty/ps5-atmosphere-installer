"""Run with .venv/bin/python. Downloads only pinned build inputs, not game data."""
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
FILES = {
    'cJSON.c': '298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d',
    'cJSON.h': '25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee',
    'LICENSE': 'a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c',
}
for name, expected in FILES.items():
    destination = ROOT / '.deps/cjson' / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    data = destination.read_bytes() if destination.exists() else urllib.request.urlopen(f'https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/{name}', timeout=30).read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f'Checksum mismatch: {name}')
    destination.write_bytes(data)
    print(f'Verified cJSON v1.7.19/{name}')
ca = ROOT / '.deps/cacert.pem'
if not ca.exists():
    ca.write_bytes(urllib.request.urlopen('https://curl.se/ca/cacert.pem', timeout=30).read())
if b'-----BEGIN CERTIFICATE-----' not in ca.read_bytes():
    raise SystemExit('CA bundle is invalid')
print('CA SHA-256:', hashlib.sha256(ca.read_bytes()).hexdigest())
