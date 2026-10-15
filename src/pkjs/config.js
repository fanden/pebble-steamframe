module.exports = [
  { "type": "heading", "defaultValue": "Steam Frame" },
  {
    "type": "text",
    "defaultValue": "Run the agent on your Frame (see README), then paste the address and token it prints."
  },
  {
    "type": "section",
    "items": [
      {
        "type": "input",
        "messageKey": "Host",
        "label": "Frame address",
        "description": "e.g. 192.168.1.50:8765 (or a Tailscale name)",
        "attributes": { "placeholder": "192.168.1.50:8765" }
      },
      {
        "type": "input",
        "messageKey": "Token",
        "label": "Token"
      }
    ]
  },
  { "type": "submit", "defaultValue": "Save" }
];
