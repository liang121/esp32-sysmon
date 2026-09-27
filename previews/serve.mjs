import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';

const port = Number(process.env.SYSMON_PREVIEW_PORT || 8798);
const html = new URL('./sysmon-layout-spike.html', import.meta.url);
const font = new URL('../firmware-idf/managed_components/lvgl__lvgl/scripts/generators/built_in_font/Montserrat-Medium.ttf', import.meta.url);
createServer(async (req, res) => {
  const route = req.url?.split('?')[0];
  const file = route === '/' || route === '/sysmon-layout-spike.html' ? html : route === '/font' ? font : null;
  if (!file) { res.writeHead(404).end(); return; }
  try {
    res.writeHead(200, { 'Content-Type': file === font ? 'font/ttf' : 'text/html; charset=utf-8' });
    res.end(await readFile(file));
  } catch { res.writeHead(500).end(); }
}).listen(port, '127.0.0.1', () => console.log(`http://127.0.0.1:${port}`));
