#!/bin/sh
# Run on the Steam Frame (SSH or Desktop Mode → Konsole). Installs into ~ only; no sudo.
#   curl -LsSf https://raw.githubusercontent.com/OWNER/pebble-steamframe/main/agent/install.sh | sh
set -e
DIR="$HOME/.local/share/pebble-frame"
REPO="${REPO:-https://raw.githubusercontent.com/OWNER/pebble-steamframe/main}"
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

IP=$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -n1)
echo
echo "Agent running. In the Pebble app settings enter:"
echo "  Frame address: $IP:8765"
echo "  Token:         $(cat "$HOME/.config/pebble-frame/token")"
