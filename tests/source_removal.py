"""Local-only source removal and persistence checks; no SMB connection required."""
import json, re, socket, subprocess, tempfile, time, urllib.request, urllib.error
from pathlib import Path

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    base = Path(tmp)
    state, storage = base/'state', base/'storage'
    state.mkdir(); storage.mkdir()
    sentinel = storage/'keep-game.bin'; sentinel.write_bytes(b'keep this game')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    url = f'http://127.0.0.1:{port}'
    token = ''
    def request(action=None, expected=200, **fields):
        body = None if action is None else {'action': action, **fields}
        req = urllib.request.Request(url+'/api/v1/smb', data=None if body is None else json.dumps(body).encode(), headers={'Content-Type':'application/json', 'Origin':url, 'Authorization':'Bearer '+token})
        try:
            with urllib.request.urlopen(req, timeout=5) as response: code, data = response.status, json.load(response)
        except urllib.error.HTTPError as error: code, data = error.code, json.load(error)
        assert code == expected, (code, data)
        return data
    def start():
        global token
        log = base/'app.log'
        with log.open('w') as output:
            process = subprocess.Popen([str(root/'build/atmosphere-host'), '--state', str(state), '--storage', str(storage), '--port', str(port)], stdout=output, stderr=subprocess.STDOUT)
        for _ in range(200):
            match = re.search(r'Pairing code: (\d{6})', log.read_text())
            if match:
                req = urllib.request.Request(url+'/api/v1/pair', data=json.dumps({'code':match[1]}).encode(), headers={'Content-Type':'application/json', 'Origin':url})
                try:
                    with urllib.request.urlopen(req) as response: token=json.load(response)['token']
                    return process
                except OSError: pass
            assert process.poll() is None, log.read_text()
            time.sleep(.05)
        process.terminate(); process.wait(); raise AssertionError(log.read_text())
    app = start()
    try:
        first = request()['activeSourceId']
        request('addSource', 202); second = request()['activeSourceId']
        request('deactivateSource',202,sourceId=second)
        assert request()['activeSourceId']=='' and len(request()['sources'])==2
        app.terminate();app.wait(timeout=10);app=start()
        assert request()['activeSourceId']=='' and len(request()['sources'])==2
        request('selectSource',202,sourceId=second)
        assert request()['activeSourceId']==second
        request('deleteSource', 404, sourceId='missing')
        request('deleteSource', 202, sourceId=first)
        assert request()['activeSourceId'] == second
        request('addSource', 202)
        request('deleteSource', 202, sourceId=request()['activeSourceId'])
        assert request()['activeSourceId'] == second
        request('deleteSource', 202, sourceId=second)
        assert request()['sources'] == [] and request()['games'] == []
        app.terminate(); app.wait(); app = start()
        assert request()['sources'] == []
        request('addSource', 202)
        assert len(request()['sources']) == 1
        assert sentinel.read_bytes() == b'keep this game'
        print('PASS: inactive/active/last source deletion, unknown ID, restart persistence, re-add, files preserved')
    finally:
        app.terminate(); app.wait()
