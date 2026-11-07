#!/usr/bin/env python3
"""Local HTTP server for the webmacro fixture tests (test/sql/webmacro_fixture_server.test).

Serves test/fixtures and a few extra endpoints:
  /redirect?to=<url>   302 to <url>
  /big?bytes=<n>       a response of n bytes
  /status/<code>       an empty response with that status

Usage:
  python3 test/fixture_server.py [port]   (default 8765)
  WEBMACRO_FIXTURE_URL=http://127.0.0.1:8765 ./build/release/test/unittest test/sql/webmacro_fixture_server.test
"""
import http.server
import os
import sys
import urllib.parse

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=FIXTURES, **kwargs)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        if parsed.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", query["to"][0])
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif parsed.path == "/big":
            size = int(query.get("bytes", ["2097152"])[0])
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            self.end_headers()
            self.wfile.write(b"x" * size)
        elif parsed.path.startswith("/status/"):
            self.send_response(int(parsed.path.split("/")[2]))
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            super().do_GET()

    def log_message(self, format, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
