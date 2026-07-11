// Claude Traffic Light — local proxy
// ------------------------------------------------------------
// Transparently proxies Claude Code -> api.anthropic.com and captures
// the anthropic-ratelimit-unified-* response headers along the way.
// Serves the latest state to the traffic light (ESP32-C3) via GET /util,
// including reset_in_5h/reset_in_7d — SECONDS remaining until each window resets.
//
// Run:
//   node claude-traffic-light-proxy.js
//
// Point Claude Code at the proxy (same terminal session):
//   export ANTHROPIC_BASE_URL=http://127.0.0.1:8787
//
// SECURITY: your OAuth token passes through this proxy. Trusted home
// network only — never expose this port to the internet.
// ------------------------------------------------------------

const http  = require('http');
const https = require('https');
const os    = require('os');

const UPSTREAM = 'api.anthropic.com';
const PORT = 8787;

let state = {
  util_5h: null,       // 0.0..1.0 — how full the 5-hour window is
  status_5h: null,     // allowed | exceeded | rate_limited
  util_7d: null,       // 0.0..1.0 — weekly window
  status_7d: null,     // allowed | exceeded | rate_limited
  reset_raw_5h: null,  // raw reset header value (ISO or epoch)
  reset_raw_7d: null,  // raw reset header value (ISO or epoch)
  updated: null,       // when we last saw the headers
};

function num(h, key) {
  const v = h[key];
  return v === undefined ? null : parseFloat(v);
}

// Returns seconds until reset from an ISO timestamp or unix epoch (seconds).
function resetInSeconds(v) {
  if (v === undefined || v === null) return null;
  const s = String(v).trim();
  if (/^\d+(\.\d+)?$/.test(s)) {                     // unix epoch (seconds)
    return Math.max(0, Math.round(parseFloat(s) - Date.now() / 1000));
  }
  const t = Date.parse(s);                            // ISO 8601
  return isNaN(t) ? null : Math.max(0, Math.round((t - Date.now()) / 1000));
}

const server = http.createServer((req, res) => {
  // Endpoint for the traffic light: reset_in_5h is computed per-request (always fresh)
  if (req.method === 'GET' && req.url === '/util') {
    const out = {
      util_5h: state.util_5h,
      status_5h: state.status_5h,
      util_7d: state.util_7d,
      status_7d: state.status_7d,
      reset_in_5h: resetInSeconds(state.reset_raw_5h),
      reset_in_7d: resetInSeconds(state.reset_raw_7d),
      updated: state.updated,
    };
    res.writeHead(200, {
      'Content-Type': 'application/json',
      'Access-Control-Allow-Origin': '*',
    });
    res.end(JSON.stringify(out));
    return;
  }

  // Everything else — pass through to Anthropic untouched
  const headers = { ...req.headers, host: UPSTREAM };
  const upstream = https.request(
    { hostname: UPSTREAM, port: 443, path: req.url, method: req.method, headers },
    (up) => {
      const h = up.headers;
      if (h['anthropic-ratelimit-unified-5h-utilization'] !== undefined) {
        state = {
          util_5h: num(h, 'anthropic-ratelimit-unified-5h-utilization'),
          status_5h: h['anthropic-ratelimit-unified-5h-status'] ?? null,
          util_7d: num(h, 'anthropic-ratelimit-unified-7d-utilization'),
          status_7d: h['anthropic-ratelimit-unified-7d-status'] ?? null,
          reset_raw_5h: h['anthropic-ratelimit-unified-5h-reset'] ?? null,
          reset_raw_7d: h['anthropic-ratelimit-unified-7d-reset'] ?? null,
          updated: Date.now(),
        };
        console.log(
          `5h util=${state.util_5h} status=${state.status_5h} reset_in=${resetInSeconds(state.reset_raw_5h)}s  ` +
          `7d util=${state.util_7d} status=${state.status_7d} reset_in=${resetInSeconds(state.reset_raw_7d)}s`
        );
      }
      res.writeHead(up.statusCode, up.headers);
      up.pipe(res); // pipe the body as-is (including gzip streams)
    }
  );

  upstream.on('error', (e) => { res.writeHead(502); res.end('proxy error: ' + e.message); });
  req.pipe(upstream);
});

server.listen(PORT, '0.0.0.0', () => {
  const ips = [];
  for (const ifaces of Object.values(os.networkInterfaces()))
    for (const i of ifaces) if (i.family === 'IPv4' && !i.internal) ips.push(i.address);
  console.log(`Claude traffic-light proxy listening on :${PORT}`);
  console.log(`Claude Code:   export ANTHROPIC_BASE_URL=http://127.0.0.1:${PORT}`);
  console.log(`For ESP32:     PROXY_URL = http://${ips[0] || '<host-IP>'}:${PORT}/util`);
});
