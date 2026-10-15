var Clay = require('pebble-clay');
var clay = new Clay(require('./config'), null, { autoHandleEvents: false });

var POLL_MS = 5000;
var games = [];

function setting(key) {
  return localStorage.getItem(key) || '';
}

function request(method, path, cb) {
  var host = setting('host');
  if (!host) return send({ Status: 'Set Frame address in app settings' });
  if (host.indexOf('://') < 0) host = 'http://' + host;
  var xhr = new XMLHttpRequest();
  xhr.open(method, host.replace(/\/$/, '') + path);
  xhr.setRequestHeader('Authorization', 'Bearer ' + setting('token'));
  xhr.timeout = 4000;
  xhr.onload = function () {
    if (xhr.status === 401) return send({ Status: 'Bad token' });
    if (xhr.status !== 200) return send({ Status: 'Frame error ' + xhr.status });
    cb(JSON.parse(xhr.responseText));
  };
  xhr.onerror = xhr.ontimeout = function () { send({ Status: "Can't reach Frame" }); };
  xhr.send();
}

function send(msg) {
  Pebble.sendAppMessage(msg, null, function (e) { console.log('send failed: ' + JSON.stringify(e)); });
}

function sendStats(s) {
  var b = s.battery || {};
  send({
    Battery: b.pct || 0,
    // Steam reports ~0 W "Discharging" when full on a charger, so treat <0.5 W as idle
    Charging: b.status === 'Charging' ? 1 : 0,
    Watts: Math.round((b.watts || 0) * 10),
    Mins: b.watts >= 0.5 ? (b.mins || 0) : 0,
    BatTemp: b.temp || 0,
    Health: b.health || 0,
    Volts: Math.round((b.volts || 0) * 100),
    CpuTemp: s.cpu_temp,
    Cpu: s.cpu,
    Mem: s.mem,
    Volume: s.volume.pct,
    Muted: s.volume.muted ? 1 : 0,
    Game: s.game || '',
    Status: ''
  });
}

function poll() { request('GET', '/stats', sendStats); }

Pebble.addEventListener('ready', function () {
  poll();
  setInterval(poll, POLL_MS);
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload;
  if (p.Action === 'games') {
    request('GET', '/games', function (list) {
      games = list;
      // names truncated to keep the message small; ids stay on the phone
      send({ GameNames: list.map(function (g) { return g.name.substring(0, 30); }).join('\n') || '(no games)' });
    });
  } else if (p.Action === 'refresh') {
    poll();
  } else if (p.Action) {
    request('POST', '/action/' + p.Action, sendStats);
  } else if (p.Launch !== undefined && games[p.Launch]) {
    request('POST', '/action/launch/' + games[p.Launch].id, sendStats);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var s = clay.getSettings(e.response, false);
  localStorage.setItem('host', (s.Host.value || '').trim());
  localStorage.setItem('token', (s.Token.value || '').trim());
  poll();
});
