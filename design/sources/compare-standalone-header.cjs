/** Compare the current image titlebars at their original raster scale. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const sharp = require('sharp');
const design = path.resolve(__dirname, '..');
const rows = [
  { file: '01-workspace-light', rect: { left: 17, top: 14, width: 1502, height: 59 }, y: 88, label: '浅色 · 主工作区', labelY: 76 },
  { file: '05-standalone-light', rect: { left: 30, top: 54, width: 720, height: 59 }, y: 186, label: '浅色 · 独立窗口', labelY: 174 },
  { file: '02-workspace-dark', rect: { left: 17, top: 13, width: 1550, height: 51 }, y: 284, label: '深色 · 主工作区', labelY: 272 },
  { file: '06-standalone-dark', rect: { left: 30, top: 52, width: 720, height: 51 }, y: 382, label: '深色 · 独立窗口', labelY: 370 },
];
async function main() {
  const text = rows.map(r => `<text x="24" y="${r.labelY}" font-size="17" fill="#344054">${r.label}</text>`).join('');
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="1600" height="485"><rect width="1600" height="485" fill="#F4F5F7"/><g font-family="Microsoft YaHei UI,Microsoft YaHei,sans-serif"><text x="24" y="35" font-size="25" font-weight="600" fill="#1F2937">主工作区与独立窗口：标题栏对照</text>${text}<text x="24" y="463" font-size="15" fill="#667085">各组使用原图相同像素比例；精确逻辑尺寸以 tokens.json 为准。</text></g></svg>`;
  const layers = [];
  for (const r of rows) layers.push({ input: await sharp(path.join(design, 'hifi/v2/boards', r.file + '.png')).extract(r.rect).png().toBuffer(), left: 24, top: r.y });
  const out = path.join(design, 'hifi/v2/comparisons/standalone-header-alignment.png');
  await fs.mkdir(path.dirname(out), { recursive: true });
  await sharp(Buffer.from(svg)).composite(layers).png().toFile(out);
  console.log(JSON.stringify({ out, lightImageHeight: 58, darkImageHeight: 50, unit: 'reference-image-pixels-not-logical-pixels' }));
}
main().catch(e => { console.error(e); process.exitCode = 1; });
