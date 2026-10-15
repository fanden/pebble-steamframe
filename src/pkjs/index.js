var Clay = require('pebble-clay');
var clay = new Clay(require('./config'), null, { autoHandleEvents: false });

var POLL_MS = 5000;
var TIMEOUT_MS = 4000;
// Tried in parallel when we don't know where the Frame is. `frame` is SteamOS's default hostname
// (mDNS on the LAN, MagicDNS on Tailscale).
// names only; add a subnet sweep if a renamed Frame without Tailscale turns up
var CANDIDATES = ['frame.local:8765', 'frame:8765', 'steamframe.local:8765'];

var games = [];
var busy = false;  // discovery or pairing in flight

function ls(key) { return localStorage.getItem(key) || ''; }
function host() { return ls('manualHost') || ls('host'); }

function send(msg) {
  Pebble.sendAppMessage(msg, null, function (e) { console.log('send failed: ' + JSON.stringify(e)); });
}
function status(text) { send({ Status: text }); }

// cb(statusCode, parsedBody); statusCode 0 = unreachable/timeout
function request(method, h, path, cb) {
  if (h.indexOf('://') < 0) h = 'http://' + h;
  var xhr = new XMLHttpRequest();
  var done = false;
  function finish(code, body) { if (!done) { done = true; cb(code, body); } }
  // the phone runtime ignores xhr.timeout, so enforce our own
  var timer = setTimeout(function () { xhr.abort(); finish(0); }, TIMEOUT_MS);
  xhr.onload = function () {
    clearTimeout(timer);
    var body = null;
    try { body = JSON.parse(xhr.responseText); } catch (e) {}
    finish(xhr.status, body);
  };
  xhr.onerror = function () { clearTimeout(timer); finish(0); };
  xhr.open(method, h.replace(/\/$/, '') + path);
  xhr.setRequestHeader('Authorization', 'Bearer ' + ls('token'));
  xhr.send();
}

function discover(cb) {
  status('Looking for Frame…');
  var left = CANDIDATES.length, found = false;
  CANDIDATES.forEach(function (h) {
    request('GET', h, '/hello', function (code, body) {
      left--;
      if (!found && code === 200 && body && body.app === 'pebble-frame') {
        found = true;
        cb(h);
      } else if (!found && left === 0) {
        cb(null);
      }
    });
  });
}

function pair(h, cb) {
  status('Pairing…');
  request('POST', h, '/pair', function (code, body) {
    if (code === 200 && body && body.token) {
      localStorage.setItem('token', body.token);
      cb(true);
    } else if (code === 403) {
      status('Frame paired to another phone (see README)');
      cb(false);
    } else {
      cb(false);
    }
  });
}

// Make sure we know a host and hold a token, then run fn(host)
function connected(fn) {
  if (busy) return;
  var h = host();
  if (!h) {
    busy = true;
    return discover(function (found) {
      busy = false;
      if (!found) return status('No Frame found. Is the agent running?');
      localStorage.setItem('host', found);
      connected(fn);
    });
  }
  if (!ls('token')) {
    busy = true;
    return pair(h, function (ok) { busy = false; if (ok) connected(fn); });
  }
  fn(h);
}

function call(method, path, cb) {
  connected(function (h) {
    request(method, h, path, function (code, body) {
      if (code === 200) return cb(body);
      if (code === 401) {  // token no longer valid (Frame was reset): pair again
        localStorage.removeItem('token');
        return status('Re-pairing…');
      }
      if (code === 0 && !ls('manualHost')) {  // Frame moved or asleep: look again next time
        localStorage.removeItem('host');
        return status("Can't reach Frame");
      }
      status(code === 0 ? "Can't reach Frame" : 'Frame error ' + code);
    });
  });
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

function poll() { call('GET', '/stats', sendStats); }

Pebble.addEventListener('ready', function () {
  poll();
  setInterval(poll, POLL_MS);
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload;
  if (p.Action === 'games') {
    call('GET', '/games', function (list) {
      games = list;
      // names truncated to keep the message small; ids stay on the phone
      send({ GameNames: list.map(function (g) { return g.name.substring(0, 30); }).join('\n') || '(no games)' });
    });
  } else if (p.Action === 'refresh') {
    poll();
  } else if (p.Action) {
    call('POST', '/action/' + p.Action, sendStats);
  } else if (p.Launch !== undefined && games[p.Launch]) {
    call('POST', '/action/launch/' + games[p.Launch].id, sendStats);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var s = clay.getSettings(e.response, false);
  localStorage.setItem('manualHost', (s.Host.value || '').trim());
  localStorage.removeItem('token');  // new place, new pairing
  poll();
});
