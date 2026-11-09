import http.server
import json
import os
import ssl
import sys

work, arch = sys.argv[1:]
rpm_name = f"Modrinth.App-0.23.1-1.{arch}.rpm"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/repos/modrinth/code/releases?per_page=100":
            self.send_error(404)
            return
        path = os.path.join(work, "varlib/dnf5-gh/modrinth_code", rpm_name)
        data = json.dumps([{
            "tag_name": "v0.23.1",
            "draft": False,
            "prerelease": False,
            "assets": [{
                "name": rpm_name,
                "browser_download_url": "https://api.github.com/test.rpm",
                "size": os.path.getsize(path),
            }],
        }]).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


server = http.server.HTTPServer(("127.0.0.1", 443), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(os.path.join(work, "cert.pem"), os.path.join(work, "key.pem"))
server.socket = context.wrap_socket(server.socket, server_side=True)
server.serve_forever()
