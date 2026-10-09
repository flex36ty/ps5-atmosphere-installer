"""Local read-only FTP fixture; no external service or game files required."""
import socket, socketserver, subprocess, sys, threading, posixpath, time
DATA=bytes(range(251))*(9000000//251+1)
FILES={'/games/game #1.exfat': DATA[:9000000]}
DELAY=0
WRITE_ENABLED=False
DIRECTORIES=set()
CORRUPT_UPLOADS=False
RETR_PATHS=[]
class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        passive=None; offset=0; cwd='/';rename_from=None
        def path(arg):return posixpath.normpath(posixpath.join(cwd,arg))
        def directory(p):return p=='/' or p in DIRECTORIES or any(f.startswith(p.rstrip('/')+'/') for f in FILES)
        def reply(s):self.wfile.write((s+'\r\n').encode());self.wfile.flush()
        reply('220 fixture')
        try:
            for raw in self.rfile:
                command,_,arg=raw.decode().strip().partition(' ');command=command.upper()
                if command=='USER':reply('331 password')
                elif command=='PASS':reply('230 logged in')
                elif command=='PWD':reply('257 "/"')
                elif command=='SYST':reply('215 UNIX Type: L8')
                elif command in ('TYPE','NOOP'):reply('200 ok')
                elif command=='CWD':
                    p=path(arg)
                    if directory(p):cwd=p;reply('250 directory changed')
                    else:reply('550 missing directory')
                elif command=='EPSV':
                    if passive:passive.close()
                    passive=socket.socket();passive.bind(('127.0.0.1',0));passive.listen()
                    reply(f'229 Entering Extended Passive Mode (|||{passive.getsockname()[1]}|)')
                elif command=='SIZE':reply(f'213 {len(FILES[path(arg)])}' if path(arg) in FILES else '550 missing file')
                elif command=='MDTM':reply('213 20240101000000' if path(arg) in FILES else '550 missing file')
                elif command=='REST':offset=int(arg);reply('350 restart accepted')
                elif command=='MKD':
                    p=path(arg)
                    if not WRITE_ENABLED or directory(p) or p in FILES or not directory(posixpath.dirname(p)):reply('550 not permitted')
                    else:DIRECTORIES.add(p);reply('257 created')
                elif command=='RMD':
                    p=path(arg)
                    if not WRITE_ENABLED or p not in DIRECTORIES or any(f.startswith(p+'/') for f in FILES) or any(d.startswith(p+'/') for d in DIRECTORIES):reply('550 not permitted')
                    else:DIRECTORIES.remove(p);reply('250 removed')
                elif command=='RNFR':
                    p=path(arg)
                    if WRITE_ENABLED and (p in DIRECTORIES or p in FILES):rename_from=p;reply('350 rename ready')
                    else:reply('550 not permitted')
                elif command=='RNTO':
                    p=path(arg)
                    if not WRITE_ENABLED or not rename_from or directory(p) or p in FILES:reply('550 not permitted')
                    else:
                        for f in list(FILES):
                            if f==rename_from or f.startswith(rename_from+'/'):FILES[p+f[len(rename_from):]]=FILES.pop(f)
                        for d in list(DIRECTORIES):
                            if d==rename_from or d.startswith(rename_from+'/'):DIRECTORIES.remove(d);DIRECTORIES.add(p+d[len(rename_from):])
                        rename_from=None;reply('250 renamed')
                elif command=='STOR':
                    p=path(arg)
                    if not WRITE_ENABLED or p in FILES:reply('550 not permitted');continue
                    reply('150 opening data');data,_=passive.accept();content=bytearray()
                    while True:
                        chunk=data.recv(65536)
                        if not chunk:break
                        content.extend(chunk)
                        if DELAY:time.sleep(DELAY)
                    data.close();passive.close();passive=None
                    if CORRUPT_UPLOADS and content:content[0]^=1
                    FILES[p]=bytes(content);reply('226 done')
                elif command in ('MLSD','RETR'):
                    reply('150 opening data');data,_=passive.accept()
                    try:
                        if command=='MLSD':
                            entries={}
                            for d in DIRECTORIES:
                                prefix=cwd.rstrip('/')+'/'
                                if d.startswith(prefix) and '/' not in d[len(prefix):]:
                                    name=d[len(prefix):];entries[name]=f'type=dir; {name}\r\n'
                            for f,content in FILES.items():
                                prefix=cwd.rstrip('/')+'/'
                                if not f.startswith(prefix):continue
                                rel=f[len(prefix):];name=rel.split('/')[0]
                                entries[name]=f'type=dir; {name}\r\n' if '/' in rel else f'type=file;size={len(content)};modify=20240101000000; {name}\r\n'
                            data.sendall(('type=cdir; .\r\ntype=pdir; ..\r\n'+''.join(entries.values())).encode())
                        else:
                            RETR_PATHS.append(path(arg))
                            content=FILES[path(arg)]
                            for start in range(offset,len(content),65536):
                                data.sendall(content[start:start+65536])
                                if DELAY:time.sleep(DELAY)
                            offset=0
                    except (BrokenPipeError,ConnectionResetError):pass
                    finally:data.close();passive.close();passive=None
                    reply('226 done')
                elif command=='QUIT':reply('221 bye');break
                else:reply('502 unsupported')
        except (ConnectionResetError,BrokenPipeError):pass
        finally:
            if passive:passive.close()
class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
if __name__=='__main__':
    with Server(('127.0.0.1',0),Handler) as server:
        threading.Thread(target=server.serve_forever,daemon=True).start()
        result=subprocess.run([sys.argv[1],str(server.server_address[1])],timeout=30)
        server.shutdown()
        raise SystemExit(result.returncode)
