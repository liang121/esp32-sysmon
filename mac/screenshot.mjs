// Capture the currently displayed ESP32 RGB565 frame over USB as a local PNG.
// Usage: node mac/screenshot.mjs /absolute/path/screen.png [/dev/cu.usbmodemXXXX]
import { execFileSync } from 'node:child_process';
import { closeSync, openSync, readSync, readdirSync, writeFileSync, writeSync } from 'node:fs';
import { dirname, isAbsolute, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { rgb565ToPng, validateFrame } from './screenshot-format.mjs';

const output = process.argv[2];
const repository = resolve(dirname(fileURLToPath(import.meta.url)), '..');
if (!output || !isAbsolute(output) || !output.toLowerCase().endsWith('.png')) {
  throw new Error('Usage: node mac/screenshot.mjs /absolute/path/screen.png [/dev/cu.usbmodemXXXX]');
}
const location = relative(repository, output);
if (!location.startsWith('..') && !isAbsolute(location)) {
  throw new Error('Save screenshots outside the source repository');
}
const ports = readdirSync('/dev').filter((name) => name.startsWith('cu.usbmodem')).map((name) => '/dev/' + name);
const port = process.argv[3] || (ports.length === 1 ? ports[0] : null);
if (!port) throw new Error(`Expected one ESP32 USB port; found ${ports.length}. Pass the port explicitly.`);
execFileSync('stty', ['-f', port, '115200', 'raw', '-echo', 'min', '0', 'time', '1']);
const fd = openSync(port, 'r+');
const pause = () => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10);
const scratch = Buffer.alloc(8192);

function readBytes(limit) {
  const n = readSync(fd, scratch, 0, Math.min(limit, scratch.length), null);
  if (!n) pause();
  return scratch.subarray(0, n);
}

function readScreenshot() {
  writeSync(fd, 'screenshot\n');
  const marker = Buffer.from('FRAME RGB565 ');
  const headerDeadline = Date.now() + 10000;
  let prefix = Buffer.alloc(0);
  let headerEnd = -1;
  let start = -1;
  while (Date.now() < headerDeadline) {
    prefix = Buffer.concat([prefix, readBytes(scratch.length)]);
    start = prefix.indexOf(marker);
    if (start >= 0) headerEnd = prefix.indexOf(10, start);
    if (headerEnd >= 0) break;
    if (prefix.length > 8192) prefix = prefix.subarray(-1024);
  }
  if (headerEnd < 0) throw new Error('No screenshot frame received from ESP32');
  const header = prefix.subarray(start, headerEnd).toString('ascii').trim();
  const match = /^FRAME RGB565 (\d+) (\d+) (\d+) ([0-9a-fA-F]{8})$/.exec(header);
  if (!match) throw new Error('Invalid screenshot header');
  const width = Number(match[1]);
  const height = Number(match[2]);
  const length = Number(match[3]);
  if (width < 1 || height < 1 || width > 4096 || height > 4096 || length !== width * height * 2) {
    throw new Error('Invalid screenshot dimensions');
  }
  const raw = Buffer.allocUnsafe(length);
  const buffered = prefix.subarray(headerEnd + 1);
  let offset = Math.min(buffered.length, length);
  buffered.copy(raw, 0, 0, offset);
  const bodyDeadline = Date.now() + 120000;
  while (offset < length && Date.now() < bodyDeadline) {
    const chunk = readBytes(length - offset);
    chunk.copy(raw, offset);
    offset += chunk.length;
  }
  if (offset !== length) throw new Error(`Incomplete screenshot: received ${offset}/${length} bytes`);
  validateFrame(raw, width, height, Number.parseInt(match[4], 16));
  return { raw, width, height };
}

try {
  let frame;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      frame = readScreenshot();
      break;
    } catch (error) {
      if (!error.message.includes('checksum') || attempt === 2) throw error;
      console.error('Screenshot checksum mismatch; retrying capture');
    }
  }
  const png = rgb565ToPng(frame.raw, frame.width, frame.height);
  writeFileSync(output, png, { flag: 'wx', mode: 0o600 });
  console.log(`Saved ${frame.width}x${frame.height} screenshot: ${output}`);
} finally {
  closeSync(fd);
}
