#!/usr/bin/env python3
"""Tiny HTTP agent that runs on the Steam Frame and serves stats/controls to the Pebble app.

Stdlib only (SteamOS ships Python 3.12). Every request needs `Authorization: Bearer <token>`;
the token is generated on first run into ~/.config/pebble-frame/token.
"""
import glob
import hmac
import json
import os
import re
import secrets
import signal
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PORT = int(os.environ.get("PEBBLE_FRAME_PORT", "8765"))
TOKEN_FILE = Path.home() / ".config/pebble-frame/token"
STEAMAPPS = Path.home() / ".local/share/Steam/steamapps"
VOLUME_STEP = "5%"
POWER = {"sleep": "suspend", "reboot": "reboot", "poweroff": "poweroff"}


def read(path, default=""):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return default


def token():
    if not TOKEN_FILE.exists():
        TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
        TOKEN_FILE.write_text(secrets.token_urlsafe(16))
        TOKEN_FILE.chmod(0o600)
    return TOKEN_FILE.read_text().strip()


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
        # relies on logind allowing the user manager to suspend (it allows inhibit locks,
        # per frame-control's notes); fall back to SteamClient.System.SuspendPC() over DevTools if not
        subprocess.Popen(["sh", "-c", f"sleep 1; systemctl {POWER[action]}"], start_new_session=True)
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
        if hmac.compare_digest(given, TOKEN.encode()):
            return True
        self.reply(401, {"error": "bad token"})
        return False

    def do_GET(self):
        if not self.authed():
            return
        if self.path == "/stats":
            self.reply(200, stats())
        elif self.path == "/games":
            self.reply(200, games()[:20])
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
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
    TOKEN = token()
    print(f"pebble-frame agent on :{PORT}, token: {TOKEN}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
