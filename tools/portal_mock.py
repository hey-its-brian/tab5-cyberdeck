#!/usr/bin/env python3
"""Serve the file portal's real web page from this Mac, backed by a local
folder, so the browser side can be developed and screenshotted without the
Tab5. Mirrors the JSON API in components/deck_portal/portal.c.

    tools/portal_mock.py                 # sim/sdcard, http://127.0.0.1:8080, PIN 271828
    tools/portal_mock.py --root DIR --port 8000
    tools/portal_mock.py --open          # no PIN (for headless screenshots)

Stdlib only. Listens on localhost only; this is a dev tool, not a server.
"""
import argparse
import json
import os
import secrets
import shutil
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "components", "deck_portal", "www", "index.html")
PIN = "271828"  # same as the simulator's PORTAL screen
TOKEN = secrets.token_hex(16)
ROOT = ""
OPEN = False


def full_path(rel):
    """Same rules as portal.c: absolute, no '..', no backslash, no '//'."""
    if not rel or not rel.startswith("/") or len(rel) >= 192:
        return None
    if ".." in rel or "\\" in rel or "//" in rel:
        return None
    return os.path.join(ROOT, rel.rstrip("/").lstrip("/"))


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[portal] " + fmt % args)

    # ---- helpers ----
    def send_json(self, obj, status=200, headers=None):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def fail(self, status, msg):
        self.send_json({"error": msg}, status)

    def query(self):
        q = urllib.parse.urlparse(self.path).query
        return {k: v[0] for k, v in urllib.parse.parse_qs(q).items()}

    def authed(self):
        return OPEN or ("deck=" + TOKEN) in (self.headers.get("Cookie") or "")

    def route(self):
        return urllib.parse.urlparse(self.path).path

    # ---- GET ----
    def do_GET(self):
        r = self.route()
        if r == "/":
            with open(PAGE, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if not r.startswith("/api/"):
            return self.fail(404, "not found")
        if not self.authed():
            return self.fail(401, "login")
        q = self.query()
        if r == "/api/info":
            du = shutil.disk_usage(ROOT)
            return self.send_json({"storage": "SD CARD", "free": du.free, "total": du.total})
        if r == "/api/list":
            path = full_path(q.get("path", "/"))
            if path is None:
                return self.fail(400, "bad path")
            if not os.path.isdir(path):
                return self.fail(404, "no such folder")
            entries = []
            for name in sorted(os.listdir(path)):
                if name.startswith("."):
                    continue
                st = os.stat(os.path.join(path, name))
                entries.append({"name": name, "dir": os.path.isdir(os.path.join(path, name)),
                                "size": st.st_size, "mtime": int(st.st_mtime)})
            return self.send_json({"entries": entries})
        if r == "/api/file":
            path = full_path(q.get("path", ""))
            if path is None or not os.path.isfile(path):
                return self.fail(404, "no such file")
            with open(path, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % os.path.basename(path))
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.fail(404, "not found")

    # ---- POST ----
    def do_POST(self):
        r = self.route()
        n = int(self.headers.get("Content-Length") or 0)
        if r == "/api/login":
            body = self.rfile.read(n).decode(errors="replace")
            if body.startswith("pin=") and body[4:10] == PIN:
                return self.send_json({"ok": True}, headers={
                    "Set-Cookie": "deck=%s; Path=/; HttpOnly; SameSite=Strict" % TOKEN})
            return self.fail(403, "wrong pin")
        if not self.authed():
            self.rfile.read(n)
            return self.fail(401, "login")
        q = self.query()
        if r == "/api/upload":
            path = full_path(q.get("path", ""))
            if path is None or len(q.get("path", "")) < 2:
                self.rfile.read(n)
                return self.fail(400, "bad path")
            with open(path + ".part", "wb") as f:
                left = n
                while left > 0:
                    chunk = self.rfile.read(min(left, 65536))
                    if not chunk:
                        break
                    f.write(chunk)
                    left -= len(chunk)
                    time.sleep(0.01)  # slow it down a little so the progress bars show
            os.replace(path + ".part", path)
            return self.send_json({"ok": True})
        if r == "/api/mkdir":
            path = full_path(q.get("path", ""))
            if path is None:
                return self.fail(400, "bad path")
            os.makedirs(path, exist_ok=True)
            return self.send_json({"ok": True})
        if r == "/api/delete":
            path = full_path(q.get("path", ""))
            if path is None or len(q.get("path", "")) < 2 or not os.path.exists(path):
                return self.fail(400, "bad path")
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
            return self.send_json({"ok": True})
        if r == "/api/rename":
            a, b = full_path(q.get("from", "")), full_path(q.get("to", ""))
            if a is None or b is None:
                return self.fail(400, "bad path")
            if os.path.exists(b):
                return self.fail(409, "name already exists")
            os.rename(a, b)
            return self.send_json({"ok": True})
        self.fail(404, "not found")


def main():
    global ROOT, OPEN
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.join(HERE, "..", "sim", "sdcard"))
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--open", action="store_true", help="skip the PIN")
    a = ap.parse_args()
    ROOT = os.path.abspath(a.root)
    OPEN = a.open
    for d in ("notes", "music"):
        os.makedirs(os.path.join(ROOT, d), exist_ok=True)
    print("portal mock: http://127.0.0.1:%d  PIN %s  serving %s" % (a.port, PIN, ROOT))
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
