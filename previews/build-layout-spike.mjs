import { writeFileSync } from 'node:fs';

// Layout spike only: 800 x 480 SVGs with realistic sample metrics, no live device data.
const ink = { bg: '#081018', panel: '#12202b', raised: '#1a2e3b', track: '#294356',
  text: '#e7ecf0', dim: '#8999a6', blue: '#489eff', purple: '#a377f5', green: '#37d790' };
const box = (x, y, w, h, fill, r = 0) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="${fill}"/>`;
const text = (x, y, content, size = 12, fill = ink.text, anchor = 'start') =>
  `<text x="${x}" y="${y}" font-size="${size}" fill="${fill}" text-anchor="${anchor}">${content}</text>`;
const lines = (points, stroke, width = 2) => `<polyline points="${points}" fill="none" stroke="${stroke}" stroke-width="${width}" stroke-linecap="round" stroke-linejoin="round"/>`;
const svg = (content) => `<svg viewBox="0 0 800 480" width="800" height="480" xmlns="http://www.w3.org/2000/svg" role="img">${box(0, 0, 800, 480, ink.bg)}${content}</svg>`;
const home = (title, dot = 0) => `${text(36, 38, '⌂', 29, '#fff', 'middle')}${text(80, 38, title, 24)}${text(400, 468, dot ? '○   ●' : '●   ○', 12, ink.text, 'middle')}`;

function graph(x, y, w, h, values, stroke) {
  let content = box(x, y, w, h, ink.panel, 8);
  for (let k = 1; k < 4; k++) content += lines(`${x + 12},${y + h * k / 4} ${x + w - 12},${y + h * k / 4}`, '#294356', 1);
  const points = values.map((v, i) => `${(x + 13 + i * (w - 26) / (values.length - 1)).toFixed(1)},${(y + h - 11 - v * (h - 22) / 100).toFixed(1)}`).join(' ');
  return content + lines(points, stroke, 2.5);
}

function system(cores) {
  let content = home('System');
  content += text(770, 34, 'MEMORY OK', 12, ink.green, 'end');
  content += box(16, 72, 246, 98, ink.panel, 10) + box(16, 180, 246, 100, ink.panel, 10) + box(16, 290, 246, 154, ink.panel, 10);
  content += text(30, 99, 'CPU', 12, ink.dim) + text(30, 142, '64%', 32) + text(145, 142, 'load 3.42', 12, ink.dim);
  content += text(30, 204, 'App data / RAM', 12, ink.dim) + text(30, 235, '17.8G / 32G', 24);
  content += text(30, 258, 'zip 9.2G → 4.1G', 12, ink.dim) + text(30, 275, 'ram 25.6G   swap 1.2G', 12, ink.dim);
  content += text(30, 308, 'Top processes', 12, ink.dim);
  [['WindowServer', '34%  802M'], ['Chrome Helper', '19%  620M'], ['kernel_task', '8%  440M'], ['Code Helper', '5%  380M'], ['Finder', '2%  190M']].forEach(([name, value], i) => {
    content += text(30, 335 + i * 23, name, 12, ink.blue);
    content += text(248, 335 + i * 23, value, 12, ink.dim, 'end');
  });
  content += text(302, 79, 'CPU  /  last 2 min', 12, ink.dim);
  const cpu = Array.from({ length: 60 }, (_, i) => Math.min(93, 20 + 18 * Math.sin(i * 0.34) + 24 * Math.max(0, Math.sin(i * 0.11 + 2))));
  content += graph(302, 90, 478, 130, cpu, ink.blue);
  content += text(294, 103, '100', 11, ink.dim, 'end') + text(294, 214, '0', 11, ink.dim, 'end');
  content += box(302, 233, 478, 108, ink.raised, 10);
  content += text(318, 251, 'CPU cores', 12, ink.text) + text(764, 251, `${cores} active`, 12, ink.dim, 'end');
  const vals = [65, 42, 88, 73, 22, 55, 31, 19, 47, 58, 61, 28, 82, 35, 23, 49];
  const slot = 454 / cores, width = Math.min(36, slot - 9);
  for (let i = 0; i < cores; i++) {
    const left = 314 + i * slot + (slot - width) / 2;
    content += box(left, 265, width, 52, ink.track, 3);
    content += box(left, 317 - 52 * vals[i] / 100, width, 52 * vals[i] / 100, ink.blue, 2);
    content += text(314 + slot * (i + 0.5), 332, String(i), 11, ink.dim, 'middle');
  }
  content += text(302, 358, 'Swap I/O    0.2 MB/s   /   0 is healthy', 12, ink.dim);
  const swap = Array.from({ length: 60 }, (_, i) => Math.max(4, 5 + 36 * Math.max(0, Math.sin(i * 0.19) - 0.65)));
  content += graph(302, 367, 478, 77, swap, ink.purple);
  return svg(content);
}

function aiSection(y, name, plan, five, weekly) {
  let content = box(16, y, 768, 184, ink.panel, 10);
  content += text(30, y + 36, name, 24) + text(768, y + 33, `updated 2m ago  /  ${plan}`, 12, ink.dim, 'end');
  for (const [index, value, title, reset] of [[0, five, '5 hour', 'resets in 2h 24m'], [1, weekly, 'Weekly', 'resets in 4d 7h']]) {
    const ry = y + 46 + index * 72;
    content += text(30, ry + 22, title, 18, ink.dim);
    content += box(142, ry, 480, 32, ink.track, 5) + box(142, ry, 480 * value / 100, 32, value >= 70 ? '#feb340' : ink.blue, 5);
    content += text(650, ry + 26, `${value}%`, 24);
    content += text(142, ry + 51, reset, 12, ink.dim);
  }
  return content;
}

const ai = svg(home('AI usage', 1) + box(630, 8, 154, 44, ink.blue, 10) + text(707, 37, 'REFRESH', 18, '#fff', 'middle') + aiSection(72, 'Claude Code', 'Max', 42, 76) + aiSection(264, 'Codex', 'Plus', 18, 53));
const launcher = svg(
  text(32, 61, 'APPS', 32)
  + box(40, 108, 86, 86, '#143356', 18)
  + [24, 40, 56].map((h, i) => box(58 + i * 22, 178 - h, 14, h, i === 2 ? ink.purple : ink.blue, 3)).join('')
  + text(83, 218, 'Monitor', 16, ink.text, 'middle')
);

const page = `<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><title>Sysmon 800×480 布局预览</title>
<style>
@font-face { font-family: PreviewMontserrat; src: url('/font') format('truetype'), url('../firmware-idf/managed_components/lvgl__lvgl/scripts/generators/built_in_font/Montserrat-Medium.ttf') format('truetype'); }
* { box-sizing: border-box; } body { background: #dde5eb; color: #20303f; font: 15px system-ui; margin: 24px; }
.wrap { max-width: 900px; margin: auto; } h1 { font-size: 23px; margin: 0 0 6px; } p { margin: 0 0 18px; line-height: 1.5; }
.controls { display: flex; gap: 9px; flex-wrap: wrap; margin-bottom: 14px; } input { position: absolute; opacity: 0; }
.controls label { display: inline-block; padding: 9px 16px; border-radius: 8px; border: 1px solid #afc0ce; cursor: pointer; background: white; }
input:focus-visible ~ .controls label { outline-offset: 3px; }
#core8:checked ~ .controls label[for=core8], #core16:checked ~ .controls label[for=core16], #ai:checked ~ .controls label[for=ai], #home:checked ~ .controls label[for=home] { background: #142c41; color: white; }
.display { width: 800px; height: 480px; max-width: 100%; margin: auto; background: #081018; overflow: hidden; box-shadow: 0 12px 32px #142c412e; }
.screen { display: none; } .screen svg { display: block; max-width: 100%; height: auto; font-family: PreviewMontserrat, Montserrat, Arial, sans-serif; font-weight: 500; }
#core8:checked ~ .display .core8, #core16:checked ~ .display .core16, #ai:checked ~ .display .ai, #home:checked ~ .display .home { display: block; }
.note { margin-top: 14px; font-size: 13px; color: #46596a; }
</style><main class="wrap"><h1>Sysmon · 800 × 480 实际像素布局</h1><p>切换模拟 8 / 16 核，检查右侧核心负载的宽度与高度；数据为示例。此页是布局 spike，设备目前仍运行上一版。</p>
<input type="radio" name="screen" id="core8" checked><input type="radio" name="screen" id="core16"><input type="radio" name="screen" id="ai"><input type="radio" name="screen" id="home">
<div class="controls"><label for="core8">系统 · 8 核</label><label for="core16">系统 · 16 核</label><label for="ai">AI 用量</label><label for="home">App 首页</label></div>
<div class="display"><div class="screen core8">${system(8)}</div><div class="screen core16">${system(16)}</div><div class="screen ai">${ai}</div><div class="screen home">${launcher}</div></div>
<p class="note">预览使用与固件相同的 800×480 画布与 Montserrat 字体；色块、坐标和内容间距按计划中的 LVGL 控件绘制。曲线与数值是示例数据，最终固件仍读取 Mac 实时数据。</p></main></html>`;

writeFileSync(new URL('./sysmon-layout-spike.html', import.meta.url), page);
