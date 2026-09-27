// Install/uninstall the sysmon server as a login LaunchAgent (ProcessType=Interactive so it
// keeps getting CPU when the Mac is under heavy load).
// Usage: node install-agent.mjs [uninstall]
import { writeFileSync, rmSync, mkdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const LABEL = 'local.sysmon';
const DIR = dirname(fileURLToPath(import.meta.url));
const plist = join(homedir(), 'Library/LaunchAgents', LABEL + '.plist');
const uid = process.getuid();
const run = (...a) => { try { execFileSync('launchctl', a, { stdio: 'ignore' }); } catch {} };

run('bootout', `gui/${uid}/${LABEL}`);
if (process.argv[2] === 'uninstall') { rmSync(plist, { force: true }); console.log('removed', plist); process.exit(0); }

mkdirSync(dirname(plist), { recursive: true });
const log = join(homedir(), 'Library/Logs/sysmon.log');
writeFileSync(plist, `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>${LABEL}</string>
  <key>ProgramArguments</key><array><string>${process.execPath}</string><string>${join(DIR, 'server.mjs')}</string></array>
  <key>WorkingDirectory</key><string>${DIR}</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardOutPath</key><string>${log}</string>
  <key>StandardErrorPath</key><string>${log}</string>
</dict></plist>
`);
execFileSync('launchctl', ['bootstrap', `gui/${uid}`, plist]);
console.log('installed', plist, '\nlog:', log);
