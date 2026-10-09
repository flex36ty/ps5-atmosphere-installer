"""Private loopback Samba upload integration; run as root, synthetic files only."""
import json, os, re, socket, subprocess, tempfile, time, urllib.request
from pathlib import Path
from test_image_metadata import exfat
assert os.geteuid()==0
def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
with tempfile.TemporaryDirectory(prefix='atmosphere-smb-export-') as tmp:
    base=Path(tmp);base.chmod(0o755);share=base/'share';share.mkdir(mode=0o777);share.chmod(0o777)
    local=base/'local';local.mkdir();state=base/'state';state.mkdir()
    game=local/'homebrew/Local Folder';(game/'sce_sys').mkdir(parents=True);(game/'data/empty').mkdir(parents=True)
    (game/'sce_sys/param.json').write_text(json.dumps({'titleId':'PPSA70002','localizedParameters':{'defaultLanguage':'en-US','en-US':{'titleName':'SMB Backup Test'}}}))
    (game/'data/file #1.bin').write_bytes(bytes(range(251))*10000)
    (local/'homebrew/Image.exfat').write_bytes(exfat())
    smb_port,http_port=port(),port();config=base/'smb.conf'
    config.write_text(f'''[global]
server role = standalone server
interfaces = 127.0.0.1
bind interfaces only = yes
smb ports = {smb_port}
server min protocol = SMB2
map to guest = Bad User
guest account = nobody
pid directory = {base}
lock directory = {base}
state directory = {base}
cache directory = {base}
private dir = {base}
log file = {base}/samba.log
[games]
path = {share}
guest ok = yes
read only = no
wide links = no
follow symlinks = no
''')
    with (base/'smbd.log').open('w') as log:samba=subprocess.Popen(['smbd','--foreground','--no-process-group','--configfile',str(config)],stdin=subprocess.PIPE,stdout=log,stderr=log,start_new_session=True)
    app=None;token='';origin=f'http://127.0.0.1:{http_port}'
    def request(path='/smb',body=None):
        headers={'Content-Type':'application/json','Origin':origin}
        if token:headers['Authorization']='Bearer '+token
        with urllib.request.urlopen(urllib.request.Request(origin+'/api/v1'+path,data=None if body is None else json.dumps(body).encode(),headers=headers),timeout=30) as r:return json.load(r)
    def action(operation,**kw):return request(body={'action':operation,**kw})
    def idle():
        for _ in range(1200):
            s=request()
            if not s['busy'] and not s['installed']['checking']:return s
            time.sleep(.025)
        raise AssertionError('worker timeout')
    try:
        time.sleep(.5)
        with (base/'app.log').open('w') as log:app=subprocess.Popen(['build/atmosphere-host','--state',str(state),'--storage',str(local),'--port',str(http_port)],stdout=log,stderr=log)
        for _ in range(300):
            text=(base/'app.log').read_text();match=re.search(r'Pairing code: (\d{6})',text)
            if match:
                try:token=request('/pair',{'code':match[1]})['token'];break
                except urllib.error.URLError:pass
            assert app.poll() is None,text;time.sleep(.025)
        assert token
        action('configure',protocol='smb',server='127.0.0.1',port=str(smb_port),share='games',folder='',username='',password='',name='SMB backup')
        action('scan');s=idle();games=s['installedGames'];assert len(games)==2 and not s['games'],s
        for g in games:
            action('export',localId=g['localId'],sourceId=s['activeSourceId'],confirmed=True);result=idle();assert result['job']['status']=='complete',result
            destination=share/result['job']['remotePath']
            assert destination==share/Path(g['path']).name,result
            source=Path(g['path'])
            if source.is_file():assert destination.read_bytes()==source.read_bytes()
            else:
                for p in source.rglob('*'):
                    if p.is_file():assert (destination/p.relative_to(source)).read_bytes()==p.read_bytes()
                    else:assert (destination/p.relative_to(source)).is_dir()
        import shutil
        existing=share/Path(games[0]['path']).name
        if existing.is_dir():shutil.rmtree(existing)
        else:existing.unlink()
        action('export',localId=games[0]['localId'],sourceId=s['activeSourceId'],confirmed=True,skipVerification=True)
        result=idle();assert result['job']['status']=='complete' and result['job']['verification']=='skipped',result
        assert result['job']['remotePath']==Path(games[0]['path']).name,result
        assert not list(share.glob('.atmosphere-upload-*'))
        action('scan');result=idle();assert all(not g.get('localOnly') for g in result['games']),result
        print('PASS: SMB guest writes, folders, empty directories, images, SHA-256 readback and server rediscovery')
    finally:
        if app:app.terminate();app.wait(timeout=20)
        samba.terminate();samba.wait(timeout=20)
