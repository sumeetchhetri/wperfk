import http.server, socketserver, sys, ssl
class H(http.server.BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def _r(self):
        n=int(self.headers.get('Content-Length',0) or 0)
        if n: self.rfile.read(n)
        b=b'{"ok":true,"token":"abc"}'
        self.send_response(200); self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)
    do_GET=do_POST=do_PUT=do_DELETE=_r
    def log_message(self,*a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads=True; allow_reuse_address=True; request_queue_size=1024
s=S(('127.0.0.1',int(sys.argv[1])),H)
if len(sys.argv)>2:
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2],sys.argv[3]); s.socket=ctx.wrap_socket(s.socket,server_side=True)
s.serve_forever()
