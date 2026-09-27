// sysmon server: runs the Swift sampler, keeps 2 min of history, serves it on the LAN.
//   GET /           phone/desktop web view
//   GET /api/now    compact JSON polled by the ESP32 once per second
// Usage: node server.mjs   (env PORT, default 8787)
import { spawn, execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { existsSync, statSync, readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { claudeUsage, codexUsage } from './usage.mjs';

const DIR = dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT || 8787);
const HISTORY = 120;
const SAMPLER = join(DIR, 'sampler');
const SAMPLER_SRC = join(DIR, 'sampler.swift');

if (!existsSync(SAMPLER) || statSync(SAMPLER).mtimeMs < statSync(SAMPLER_SRC).mtimeMs) {
  console.log('building sampler…');
  execFileSync('swiftc', ['-O', '-o', SAMPLER, SAMPLER_SRC], { stdio: 'inherit' });
}

let latest = null;
const hist = { cpu: [], mem: [], pressure: [], swapr: [] };

// "Google Chrome Helper (Renderer)" -> "Google Chrome"
const appName = (n) => n.replace(/ Helper( \(.*\))?$/, '').replace(/ \(.*\)$/, '') || n;

function ingest(s) {
  const m = s.mem;
  const top = new Map();
  for (const [name, cpu, rss] of s.top) {
    const k = appName(name);
    const cur = top.get(k) || [k, 0, 0];
    cur[1] += cpu; cur[2] += rss;
    top.set(k, cur);
  }
  latest = {
    t: s.t,
    cpu: Math.round(s.cpu),
    cores: s.cores.map(Math.round),
    load: Math.round(s.load * 100) / 100,
    mp: m.pressure,                       // 1 normal · 2 warn · 4 critical
    mfree: m.freePct,
    mused: Math.round(m.usedGB * 10) / 10,
    mtot: Math.round(m.totalGB),
    mcomp: Math.round(m.compressedGB * 10) / 10,   // RAM the compressor occupies
    mstored: Math.round(m.storedGB * 10) / 10,     // …holding this much data uncompressed
    mdemand: Math.round(m.demandGB * 10) / 10,     // app data if nothing were compressed
    swap: Math.round(m.swapUsedGB * 10) / 10,
    swapr: Math.round(m.swapMBps * 10) / 10, // MB/s swapped in+out
    top: [...top.values()].sort((a, b) => b[1] - a[1]).slice(0, 5)
      // the ESP32 font is ASCII-only; anything else would index outside the glyph table
      .map(([n, c, r]) => [(n.replace(/[^\x20-\x7e]+/g, '?').replace(/^\?+$/, 'app')).slice(0, 24), Math.round(c), Math.round(r)]),
  };
  const push = (a, v) => { a.push(v); if (a.length > HISTORY) a.shift(); };
  push(hist.cpu, latest.cpu);
  push(hist.mem, Math.round(m.usedGB / m.totalGB * 100));
  push(hist.pressure, m.pressure);
  push(hist.swapr, Math.round(m.swapMBps));
}

function startSampler() {
  const p = spawn(SAMPLER, [], { stdio: ['ignore', 'pipe', 'inherit'] });
  createInterface({ input: p.stdout }).on('line', (line) => {
    try { ingest(JSON.parse(line)); } catch (e) { console.error('bad sample', e.message); }
  });
  p.on('exit', (code) => { console.error(`sampler exited (${code}), restarting in 1s`); setTimeout(startSampler, 1000); });
}
startSampler();

// AI subscription usage: fetched once at startup, then only on demand (the screen's REFRESH
// button hits /api/ai/refresh). Refreshes closer than 30 s apart are ignored — the Claude
// endpoint rate-limits aggressive callers.
const AI_MIN_GAP_MS = 30 * 1000;
const ai = { cc: { data: null, at: 0, err: 'loading', busy: false }, cx: { data: null, at: 0, err: 'loading', busy: false } };
const AI_FETCH = { cc: claudeUsage, cx: codexUsage };
let aiLastRefresh = 0;
async function pollAi(key) {
  if (ai[key].busy) return;
  ai[key].busy = true;
  try { ai[key] = { data: await AI_FETCH[key](), at: Date.now(), err: null, busy: false }; }
  catch (e) { ai[key] = { ...ai[key], err: e.message.slice(0, 40), busy: false }; console.error(key, 'usage:', e.message); }
}
function refreshAi() {
  if (Date.now() - aiLastRefresh < AI_MIN_GAP_MS) return false;
  aiLastRefresh = Date.now();
  pollAi('cc'); pollAi('cx');
  return true;
}
refreshAi();

// Compact form for the ESP32 (no clock there, so send seconds-until-reset instead of timestamps)
function aiSummary() {
  const now = Math.floor(Date.now() / 1000);
  const w = (x) => x && x.pct != null ? [x.pct, x.reset ? Math.max(0, x.reset - now) : -1] : [-1, -1];
  const one = ({ data, at, err, busy }) => ({
    h5: w(data?.h5), wk: w(data?.wk), plan: data?.plan ?? '',
    age: at ? Math.round((Date.now() - at) / 1000) : -1, err: err ?? '', busy: busy ? 1 : 0,
  });
  return { cc: one(ai.cc), cx: one(ai.cx) };
}

const PAGE = readFileSync(join(DIR, 'page.html'));

createServer((req, res) => {
  const url = req.url.split('?')[0];
  if (url === '/api/now') {
    if (!latest) { res.writeHead(503).end('warming up'); return; }
    // explicit content-length: the ESP32 reads the raw stream and can't handle chunked encoding
    const body = Buffer.from(JSON.stringify({ ...latest, age: Date.now() - latest.t, hcpu: hist.cpu, hmem: hist.mem, hmp: hist.pressure, hsw: hist.swapr, ai: aiSummary() }));
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store', 'content-length': body.length });
    res.end(body);
  } else if (url === '/api/ai/refresh') {
    const started = refreshAi();
    res.writeHead(started ? 202 : 429, { 'content-length': 0 }).end();
  } else if (url === '/') {
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' }).end(PAGE);
  } else {
    res.writeHead(404).end();
  }
}).listen(PORT, '0.0.0.0', () => console.log(`sysmon on http://0.0.0.0:${PORT}`));
