#!/usr/bin/env python3
"""Tiny HTTP agent that runs on the Steam Frame and serves stats/controls to the Pebble app.

Stdlib only (SteamOS ships Python 3.12). The watch finds the agent as frame.local via GET /hello
and pairs itself with POST /pair, which hands out the token once (trust on first use). Every other
request needs `Authorization: Bearer <token>`. To pair a different phone, delete
~/.config/pebble-frame/token.
"""
import base64
import glob
import hmac
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORT = int(os.environ.get("PEBBLE_FRAME_PORT", "8765"))
TOKEN_FILE = Path.home() / ".config/pebble-frame/token"
STEAMAPPS = Path.home() / ".local/share/Steam/steamapps"
VOLUME_STEP = "5%"
# Steam does these itself; `systemctl` from outside the session needs interactive auth
POWER = {"sleep": "SuspendPC", "reboot": "RestartPC", "poweroff": "ShutdownPC"}
_pair_lock = threading.Lock()


def read(path, default=""):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def token():
    return read(TOKEN_FILE)


def pair():
    """Create and return the token if nobody has paired yet, else None."""
    with _pair_lock:
        if token():
            return None
        TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
        TOKEN_FILE.touch(mode=0o600)
        TOKEN_FILE.write_text(secrets.token_urlsafe(16))
        return token()


def steam_js(expr):
    """Fire-and-forget JS in the Steam client's SharedJSContext over its local DevTools port."""
    pages = json.load(urllib.request.urlopen("http://127.0.0.1:8080/json", timeout=3))
    url = next(p["webSocketDebuggerUrl"] for p in pages if p.get("title") == "SharedJSContext")
    hostport, path = url.split("://", 1)[1].split("/", 1)
    host, port = hostport.rsplit(":", 1)
    with socket.create_connection((host, int(port)), timeout=5) as sock:
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        if b" 101 " not in sock.recv(4096).split(b"\r\n")[0]:
            raise OSError("DevTools refused the WebSocket upgrade")
        data = json.dumps({"id": 1, "method": "Runtime.evaluate", "params": {"expression": expr}}).encode()
        mask = os.urandom(4)
        n = len(data)  # masked text frame; payloads here are always < 64 KiB
        head = bytes([0x81, 0x80 | n]) if n < 126 else bytes([0x81, 0x80 | 126]) + n.to_bytes(2, "big")
        sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        sock.recv(4096)  # wait for the reply so the call isn't dropped when we close


def battery():
    for ps in glob.glob("/sys/class/power_supply/*"):
        if read(f"{ps}/type") != "Battery":
            continue
        uv, ua = int(read(f"{ps}/voltage_now", "0")), int(read(f"{ps}/current_now", "0"))
        status = read(f"{ps}/status")
        mins = int(read(f"{ps}/time_to_full_now" if status == "Charging" else f"{ps}/time_to_empty_now", "0")) // 60
        return {
            "pct": int(read(f"{ps}/capacity", "0")),
            "status": status,
            "watts": round(abs(uv * ua) / 1e12, 1),
            "mins": mins,
            "temp": int(read(f"{ps}/temp", "0")) // 10,
            "volts": round(uv / 1e6, 2),
            "health": health(ps),
        }
    return {}


def health(ps):
    """Full-charge capacity as % of design capacity (0 if the driver doesn't say)."""
    for kind in ("charge", "energy"):
        full, design = int(read(f"{ps}/{kind}_full", "0")), int(read(f"{ps}/{kind}_full_design", "0"))
        if full and design:
            return round(100 * full / design)
    return 0


def cpu_temp():
    temps = [int(read(f"{z}/temp", "0")) for z in glob.glob("/sys/class/thermal/thermal_zone*")
             if read(f"{z}/type").startswith("cpu")]
    return max(temps, default=0) // 1000


_prev_cpu = (0, 0)


def cpu_pct():
    # % busy since the previous call (agent is polled every few seconds)
    global _prev_cpu
    fields = [int(x) for x in read("/proc/stat").splitlines()[0].split()[1:]]
    idle, total = fields[3] + fields[4], sum(fields)
    d_idle, d_total = idle - _prev_cpu[0], total - _prev_cpu[1]
    _prev_cpu = (idle, total)
    return round(100 * (1 - d_idle / d_total)) if d_total else 0


def mem_pct():
    info = dict(re.findall(r"(\w+):\s+(\d+)", read("/proc/meminfo")))
    total = int(info.get("MemTotal", 1))
    return round(100 * (1 - int(info.get("MemAvailable", total)) / total))


def parse_volume(out):
    """`wpctl get-volume` → (percent, muted). e.g. 'Volume: 0.45 [MUTED]' → (45, True)."""
    m = re.search(r"Volume:\s*([\d.]+)", out)
    return (round(float(m.group(1)) * 100) if m else 0), "[MUTED]" in out


def volume():
    try:
        out = subprocess.run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"],
                             capture_output=True, text=True, timeout=2).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {"pct": 0, "muted": False}
    pct, muted = parse_volume(out)
    return {"pct": pct, "muted": muted}


def acf_value(text, key):
    m = re.search(rf'"{key}"\s+"([^"]*)"', text)
    return m.group(1) if m else ""


def games():
    """Installed games, most recently played first."""
    # only the default library; add libraryfolders.vdf parsing for SD-card/extra libraries
    out = []
    for f in STEAMAPPS.glob("appmanifest_*.acf"):
        t = read(f)
        name = acf_value(t, "name")
        if name and not name.startswith(("Proton", "Steam Linux Runtime", "Steamworks")):
            out.append({"id": acf_value(t, "appid"), "name": name, "last": int(acf_value(t, "LastPlayed") or 0)})
    return sorted(out, key=lambda g: -g["last"])


def running_game():
    """(pid, appid) of Steam's reaper for the running game, found via its `AppId=` argument."""
    for cmd in glob.glob("/proc/[0-9]*/cmdline"):
        args = read(cmd).split("\0")
        if args and args[0].endswith("reaper"):
            for a in args:
                if a.startswith("AppId="):
                    return int(cmd.split("/")[2]), a[6:]
    return None, None


def stats():
    _, appid = running_game()
    game = ""
    if appid:
        game = acf_value(read(STEAMAPPS / f"appmanifest_{appid}.acf"), "name") or f"App {appid}"
    return {
        "battery": battery(),
        "cpu": cpu_pct(),
        "cpu_temp": cpu_temp(),
        "mem": mem_pct(),
        "volume": volume(),
        "game": game,
        "uptime_min": int(float(read("/proc/uptime", "0").split()[0]) // 60),
    }


def wpctl(*args):
    subprocess.run(["wpctl", *args], timeout=2)


def act(action, arg):
    if action == "vol_up":
        wpctl("set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", VOLUME_STEP + "+")
    elif action == "vol_down":
        wpctl("set-volume", "@DEFAULT_AUDIO_SINK@", VOLUME_STEP + "-")
    elif action == "mute":
        wpctl("set-mute", "@DEFAULT_AUDIO_SINK@", "toggle")
    elif action == "quit_game":
        pid, _ = running_game()
        if pid:
            os.kill(pid, signal.SIGTERM)
    elif action == "launch" and arg.isdigit():
        subprocess.Popen(["steam", f"steam://rungameid/{arg}"], start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif action in POWER:
        # delayed so the HTTP reply gets out before the Frame goes down
        threading.Timer(1, steam_js, [f"SteamClient.System.{POWER[action]}()"]).start()
    else:
        return False
    return True


class Handler(BaseHTTPRequestHandler):
    def reply(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def authed(self):
        given = self.headers.get("Authorization", "").removeprefix("Bearer ").encode()
        current = token().encode()
        if current and hmac.compare_digest(given, current):
            return True
        self.reply(401, {"error": "bad token"})
        return False

    def do_GET(self):
        if self.path == "/hello":  # discovery probe, no auth
            return self.reply(200, {"app": "pebble-frame", "paired": bool(token())})
        if not self.authed():
            return
        if self.path == "/stats":
            self.reply(200, stats())
        elif self.path == "/games":
            self.reply(200, games()[:20])
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/pair":
            t = pair()
            return self.reply(200, {"token": t}) if t else self.reply(403, {"error": "already paired"})
        if not self.authed():
            return
        # /action/<name>[/<arg>]
        parts = self.path.strip("/").split("/")
        if len(parts) >= 2 and parts[0] == "action" and act(parts[1], parts[2] if len(parts) > 2 else ""):
            time.sleep(0.2)  # let wpctl/steam settle so the returned stats reflect the change
            self.reply(200, stats())
        else:
            self.reply(400, {"error": "unknown action"})

    def log_message(self, *_):
        pass


if __name__ == "__main__":
    print(f"pebble-frame agent on :{PORT}, {'paired' if token() else 'waiting to pair'}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
