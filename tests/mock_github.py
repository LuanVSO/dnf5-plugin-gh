import http.server
import json
import os
import ssl
import sys

work, arch = sys.argv[1:]


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/assets/"):
            with open(os.path.join(work, "asset-requests"), "a") as log:
                log.write(self.path + "\n")
            if os.path.exists(os.path.join(work, "fail-asset")):
                self.send_error(503)
                return
            name = self.path.removeprefix("/assets/")
            path = os.path.join(work, "fixtures", name)
            if not os.path.isfile(path):
                self.send_error(404)
                return
            with open(path, "rb") as source:
                data = source.read()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if self.path != "/repos/modrinth/code/releases?per_page=100":
            self.send_error(404)
            return
        with open(os.path.join(work, "requests"), "a") as log:
            log.write("release\n")
        if os.path.exists(os.path.join(work, "fail")):
            self.send_error(503)
            return
        with open(os.path.join(work, "version")) as version_file:
            version = version_file.read().strip()
        rpm_name = f"Modrinth.App-{version}-1.{arch}.rpm"
        path = os.path.join(work, "fixtures", rpm_name)
        assets = [{
            "name": rpm_name,
            "browser_download_url": "https://api.github.com/assets/" + rpm_name,
            "size": os.path.getsize(path),
        }]
        if os.path.exists(os.path.join(work, "two-assets")):
            rpm_name = f"Modrinth.Data-{version}-1.{arch}.rpm"
            path = os.path.join(work, "fixtures", rpm_name)
            assets.append({
                "name": rpm_name,
                "browser_download_url": "https://api.github.com/assets/" + rpm_name,
                "size": os.path.getsize(path),
            })
        data = json.dumps([{
            "tag_name": "v" + version,
            "draft": False,
            "prerelease": False,
            "assets": assets,
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
