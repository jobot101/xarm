"""server.py — Shop MotherShip control panel: serves ui/ and passes /b/... through to the arm board.

    python server.py                  # http://127.0.0.1:47620/  (board from ../johnny.json or board_ip.txt)
    python server.py --board 192.168.1.27 --no-browser

The board's web API has no CORS headers, so the page talks to it through here: /b/api/state -> http://<board>/api/state.
Standard library only.
"""
import argparse, json, mimetypes, os, socket, sys, threading, urllib.error, urllib.request, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, "ui")
ROOT = os.path.dirname(HERE)                     # the johnny folder: johnny.json, board_ip.txt
sys.path.insert(0, HERE)                         # actions.py
PORT = 47620


def board_host(cli=None):
    if cli: return cli
    try: host = json.load(open(os.path.join(ROOT, "johnny.json"))).get("board", "shop-mothership.local")
    except (OSError, ValueError): host = "shop-mothership.local"
    cache = os.path.join(ROOT, "board_ip.txt")
    try:
        ip = socket.gethostbyname(host)
        if not host.replace(".", "").isdigit(): open(cache, "w").write(ip)
        return ip
    except OSError:                              # Windows often can't resolve .local names: last address that worked
        try: return open(cache).read().strip() or host
        except OSError: return host


class H(BaseHTTPRequestHandler):
    board = "shop-mothership.local"

    def log_message(self, *a): pass

    def send(self, code, body, ctype="application/json", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def proxy(self, method):
        url = f"http://{H.board}{self.path[2:]}"
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)) if method == "POST" else None
        req = urllib.request.Request(url, data=body, method=method,
                                     headers={"Content-Type": self.headers.get("Content-Type", "application/json")})
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                self.send(r.status, r.read(), r.headers.get("Content-Type", "application/octet-stream"))
        except urllib.error.HTTPError as e:
            self.send(e.code, e.read() or b"{}", e.headers.get("Content-Type", "text/plain"))
        except Exception as e:
            self.send(502, json.dumps({"ok": False, "msg": f"can't reach the board at {H.board}: {type(e).__name__}"}).encode())

    def do_GET(self):
        if self.path.startswith("/b/"): return self.proxy("GET")
        if self.path == "/ping": return self.send(200, json.dumps({"ok": True, "board": H.board}).encode())
        if self.path == "/actions":
            import actions
            lib = actions.library()
            return self.send(200, json.dumps({"actions": [{"name": n, "frames": len(actions.load(f)), "secs": sum(t for t, _ in actions.load(f)) / 1000} for n, f in lib.items()],
                                              "last": getattr(H, "last_action", "")}).encode())
        if self.path == "/commands":                 # the brain's action menu (johnny.py COMMANDS), all plain router text
            try:
                sys.path.insert(0, ROOT); import johnny; cmds = list(johnny.COMMANDS)
            except Exception: cmds = []
            return self.send(200, json.dumps(cmds).encode())
        p = self.path.split("?")[0]
        p = "index.html" if p in ("/", "") else p.lstrip("/")
        f = os.path.normpath(os.path.join(UI, p))
        if not f.startswith(UI + os.sep) or not os.path.isfile(f): return self.send(404, b"not found", "text/plain")
        self.send(200, open(f, "rb").read(), mimetypes.guess_type(f)[0] or "application/octet-stream")

    def do_POST(self):
        if self.path.startswith("/b/"):
            if self.path.startswith("/b/api/cmd"):            # STOP from the panel also ends a playing action
                try:
                    import actions; actions.stop()
                except Exception: pass
            return self.proxy("POST")
        if self.path == "/actions/play":
            try: body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
            except ValueError: return self.send(400, json.dumps({"ok": False, "msg": "bad JSON"}).encode())
            import actions
            name = body.get("name", "")
            if not actions.find(name): return self.send(200, json.dumps({"ok": False, "msg": f"no action called {name!r}"}).encode())
            if actions.playing(): return self.send(200, json.dumps({"ok": False, "msg": "another action is already playing"}).encode())
            threading.Thread(target=lambda: setattr(H, "last_action", actions.play(H.board, name, body.get("speed", 0.5), log=lambda *a: None)), daemon=True).start()
            return self.send(200, json.dumps({"ok": True, "msg": f"playing {name} (watch the arm; STOP ends it)"}).encode())
        self.send(404, b"not found", "text/plain")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board")
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    H.board = board_host(a.board)
    try: srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    except OSError:
        print(f"already running on http://127.0.0.1:{PORT}/"); return
    print(f"Shop MotherShip panel on http://127.0.0.1:{PORT}/  (board {H.board})")
    if not a.no_browser: threading.Timer(0.5, lambda: webbrowser.open(f"http://127.0.0.1:{PORT}/")).start()
    srv.serve_forever()


if __name__ == "__main__":
    main()
