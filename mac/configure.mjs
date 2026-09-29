// Write Wi-Fi + Mac host config into the ESP32 over USB serial.
// Usage: node configure.mjs          (prompts; password input is hidden)
//        node configure.mjs status   (just print device status)
//        node configure.mjs host     (set this Mac's .local name without changing Wi-Fi)
//        node configure.mjs tap x y  (inject a USB-only screen tap)
import { execFileSync } from 'node:child_process';
import { readdirSync, openSync, readSync, writeSync, closeSync } from 'node:fs';
import { createInterface } from 'node:readline';

const port = readdirSync('/dev').filter((f) => f.startsWith('cu.usbmodem')).map((f) => '/dev/' + f)[0];
if (!port) { console.error('ESP32 not found: plug it in via USB-C'); process.exit(1); }
execFileSync('stty', ['-f', port, '115200', 'raw', '-echo', 'min', '0', 'time', '1']);
const fd = openSync(port, 'r+');

const sleep = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
function send(line, timeoutMs = 1500) {
  writeSync(fd, line + '\n');
  const buf = Buffer.alloc(4096); let out = ''; const until = Date.now() + timeoutMs;
  while (Date.now() < until) {
    let n = 0; try { n = readSync(fd, buf, 0, buf.length, null); } catch { n = 0; }
    if (n > 0) { out += buf.subarray(0, n).toString(); if (/^(OK|ERR|ssid=).*\r?\n/m.test(out)) break; } else sleep(50);
  }
  return (out.match(/^(OK|ERR|ssid=).*$/m) || ['(no reply)'])[0].trim();
}

async function ask(q, { hidden = false, def = '' } = {}) {
  const rl = createInterface({ input: process.stdin, output: process.stdout, terminal: true });
  if (hidden) rl._writeToOutput = (s) => { if (s.startsWith(q)) rl.output.write(q); };
  const a = await new Promise((r) => rl.question(q, r));
  rl.close(); if (hidden) process.stdout.write('\n');
  return a.trim() || def;
}

if (process.argv[2] === 'status') { console.log(send('status')); closeSync(fd); process.exit(0); }
if (process.argv[2] === 'tap') {
  const x = Number(process.argv[3]), y = Number(process.argv[4]);
  if (!Number.isInteger(x) || !Number.isInteger(y) || x < 0 || x >= 800 || y < 0 || y >= 480) {
    closeSync(fd);
    console.error('Usage: node mac/configure.mjs tap <x:0-799> <y:0-479>');
    process.exit(1);
  }
  const result = send(`tap ${x} ${y}`, 3000);
  closeSync(fd);
  console.log(result);
  if (result !== 'OK tap') process.exitCode = 1;
  process.exit();
}

const host = execFileSync('scutil', ['--get', 'LocalHostName']).toString().trim() + '.local';
if (process.argv[2] === 'host') {
  console.log('host →', send(`set host ${host}`));
  console.log(send('reboot'));
  closeSync(fd);
  process.exit(0);
}
const ssid = await ask('Wi-Fi 名称 (SSID): ');
const pass = await ask('Wi-Fi 密码 (输入不显示): ', { hidden: true });
const h = await ask(`Mac 地址 [${host}]: `, { def: host });
for (const [k, v] of [['ssid', ssid], ['pass', pass], ['host', h], ['port', '8787']]) console.log(k, '→', send(`set ${k} ${v}`));
console.log(send('reboot'));
closeSync(fd);
console.log('完成。屏幕会在 10 秒内连上 Wi-Fi 并显示数据；查看状态：node configure.mjs status');
