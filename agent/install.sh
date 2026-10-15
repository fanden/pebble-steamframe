#!/bin/sh
# Run on the Steam Frame (SSH or Desktop Mode → Konsole). Installs into ~ only; no sudo.
#   curl -LsSf https://raw.githubusercontent.com/fanden/pebble-steamframe/main/agent/install.sh | sh
set -e
DIR="$HOME/.local/share/pebble-frame"
REPO="${REPO:-https://raw.githubusercontent.com/fanden/pebble-steamframe/main}"
mkdir -p "$DIR" "$HOME/.config/systemd/user"

if [ -f "$(dirname "$0")/frame_agent.py" ]; then
  cp "$(dirname "$0")/frame_agent.py" "$DIR/"
else
  curl -LsSf "$REPO/agent/frame_agent.py" -o "$DIR/frame_agent.py"
fi

cat > "$HOME/.config/systemd/user/pebble-frame.service" <<EOF
[Unit]
Description=Pebble Steam Frame agent

[Service]
ExecStart=/usr/bin/python3 $DIR/frame_agent.py
Restart=always

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable --now pebble-frame.service
sleep 1

echo
echo "Agent running. Open Steam Frame on your Pebble: it finds this Frame (frame.local) and pairs by itself."
echo "Phone and Frame need to be on the same network (or both on Tailscale)."
