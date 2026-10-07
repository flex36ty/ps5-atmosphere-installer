"""Exercise deletion against a mock provider. Never removes any files."""
import http.server,json,os,re,socket,subprocess,tempfile,threading,time,urllib.request,urllib.error,uuid
from pathlib import Path
game=dict(title_id='PPSA12345',title_name='Fixture',path='/mnt/usb0/Fixture.ffpfsc',runtime_path='',image_type='pfsc',platform='ps5',source_type='image',installed=True,mounted=False,source_available=True,managed=True,can_manage_source=True)
deletes=[]
def job():
    return dict(status=0,job_id=1,operation='delete',state='complete',title_id='PPSA12345',source=game['path'],destination='',total_bytes=0,processed_bytes=0,result_status=0,result_error='',active=False,cancellable=False)
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path.endswith('/version'):out=dict(status=0,api_version=1,shadowmount_version='test',capabilities=['list_games','delete_game_source','storage_job_status'])
        elif self.path.endswith('/games'):out=dict(status=0,count=1,games=[game])
        elif self.path.endswith('/games/storage/status'):out=job()
        elif self.path.endswith('/games/delete'):deletes.append(body);out=job()
        else:out=dict(status=1,error='Unsupported fixture route')
        data=json.dumps(out).encode();self.send_response(200);self.end_headers();self.wfile.write(data)
with tempfile.TemporaryDirectory() as tmp,http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    root=Path(tmp);(root/'storage').mkdir();(root/'state').mkdir();token=''
    def request(path,body=None):
        headers={'Content-Type':'application/json','Origin':f'http://127.0.0.1:{port}'}
        if token:headers['Authorization']='Bearer '+token
        req=urllib.request.Request(f'http://127.0.0.1:{port}/api/v1'+path,data=None if body is None else json.dumps(body).encode(),headers=headers)
        with urllib.request.urlopen(req,timeout=10) as r:return json.load(r)
    with (root/'log').open('w') as log:
        app=subprocess.Popen(['build/atmosphere-host','--state',str(root/'state'),'--storage',str(root/'storage'),'--port',str(port)],stdout=log,stderr=log,env={**os.environ,'ATMOSPHERE_TEST_LIBRARY_PORT':str(server.server_port),'ATMOSPHERE_TEST_URL':f'http://127.0.0.1:{server.server_port}/fixture','ATMOSPHERE_TEST_SIZE':'1'})
    try:
        for _ in range(200):
            match=re.search(r'Pairing code: (\d{6})',(root/'log').read_text())
            if match:token=request('/pair',{'code':match[1]})['token'];break
            assert app.poll() is None,(root/'log').read_text();time.sleep(.05)
        for _ in range(200):
            library=request('/library')
            if library.get('status')=='ready':break
            time.sleep(.05)
        key=library['games'][0]['sourceKey']
        def action(**changes):
            body=dict(action='delete',titleId='PPSA12345',sourceKey=key,requestId=uuid.uuid4().hex,confirmed=True);body.update(changes)
            request('/library/actions',body)
            for _ in range(200):
                result=request('/library')['action']
                if result.get('id')==body['requestId'] and result.get('state') in ('error','complete'):return result
                time.sleep(.02)
            raise AssertionError(result)
        try:action(confirmed=False);raise AssertionError('Missing confirmation accepted')
        except urllib.error.HTTPError as e:assert e.code==400
        assert action(sourceKey='0'*64)['state']=='error' and not deletes
        game['mounted']=True
        assert action()['state']=='error' and not deletes
        game['mounted']=False;game['can_manage_source']=False
        assert action()['state']=='error' and not deletes
        game['can_manage_source']=True
        assert action()['state']=='complete'
        assert deletes==[{'title_id':'PPSA12345','confirm':True}],deletes
        print('PASS: delete confirmation, stale identity, mounted/unsupported rejection, exact provider request; no files deleted')
    finally:app.terminate();app.wait(timeout=10);server.shutdown()
