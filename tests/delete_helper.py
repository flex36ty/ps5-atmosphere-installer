"""Payload lifecycle tests with a mock API and disposable sleeping parent."""
import http.server,json,subprocess,tempfile,threading,time,uuid
from pathlib import Path
calls=[];changed=False
class API(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        data=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        calls.append((self.path,data))
        if self.path.endswith('/games'):
            result={'status':0,'games':[dict(title_id='PPSA12345',path='/mnt/usb0/changed.ffpfsc' if changed else '/mnt/usb0/game.ffpfsc',can_manage_source=True,source_available=True)]}
        else:result=dict(status=0,job_id=41,active=False,result_status=0)
        self.send_response(200);self.end_headers();self.wfile.write(json.dumps(result).encode())
with http.server.ThreadingHTTPServer(('127.0.0.1',0),API) as server,tempfile.TemporaryDirectory() as tmp:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    for changed in (False,True):
        calls.clear();root=Path(tmp)/str(changed)/'PPSA99005'/'atmosphere-state';root.mkdir(parents=True)
        parent=subprocess.Popen(['sleep','60'])
        request=dict(requestId=uuid.uuid4().hex,titleId='PPSA12345',path='/mnt/usb0/game.ffpfsc',statePath=str(root),pid=parent.pid,port=server.server_port,deadline=time.time()+90)
        config=root/'request.json';config.write_text(json.dumps(request))
        helper=subprocess.Popen(['build/delete-helper-test',str(config)])
        try:
            for _ in range(100):
                if (root/'delete-ready.json').exists():break
                assert helper.poll() is None;time.sleep(.05)
            assert (root/'delete-ready.json').exists()
            (root/'delete-commit.json').write_text(json.dumps(request))
            time.sleep(1.2);assert not calls,'Contacted deletion API before parent exit'
            parent.terminate();parent.wait()
            helper.wait(timeout=20)
            result=json.loads((root/'delete-result.json').read_text())
            deletes=[body for route,body in calls if route.endswith('/games/delete')]
            if changed:assert result['state']=='error' and not deletes,result
            else:assert result['state']=='complete' and deletes==[{'title_id':'PPSA12345','confirm':True}],(result,calls)
        finally:
            if parent.poll() is None:parent.terminate();parent.wait()
            if helper.poll() is None:helper.terminate();helper.wait()
    server.shutdown()
print('PASS: acknowledged handoff, waits for parent exit, exact one-shot delete, changed-path rejection; no files deleted')
