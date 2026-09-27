// AI subscription usage: Claude Code (this Mac's login) and Codex (the real account logged in
// under ~/.codex-usage). Both report % used of a 5-hour and a weekly window plus reset times.
// Tokens are read at call time and never logged or stored.
import { execFile, spawn } from 'node:child_process';
import { homedir, userInfo } from 'node:os';
import { join, dirname } from 'node:path';
import { existsSync } from 'node:fs';

const CODEX_HOME = join(homedir(), '.codex-usage');
// launchd's PATH lacks nvm: use the codex next to this node binary, and put that dir on PATH
// because codex's launcher is itself a node script (#!/usr/bin/env node).
const NODE_BIN = dirname(process.execPath);
const CODEX = existsSync(join(NODE_BIN, 'codex')) ? join(NODE_BIN, 'codex') : 'codex';

// { h5, h5Reset, wk, wkReset } with percentages 0..100 and resets as unix seconds
const win = (pct, reset) => ({ pct: pct == null ? null : Math.round(pct), reset: reset ?? null });

function keychainToken() {
  return new Promise((resolve, reject) => {
    execFile('security', ['find-generic-password', '-s', 'Claude Code-credentials', '-a', userInfo().username, '-w'], { timeout: 60000 }, (err, out) => {
      if (err) return reject(new Error('keychain: ' + (err.killed ? 'timeout (approve the prompt?)' : 'no access')));
      try { const o = JSON.parse(out).claudeAiOauth; resolve({ token: o.accessToken, plan: o.subscriptionType || '' }); } catch { reject(new Error('keychain: unexpected format')); }
    });
  });
}

export async function claudeUsage() {
  const { token, plan } = await keychainToken();
  // Undocumented endpoint used by Claude Code's /usage; rate-limited, so poll sparingly.
  const r = await fetch('https://api.anthropic.com/api/oauth/usage', {
    headers: { authorization: `Bearer ${token}`, 'anthropic-beta': 'oauth-2025-04-20' },
    signal: AbortSignal.timeout(15000),
  });
  if (!r.ok) throw new Error(`claude http ${r.status}`);
  const j = await r.json();
  const ts = (s) => (s ? Math.floor(Date.parse(s) / 1000) : null);
  return { h5: win(j.five_hour?.utilization, ts(j.five_hour?.resets_at)), wk: win(j.seven_day?.utilization, ts(j.seven_day?.resets_at)), plan };
}

// Ask the official Codex CLI (app-server JSON-RPC over stdio); it handles token refresh itself.
export function codexUsage() {
  return new Promise((resolve, reject) => {
    const p = spawn(CODEX, ['app-server'], { env: { ...process.env, CODEX_HOME, PATH: `${NODE_BIN}:${process.env.PATH || '/usr/bin:/bin'}` }, stdio: ['pipe', 'pipe', 'ignore'] });
    const done = (fn, v) => { clearTimeout(t); p.kill(); fn(v); };
    const t = setTimeout(() => done(reject, new Error('codex timeout')), 30000);
    let buf = '';
    p.stdout.on('data', (d) => {
      buf += d;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i); buf = buf.slice(i + 1);
        let m; try { m = JSON.parse(line); } catch { continue; }
        if (m.id === 0) {
          p.stdin.write(JSON.stringify({ method: 'initialized' }) + '\n');
          p.stdin.write(JSON.stringify({ id: 1, method: 'account/rateLimits/read' }) + '\n');
        } else if (m.id === 1) {
          if (m.error) return done(reject, new Error('codex: ' + (m.error.message || 'error')));
          const rl = m.result.rateLimitsByLimitId?.codex || m.result.rateLimits;
          const w = (x) => win(x?.usedPercent, x?.resetsAt);
          // primary/secondary are ordered by window length; label by duration when present
          const wins = [rl.primary, rl.secondary].filter(Boolean).sort((a, b) => (a.windowDurationMins ?? 0) - (b.windowDurationMins ?? 0));
          return done(resolve, { h5: w(wins[0]), wk: w(wins[1]), plan: rl.planType ?? null });
        }
      }
    });
    p.on('error', (e) => done(reject, e));
    p.stdin.write(JSON.stringify({ id: 0, method: 'initialize', params: { clientInfo: { name: 'sysmon', version: '1.0' } } }) + '\n');
  });
}
