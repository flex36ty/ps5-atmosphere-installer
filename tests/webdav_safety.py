import http.server, threading, subprocess, base64
MODE='ok'
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_PROPFIND(self):
        if MODE=='auth' or self.headers.get('Authorization')!='Basic '+base64.b64encode(b'test:secret').decode():self.send_error(401);return
        href='/games/a%20%26%20b.ffpfsc'
        if MODE=='absolute':href=f'http://127.0.0.1:{self.server.server_port}/games/a%20%26%20b.ffpfsc'
        if MODE=='entity':href='/games/a%20&amp;%20b.ffpfsc'
        if MODE=='traversal':href='/games/%2e%2e'
        if MODE=='outside':href='/private/a.ffpfsc'
        if MODE=='host':href='http://other.invalid/games/a.ffpfsc'
        if MODE=='nul':href='/games/a%00b'
        xml=f'<multistatus xmlns="DAV:"><response><href>{href}</href><propstat><prop><getcontentlength>1024</getcontentlength><resourcetype/></prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>'
        if MODE=='malformed':xml=xml[:-20]
        if MODE=='doctype':xml='<!DOCTYPE x [<!ENTITY z SYSTEM "file:///etc/passwd">]>'+xml
        data=xml.encode();self.send_response(207);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_GET(self):
        assert self.path=='/games/a%20%26%20b.ffpfsc'
        assert self.headers['Range']=='bytes=123-186'
        data=bytes(i%251 for i in range(123,187))
        self.send_response(200 if MODE=='ignored' else 206)
        if MODE!='missing':self.send_header('Content-Range','bytes 0-63/1024' if MODE=='wrong' else 'bytes 123-186/1024')
        if MODE=='short':data=data[:32]
        self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
with http.server.ThreadingHTTPServer(('127.0.0.1',0),H) as server:
    threading.Thread(target=server.serve_forever,daemon=True).start()
    for MODE in ['ok','absolute','entity','auth','traversal','outside','host','nul','malformed','doctype','ignored','missing','wrong','short']:
        expected='ok' if MODE in ['ok','absolute','entity'] else 'bad-read' if MODE in ['ignored','missing','wrong','short'] else 'bad-list'
        subprocess.run(['build/webdav-driver',str(server.server_port),expected],check=True,timeout=20)
        print('PASS:',MODE)
    server.shutdown()
