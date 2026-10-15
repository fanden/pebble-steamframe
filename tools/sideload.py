#!/usr/bin/env python3
"""Install the built app from your phone while away: serves build/*.pbw on the Tailscale IP only.

    python3 tools/sideload.py            # then open http://<mac-tailscale-ip>:8080 on the phone

Two ways in from the page:
  1. Download the .pbw, then open it with the Pebble app (it owns the .pbw type on iOS/Android).
  2. "Push" runs `pebble install --phone <your phone>` from this Mac; needs Developer Connection
     turned on in the Pebble app (it listens on :9000, reachable over Tailscale).
"""
import html
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PBW = ROOT / "build" / "pebble-steamframe.pbw"
PEBBLE = Path.home() / ".local/bin/pebble"
BIND = subprocess.run(["tailscale", "ip", "-4"], capture_output=True, text=True).stdout.strip()
PORT = 8080

PAGE = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Steam Frame sideload</title>
<style>body{{font:17px -apple-system,sans-serif;margin:24px;max-width:480px}}
a.b,button{{display:block;width:100%;padding:16px;margin:12px 0;font-size:18px;border:0;border-radius:12px;
background:#1b2838;color:#fff;text-align:center;text-decoration:none}}pre{{white-space:pre-wrap;font-size:12px}}</style>
<h2>Steam Frame for Pebble</h2>
<p>Build: {built}</p>
<a class=b href="/pebble-steamframe.pbw" download>1. Download .pbw</a>
<p>Then in Safari's downloads (or Files) tap the file → Share → <b>Pebble</b>.</p>
<form method=post action=/push><button>2. Push via Developer Connection</button></form>
<p>For option 2: Pebble app → Settings → enable Developer Connection, keep the app open.</p>
<pre>{log}</pre>"""


class H(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="text/html; charset=utf-8", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def page(self, log=""):
        built = PBW.stat().st_mtime if PBW.exists() else None
        built = time.strftime("%Y-%m-%d %H:%M", time.localtime(built)) if built else "missing — run pebble build"
        self.send(200, PAGE.format(built=built, log=html.escape(log)).encode())

    def do_GET(self):
        if self.path == "/pebble-steamframe.pbw" and PBW.exists():
            self.send(200, PBW.read_bytes(), "application/octet-stream",
                      [("Content-Disposition", 'attachment; filename="pebble-steamframe.pbw"')])
        else:
            self.page()

    def do_POST(self):
        # The phone is whoever is asking (Tailscale gives each device its own IP)
        r = subprocess.run([str(PEBBLE), "install", "--phone", self.client_address[0], str(PBW)],
                           capture_output=True, text=True, timeout=120, cwd=ROOT)
        out = "\n".join(l for l in (r.stdout + r.stderr).splitlines() if "Warning" not in l and "Represents" not in l)
        self.page(("OK\n" if r.returncode == 0 else "FAILED\n") + out[-1500:])


if __name__ == "__main__":
    assert BIND.startswith("100."), f"no Tailscale IP ({BIND!r})"
    print(f"http://{BIND}:{PORT}", flush=True)
    ThreadingHTTPServer((BIND, PORT), H).serve_forever()
