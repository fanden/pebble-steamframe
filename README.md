# Steam Frame for Pebble

Control and monitor your Valve Steam Frame from a Pebble Time 2.

<p>
<img src="docs/main.png" width="160"> <img src="docs/battery.png" width="160"> <img src="docs/menu.png" width="160"> <img src="docs/power.png" width="160"> <img src="docs/games.png" width="160">
</p>

- **Stats:** battery %, power draw, time left / time to full, CPU load and temperature, RAM, volume, what's playing
- **Battery details:** charge, state, watts, time left, voltage, temperature, health
- **Controls:** volume up/down, mute, launch an installed game, quit the running game
- **Power:** sleep, restart, power off (each needs a second press to confirm)

| Button | Main screen |
|---|---|
| Up / Down | Volume up / down (hold to repeat) |
| Select | Quick settings menu |
| Hold Select | Mute toggle |

## How it works

```
Pebble ⇄ Bluetooth ⇄ phone (PebbleKit JS) ⇄ Wi-Fi/HTTP ⇄ agent on the Frame
```

The agent ([agent/frame_agent.py](agent/frame_agent.py)) is one stdlib-only Python file running as a
systemd user service on the Frame. It reads `/sys` and `/proc`, controls volume with `wpctl`, and
launches games through `steam steam://rungameid/<id>`. Every request needs a token.

## Setup

### 1. Agent on the Steam Frame

Open a terminal on the Frame (SSH, or Desktop Mode → Konsole) and run:

```sh
curl -LsSf https://raw.githubusercontent.com/OWNER/pebble-steamframe/main/agent/install.sh | sh
```

It installs to `~/.local/share/pebble-frame` (no sudo, survives SteamOS updates) and prints the
address and token. Show the token again with `cat ~/.config/pebble-frame/token`.

Uninstall: `systemctl --user disable --now pebble-frame && rm -rf ~/.local/share/pebble-frame ~/.config/systemd/user/pebble-frame.service`

### 2. App on the watch

Download `pebble-steamframe.pbw` from the [latest release](../../releases/latest) and open it with the
Pebble app on your phone, or build it yourself:

```sh
uv tool install pebble-tool
pebble sdk install latest
pebble build
pebble install --phone <phone-ip>
```

Then open the app's settings in the Pebble phone app and enter the address (`192.168.x.x:8765`) and token.

Phone and Frame need to be on the same network. Away from home, put both on Tailscale and use the
Frame's Tailscale name.

## Notes

- Unofficial. Built on what other Frame projects found ([frame-control](https://github.com/saphid/frame-control),
  [frame-perf-overlay](https://github.com/sasaken1102r/frame-perf-overlay)); a SteamOS update can move things.
- Once the Frame sleeps the agent can't wake it; the watch shows "Can't reach Frame".
- The game list only covers the internal Steam library.
- Run the agent's check with `cd agent && python3 test_agent.py`.
