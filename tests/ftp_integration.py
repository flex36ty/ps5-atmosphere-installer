import json,re,tempfile,subprocess,threading,time,urllib.request,urllib.error,hashlib,socket
from pathlib import Path
from ftp_adapter_test import Server,Handler,FILES
import ftp_adapter_test
from test_image_metadata import exfat
FILES['/games/embedded.exfat']=bytes(exfat(backport=True))
FILES['/games/Folder Game/eboot.bin']=b'test executable'
FILES['/games/Folder Game/sce_sys/param.json']=json.dumps({'titleId':'PPSA00001','localizedParameters':{'en-US':{'titleName':'FTP Folder Test'},'defaultLanguage':'en-US'}}).encode()
FILES['/games/Folder Game/content/data.bin']=bytes(range(251))*10000
with tempfile.TemporaryDirectory() as tmp, Server(('127.0.0.1',0),Handler) as ftp:
    threading.Thread(target=ftp.serve_forever,daemon=True).start()
    root=Path(tmp);(root/'state').mkdir();(root/'dest').mkdir()
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    token='';url=f'http://127.0.0.1:{port}/api/v1'
    def request(path='/smb',body=None):
        headers={'Content-Type':'application/json','Origin':f'http://127.0.0.1:{port}'}
        if token:headers['Authorization']='Bearer '+token
        with urllib.request.urlopen(urllib.request.Request(url+path,data=None if body is None else json.dumps(body).encode(),headers=headers),timeout=30) as res:return json.load(res)
    def action(name,**kw):return request(body={'action':name,**kw})
    def idle():
        for _ in range(600):
            state=request()
            if not state['busy']:return state
            time.sleep(.05)
        raise AssertionError('worker timeout')
    with (root/'log').open('w') as log:
        app=subprocess.Popen(['build/atmosphere-host','--state',str(root/'state'),'--storage',str(root/'dest'),'--port',str(port)],stdout=log,stderr=log)
    try:
        for _ in range(200):
            text=(root/'log').read_text();match=re.search(r'Pairing code: (\d{6})',text)
            if match:token=request('/pair',{'code':match[1]})['token'];break
            assert app.poll() is None,text;time.sleep(.05)
        assert token
        legacy=dict(server='127.0.0.1',share='games',folder='',username='test',domain='',destinationFolder='homebrew')
        action('configure',**legacy,password='session-password')
        action('configure',**legacy,protocol='smb',port='')
        assert request()['hasPassword'], 'Editing a legacy SMB server cleared its password'
        action('configure',protocol='ftp',server='127.0.0.1',port=str(ftp.server_address[1]),share='',folder='games',username='',password='',domain='',destinationFolder='homebrew')
        action('scan');state=idle();assert len(state['games'])==3,state
        embedded=next(g for g in state['games'] if g['filename']=='embedded.exfat');assert embedded.get('cover'),embedded
        assert embedded.get('minimumFirmware')=='12.60',embedded
        assert embedded.get('backportFiles') is True,embedded
        assert embedded.get('region')=='EUR',embedded
        folder_meta=next(g for g in state['games'] if g['filename']=='Folder Game')
        assert not folder_meta.get('minimumFirmware') and not folder_meta.get('backportFiles'),folder_meta
        assert not folder_meta.get('region'),folder_meta
        action('scan');cached=idle()
        cached_image=next(g for g in cached['games'] if g['filename']=='embedded.exfat')
        assert cached_image.get('region')=='EUR',cached_image
        assert cached_image.get('metadataCached') and cached_image.get('minimumFirmware')=='12.60' and cached_image.get('backportFiles'),cached_image
        state=cached
        for game in state['games']:
            if game['filename']=='game #1.exfat':ftp_adapter_test.DELAY=.01
            action('copy',gameId=game['id'],storageId='desktop')
            if game['filename']=='game #1.exfat':
                time.sleep(.2);action('pause');paused=idle();assert paused['job']['status']=='paused',paused
                (root/'dest/.atmosphere-smb-staging').rename(root/'dest/.orbit-smb-staging')
                ftp_adapter_test.DELAY=0;action('resume')
            result=idle();assert result['job']['status']=='complete',result;assert result['job']['verification']=='sha256'
            profile=result['job']['transferProfile']
            assert profile['result']==0 and profile['bytes']>0 and profile['remoteSeconds']>=0,profile
            saved=json.loads((root/'state/smb-state.json').read_text())
            assert saved['job']['transferProfile']==profile,'Transfer timings were not persisted'
            if game['filename']=='game #1.exfat':
                assert (root/'dest/.atmosphere-smb-staging').is_dir()
                assert not (root/'dest/.orbit-smb-staging').exists()
        for remote,data in FILES.items():
            local=root/'dest/homebrew'/remote.removeprefix('/games/')
            assert local.read_bytes()==data,remote
        (root/'dest/homebrew/Folder Game').rename(root/'dest/homebrew/Renamed Folder')
        action('scan');state=idle()
        for _ in range(300):
            local=request()['installed']
            if not local['checking']:break
            time.sleep(.02)
        assert any(g['titleId']=='PPSA00001' and g['path'].endswith('/Renamed Folder') for g in local['games']),local
        assert any(g['titleId']=='PPSA12345' for g in local['games']),local
        folder=next(g for g in state['games'] if g['filename']=='Folder Game')
        try:action('copy',gameId=folder['id'],storageId='desktop');raise AssertionError('Duplicate title was accepted without confirmation')
        except urllib.error.HTTPError as error:assert error.code==409
        action('copy',gameId=folder['id'],storageId='desktop',allowDuplicate=True)
        assert idle()['job']['status']=='complete'
        print('PASS: installed image/folder title IDs, renamed folder matching, duplicate warning and explicit override')
        print('PASS: FTP embedded metadata/cover, image and recursive folder scan, pause/resume, copy, SHA-256 verification and destination bytes')
    finally:
        app.terminate();app.wait(timeout=10);ftp.shutdown()
