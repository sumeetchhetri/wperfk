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
        # /expect-post validates CLI request options (-m, --body-file, -H)
        if self.path=='/expect-post' and (self.command!='POST' or body!=b'hello-wperfk'
                or self.headers.get('X-Test')!='1'):
            code=400
        self.send_response(code); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)
    do_GET=do_POST=do_PUT=do_DELETE=_r
    def log_message(self,*a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads=True; allow_reuse_address=True; request_queue_size=1024
s=S(('127.0.0.1',int(sys.argv[1])),H)
if len(sys.argv)>2:
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2],sys.argv[3])
    if len(sys.argv)>4:
        ctx.verify_mode=ssl.CERT_REQUIRED; ctx.load_verify_locations(sys.argv[4])
    s.socket=ctx.wrap_socket(s.socket,server_side=True)
s.serve_forever()
