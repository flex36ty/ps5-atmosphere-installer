"""Exercise local-only discovery and FTP backups using synthetic localhost data."""
import json, os, re, socket, subprocess, tempfile, threading, time, urllib.request, urllib.error
from pathlib import Path
import ftp_adapter_test as ftp
from test_image_metadata import exfat, ICON

ftp.FILES.clear();ftp.DIRECTORIES.add('/games')
with tempfile.TemporaryDirectory(prefix='atmosphere-export-') as tmp, ftp.Server(('127.0.0.1',0),ftp.Handler) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    root=Path(tmp);local=root/'local';state_dir=root/'state';local.mkdir();state_dir.mkdir()
    folder=local/'homebrew/Local Game';(folder/'sce_sys').mkdir(parents=True);(folder/'data/empty').mkdir(parents=True)
    (folder/'sce_sys/param.json').write_text(json.dumps({'titleId':'PPSA70001','contentId':'UP0006-PPSA70001_00-EASPORTSFC2027BG','requiredSystemSoftwareVersion':'0x1360000000000000','localizedParameters':{'defaultLanguage':'en-US','en-US':{'titleName':'Local Only Game'}}}))
    (folder/'sce_sys/icon0.png').write_bytes(ICON)
    (folder/'eboot.bin').write_bytes(b'synthetic executable')
    (folder/'data/test #1.bin').write_bytes(bytes(range(251))*12000)
    (local/'homebrew/Local Image.exfat').write_bytes(exfat(backport=True))
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    origin=f'http://127.0.0.1:{port}';token=''
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
    with (root/'log').open('w') as log:app=subprocess.Popen(['build/atmosphere-host','--state',str(state_dir),'--storage',str(local),'--port',str(port)],stdout=log,stderr=log)
    try:
        for _ in range(300):
            text=(root/'log').read_text();match=re.search(r'Pairing code: (\d{6})',text)
            if match:token=request('/pair',{'code':match[1]})['token'];break
            assert app.poll() is None,text;time.sleep(.025)
        assert token
        action('configure',protocol='ftp',server='127.0.0.1',port=str(server.server_address[1]),folder='games',share='',username='',password='',name='Backup FTP')
        action('scan');s=idle();source=s['activeSourceId']
        games=s['installedGames'];assert len(games)==2 and not s['games'],s
        game=next(g for g in games if g['titleId']=='PPSA70001');assert game['title']=='Local Only Game' and game['canExport'],game
        def check_details(games):
            for title,fw,region,backported in [('PPSA70001','13.60','US',False),('PPSA12345','12.60','EUR',True)]:
                g=next(g for g in games if g['titleId']==title)
                assert (g.get('minimumFirmware'),g.get('region'),g.get('backportFiles'))==(fw,region,backported),g
                assert Path(g['cover']).parent==state_dir and Path(g['cover']).read_bytes()==ICON,g
        check_details(games)
        original={str(p.relative_to(local)):p.read_bytes() for p in local.rglob('*') if p.is_file()}
        def export(g=game):action('export',localId=g['localId'],sourceId=source,confirmed=True);return idle()
        try:action('export',localId=game['localId'],sourceId=source);raise AssertionError('No confirmation required')
        except urllib.error.HTTPError as e:assert e.code==400
        denied=export();assert denied['job']['status']=='error' and 'not writable' in denied['job']['error'],denied
        assert not ftp.FILES and ftp.DIRECTORIES=={'/games'}
        ftp.WRITE_ENABLED=True
        result=export();assert result['job']['status']=='complete',result
        remote='/'+result['job']['remotePath'];assert remote=='/games/Local Game',result
        for p in folder.rglob('*'):
            if p.is_file():assert ftp.FILES[remote+'/'+str(p.relative_to(folder))]==p.read_bytes()
        assert remote+'/data/empty' in ftp.DIRECTORIES
        image=next(g for g in games if g['titleId']=='PPSA12345');result=export(image)
        assert result['job']['status']=='complete',result
        assert result['job']['remotePath']=='games/Local Image.exfat',result
        assert ftp.FILES['/'+result['job']['remotePath']]==(local/'homebrew/Local Image.exfat').read_bytes()
        before=dict(ftp.FILES);result=export();assert result['job']['status']=='error' and 'already exists' in result['job']['error'] and before==ftp.FILES,result
        assert not any('.atmosphere-upload-' in d for d in ftp.DIRECTORIES)
        def remove_fixture_copy():
            for p in list(ftp.FILES):
                if p.startswith(remote+'/'):del ftp.FILES[p]
            ftp.DIRECTORIES.difference_update({p for p in ftp.DIRECTORIES if p==remote or p.startswith(remote+'/')})
        remove_fixture_copy()
        print('PASS: direct original folder/file names, staging cleanup, existing-name rejection and unchanged server content')
        ftp.CORRUPT_UPLOADS=True;result=export();ftp.CORRUPT_UPLOADS=False
        assert result['job']['status']=='error' and '.atmosphere-upload-' in result['job']['remotePath'],result
        ftp.RETR_PATHS.clear()
        action('export',localId=game['localId'],sourceId=source,confirmed=True,skipVerification=True);result=idle()
        assert result['job']['status']=='complete' and result['job']['verification']=='skipped',result
        assert result['job']['remotePath']=='games/Local Game' and not ftp.RETR_PATHS,(result,ftp.RETR_PATHS)
        remove_fixture_copy()
        print('PASS: skip-verification publishes backup without remote content readback and reports verification skipped')
        (folder/'data/link').symlink_to('/etc/passwd');before=set(ftp.DIRECTORIES);result=export();(folder/'data/link').unlink()
        assert result['job']['status']=='error' and set(ftp.DIRECTORIES)==before,result
        (folder/'large.bin').write_bytes(bytes(range(251))*50000);ftp.DELAY=.01
        action('export',localId=game['localId'],sourceId=source,confirmed=True)
        for _ in range(200):
            current=request()
            if current['job'].get('received',0)>100000:break
            time.sleep(.01)
        action('cancel');result=idle();ftp.DELAY=0;(folder/'large.bin').unlink()
        assert result['job']['status']=='cancelled' and '.atmosphere-upload-' in result['job']['remotePath'],result
        try:action('resume');raise AssertionError('Upload incorrectly accepted download resume')
        except urllib.error.HTTPError as e:assert e.code==409
        assert original=={str(p.relative_to(local)):p.read_bytes() for p in local.rglob('*') if p.is_file()}
        print('PASS: corruption, symlinks, cancellation, upload resume rejection and unchanged local originals')
        result=export();assert result['job']['status']=='complete',result
        action('scan');s=idle();assert not any(g.get('localOnly') and g['titleId']=='PPSA70001' for g in s['games']),s
        assert any(g.get('sourceProtocol')=='ftp' and g['titleId']=='PPSA70001' for g in s['games'])
        assert len(s['installedGames'])==2,s
        action('deactivateSource',sourceId=source);action('refreshInstalled');s=idle()
        assert len(s['installedGames'])==2 and not s['games'],s
        check_details(s['installedGames'])
        print('PASS: independent Installed Games catalog remains visible with inactive servers; local-only refresh works')
        app.terminate();app.wait(timeout=20)
        saved=json.loads((state_dir/'smb-state.json').read_text());saved['job']['status']='copying'
        partial=saved['job']['remotePath'];(state_dir/'smb-state.json').write_text(json.dumps(saved))
        with (root/'restart.log').open('w') as log:app=subprocess.Popen(['build/atmosphere-host','--state',str(state_dir),'--storage',str(local),'--port',str(port)],stdout=log,stderr=log)
        for _ in range(300):
            try:restored=request();break
            except (OSError,urllib.error.URLError):time.sleep(.025)
        else:raise AssertionError('restart timeout')
        assert restored['job']['status']=='error' and restored['job']['remotePath']==partial and 'interrupted' in restored['job']['phase'],restored
        print('PASS: interrupted upload is restored as an error with its partial path; no automatic upload resume')
    finally:app.terminate();app.wait(timeout=20);server.shutdown()
