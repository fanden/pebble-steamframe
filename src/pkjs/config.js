module.exports = [
  { "type": "heading", "defaultValue": "Steam Frame" },
  {
    "type": "text",
    "defaultValue": "The app finds your Frame (frame.local) and pairs by itself. Only fill this in if it can't."
  },
  {
    "type": "section",
    "items": [
      {
        "type": "input",
        "messageKey": "Host",
        "label": "Frame address (optional)",
        "description": "e.g. 192.168.1.50:8765 or a Tailscale name. Leave empty for automatic.",
        "attributes": { "placeholder": "automatic" }
      }
    ]
  },
  { "type": "submit", "defaultValue": "Save" }
];
