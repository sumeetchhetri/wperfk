import http.server, json, socketserver, ssl, sys, threading
# usage: server.py PORT [CERT KEY [CLIENT_CA]]   (CLIENT_CA: require client certs)
hits = {}
lock = threading.Lock()

class H(http.server.BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    disable_nagle_algorithm=True
    def _r(self):
        n=int(self.headers.get('Content-Length',0) or 0)
        body=self.rfile.read(n) if n else b''
        code=200
        b=b'{"ok":true,"token":"abc"}'
        if self.path=='/__hits':            # per "METHOD path" request counts
            with lock: b=json.dumps(hits).encode()
        elif self.path=='/__reset':
            with lock: hits.clear()
        else:
            with lock:
                k=self.command+' '+self.path
                hits[k]=hits.get(k,0)+1
        # response framings (parser tests)
        if self.path=='/chunked':
            self.send_response(200); self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            for part in (b'hello ', b'chunked ', b'world'): self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
            self.wfile.write(b'0\r\n\r\n'); return
        if self.path=='/close':           # body delimited by connection close
            self.wfile.write(b'HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil-eof'); self.close_connection=True; return
        if self.path=='/http10':
            self.wfile.write(b'HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok'); self.close_connection=True; return
        if self.path=='/lf':              # bare LF line endings
            self.wfile.write(b'HTTP/1.1 200 OK\nContent-Length: 2\n\nok'); return
        if self.path=='/drop':            # keep-alive response, then the server closes anyway
            self.wfile.write(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok'); self.close_connection=True; return
        if self.path=='/204':
            self.send_response(204); self.end_headers(); return
        if self.path=='/big':
            b=b'x'*(1<<20)
        # /expect-post validates CLI request options (-m, --body-file, -H)
        if self.path=='/expect-post' and (self.command!='POST' or body!=b'hello-wperfk'
                or self.headers.get('X-Test')!='1'):
            code=400
        self.send_response(code); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)
    do_GET=do_POST=do_PUT=do_DELETE=_r
    def log_message(self,*a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads=True; allow_reuse_address=True; request_queue_size=1024
if sys.argv[1].startswith('unix:'):   # unix:/path/to.sock
    import os
    class US(socketserver.ThreadingMixIn, socketserver.UnixStreamServer): daemon_threads=True
    path=sys.argv[1][5:]
    if os.path.exists(path): os.unlink(path)
    class UH(H): disable_nagle_algorithm=False   # TCP_NODELAY fails on AF_UNIX
    s=US(path,UH)
else:
    s=S(('127.0.0.1',int(sys.argv[1])),H)
if len(sys.argv)>2:
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2],sys.argv[3])
    if len(sys.argv)>4:
        ctx.verify_mode=ssl.CERT_REQUIRED; ctx.load_verify_locations(sys.argv[4])
    s.socket=ctx.wrap_socket(s.socket,server_side=True)
s.serve_forever()
