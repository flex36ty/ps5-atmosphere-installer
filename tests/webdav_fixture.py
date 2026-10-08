"""Read-only DAV fixture for the shared remote-source integration suite."""
import http.server, urllib.parse, html, time
DATA=bytes(range(251))*(9000000//251+1)
FILES={'/games/game #1.exfat':DATA[:9000000]}
DELAY=0
class Server(http.server.ThreadingHTTPServer):
    daemon_threads=True
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_PROPFIND(self):
        path=urllib.parse.unquote(urllib.parse.urlsplit(self.path).path).rstrip('/')+'/'
        entries={path:None}
        for name,data in FILES.items():
            if name.startswith(path):
                rel=name[len(path):];parts=rel.split('/')
                entries[path+parts[0]+('/' if len(parts)>1 else '')]=None if len(parts)>1 else len(data)
        if len(entries)==1 and path!='/':self.send_error(404);return
        xml='<D:multistatus xmlns:D="DAV:">'
        for name,size in entries.items():
            xml+='<D:response><D:href>'+html.escape(urllib.parse.quote(name))+'</D:href><D:propstat><D:prop><D:resourcetype>'+('<D:collection/>' if size is None else '')+'</D:resourcetype>'
            if size is not None:xml+=f'<D:getcontentlength>{size}</D:getcontentlength>'
            xml+='<D:getlastmodified>Mon, 01 Jan 2024 00:00:00 GMT</D:getlastmodified></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>'
        data=(xml+'</D:multistatus>').encode();self.send_response(207);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def do_HEAD(self):self.file(True)
    def do_GET(self):self.file(False)
    def file(self,head):
        data=FILES.get(urllib.parse.unquote(urllib.parse.urlsplit(self.path).path))
        if data is None:self.send_error(404);return
        start,end=0,len(data)-1;partial=False
        if not head and self.headers.get('Range'):
            start,end=map(int,self.headers['Range'].removeprefix('bytes=').split('-'));end=min(end,len(data)-1);partial=True
            if start>=len(data):self.send_error(416);return
        self.send_response(206 if partial else 200)
        if partial:self.send_header('Content-Range',f'bytes {start}-{end}/{len(data)}')
        self.send_header('Content-Length',str(end-start+1));self.send_header('Last-Modified','Mon, 01 Jan 2024 00:00:00 GMT');self.end_headers()
        if not head:
            try:
                for offset in range(start,end+1,65536):
                    self.wfile.write(data[offset:min(offset+65536,end+1)])
                    if DELAY:time.sleep(DELAY)
            except (BrokenPipeError,ConnectionResetError):pass
