/** One-time local design edit. Input hashes prevent reapplying to a changed board. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');
const brand = require('./brand-mark.cjs');
const icons = require('./ui-icon-paths.cjs');
const root = path.resolve(__dirname, '../..');
const design = path.join(root, 'design');
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const cases = [
  { id: '05-standalone-light', input: '0efe84f0f4ab19910846cdd5bc81bbafeb94f3022a588f22f739fb3505caa530', theme: 'light', x: [0, 755], headerY: 58, headerH: 60, toolbarY: 126, toolbarH: 58 },
  { id: '06-standalone-dark', input: '3889b275b2417e4e725756ecf97f8e824092cab6e4b18b53e3a83d27216ed4cf', theme: 'dark', x: [0, 758], headerY: 56, headerH: 54, toolbarY: 119, toolbarH: 50 },
];
const rects = c => c.x.flatMap(dx => [
  { left: (c.theme === 'dark' ? 44 : 190) + dx, top: c.headerY, width: c.theme === 'dark' ? 455 : 310, height: c.headerH },
  { left: 48 + dx, top: c.toolbarY, width: 225, height: c.toolbarH },
]);
function overlay(c) {
  const dark = c.theme === 'dark';
  const titleColor = dark ? '#E6EAF0' : '#1F2937';
  const muted = dark ? '#BBC3CD' : '#65738A';
  const header = dark ? '#202730' : '#F8F9FA';
  const toolbar = dark ? '#1C232B' : '#FEFEFE';
  const border = dark ? '#495461' : '#DDE2E8';
  const font = 'Microsoft YaHei UI,Microsoft YaHei,Segoe UI,sans-serif';
  let body = '';
  c.x.forEach((dx, index) => {
    const headerX = (dark ? 44 : 190) + dx;
    body += `<rect x="${headerX}" y="${c.headerY}" width="${dark ? 455 : 310}" height="${c.headerH}" fill="${header}"/>`;
    if (dark) {
      body += `<g transform="translate(${56 + dx} 73) scale(${21 / 268})">${brand.mark('dark')}</g>`;
      body += `<g transform="translate(${92 + dx} 76) scale(${15 / 195})">${brand.wordmark(brand.palette.dark.word)}</g>`;
      body += `<path d="M${176 + dx} 67v27" stroke="${border}"/>`;
    }
    const cy = c.headerY + c.headerH / 2;
    body += `<text x="${203 + dx}" y="${cy}" dominant-baseline="central" font-family="${font}" font-size="18" fill="${titleColor}">写作，从这里开始.md</text>`;
    if (index === 1) body += `<circle cx="${402 + dx}" cy="${cy}" r="5" fill="#F5A623"/>`;
    body += `<rect x="${48 + dx}" y="${c.toolbarY}" width="225" height="${c.toolbarH}" fill="${toolbar}"/>`;
    const btnY = c.toolbarY + (c.toolbarH - 36) / 2;
    body += `<rect x="${52 + dx}" y="${btnY}" width="36" height="36" rx="6" fill="none" stroke="${border}"/>`;
    body += `<g transform="translate(${60 + dx} ${btnY + 8}) scale(${20 / 24})" fill="none" stroke="${titleColor}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${icons.workspace[1]}</g>`;
    body += `<text x="${101 + dx}" y="${c.toolbarY + c.toolbarH / 2}" dominant-baseline="central" font-family="${font}" font-size="16" fill="${muted}">文档 / 个人笔记</text>`;
  });
  return Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="1536" height="1024">${body}</svg>`);
}
async function main() {
  const apply = process.argv.includes('--apply');
  const review = path.join(root, 'build/design-review');
  await fs.mkdir(review, { recursive: true });
  for (const c of cases) {
    const file = path.join(design, 'hifi/v2/boards', c.id + '.png');
    const before = await fs.readFile(file);
    if (sha(before) !== c.input) throw new Error('Unexpected input; refusing to overwrite ' + c.id);
    const after = await sharp(before).composite([{ input: overlay(c) }]).png().toBuffer();
    const oldPixels = await sharp(before).ensureAlpha().raw().toBuffer();
    const newPixels = await sharp(after).ensureAlpha().raw().toBuffer();
    const allowed = rects(c);
    for (let y = 0; y < 1024; y++) for (let x = 0; x < 1536; x++) {
      if (allowed.some(r => x >= r.left && x < r.left + r.width && y >= r.top && y < r.top + r.height)) continue;
      const i = (y * 1536 + x) * 4;
      for (let k = 0; k < 4; k++) if (oldPixels[i + k] !== newPixels[i + k]) throw new Error('Pixel outside edit regions changed: ' + c.id + ' ' + x + ',' + y);
    }
    const target = apply ? file : path.join(review, c.id + '.png');
    await fs.writeFile(target, after);
    console.log(JSON.stringify({ id: c.id, inputSha256: c.input, outputSha256: sha(after), bytes: after.length, regions: allowed, applied: apply, target }));
  }
}
main().catch(e => { console.error(e); process.exitCode = 1; });
