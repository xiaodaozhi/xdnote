/** Align the standalone titlebars against the corresponding current workspace boards. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');
const brand = require('./brand-mark.cjs');
const root = path.resolve(__dirname, '../..');
const design = path.join(root, 'design');
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const cases = [
  { id: '05-standalone-light', input: '6ebc85a9c90dc84bf2c2bd385df8a5f30662048169c6b0a706717ee3d6cf7d54', theme: 'light', x: [30, 786], top: 54, oldToolbar: 122, newToolbar: 112, footer: 956, reference: '01-workspace-light', referenceTop: 14, referenceBottom: 72, referenceLeft: 17, logoCrop: { left: 42, top: 30, width: 112, height: 31 } },
  { id: '06-standalone-dark', input: '1fd0158b746ca97aae8e688458462121e3ae3017746417e9eb61c8f7f9688a13', theme: 'dark', x: [30, 788], top: 52, oldToolbar: 115, newToolbar: 102, footer: 954, reference: '02-workspace-dark', referenceTop: 13, referenceBottom: 63, referenceLeft: 17, logoCrop: { left: 38, top: 25, width: 110, height: 29 } },
];
function chrome(c) {
  const dark = c.theme === 'dark', center = (c.top + c.newToolbar) / 2;
  const fg = dark ? '#E6EAF0' : '#1F2937', background = dark ? '#202730' : '#F8F9FA';
  const border = dark ? '#3B424D' : '#DDE2E8', markH = dark ? 20 : 24, wordH = dark ? 14 : 15;
  const font = 'Microsoft YaHei UI,Microsoft YaHei,Segoe UI,sans-serif';
  let body = '';
  c.x.forEach((left, index) => {
    const w = 720, titleY = center;
    body += `<defs><clipPath id="clip-${index}"><rect x="${left}" y="${c.top}" width="${w}" height="80" rx="9"/></clipPath></defs>`;
    body += `<g clip-path="url(#clip-${index})"><rect x="${left + 1}" y="${c.top + 1}" width="${w - 2}" height="${c.newToolbar - c.top - 1}" fill="${background}"/></g>`;
    body += `<g transform="translate(${left + 27} ${center - markH / 2}) scale(${markH / 268})">${brand.mark(c.theme)}</g>`;
    body += `<g transform="translate(${left + 68} ${center - wordH / 2}) scale(${wordH / 195})">${brand.wordmark(brand.palette[c.theme].word)}</g>`;
    body += `<path d="M${left + 151} ${center - 12}v24" stroke="${border}"/>`;
    body += `<text x="${left + 173}" y="${titleY}" dominant-baseline="central" font-family="${font}" font-size="${dark ? 16 : 18}" fill="${fg}">写作，从这里开始.md</text>`;
    if (index === 1) body += `<circle cx="${left + 372}" cy="${center}" r="5" fill="#F5A623"/>`;
    const a = left + w - 165, b = left + w - 101, d = left + w - 35;
    body += `<g fill="none" stroke="${fg}" stroke-width="1.5" stroke-linecap="round"><path d="M${a - 6} ${center}h12"/><rect x="${b - 5.5}" y="${center - 5.5}" width="11" height="11"/><path d="M${d - 5.5} ${center - 5.5}l11 11m0-11l-11 11"/></g>`;
    body += `<path d="M${left + 1} ${c.newToolbar - .5}h${w - 2}" stroke="${border}"/>`;
  });
  return Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="1536" height="1024">${body}</svg>`);
}
async function main() {
  const apply = process.argv.includes('--apply');
  const review = path.join(root, 'build/design-review');
  await fs.mkdir(review, { recursive: true });
  for (const c of cases) {
    const p = path.join(design, 'hifi/v2/boards', c.id + '.png'), before = await fs.readFile(p);
    if (sha(before) !== c.input) throw new Error('Unexpected input ' + c.id);
    const shift = c.oldToolbar - c.newToolbar, layers = [];
    for (const x of c.x) {
      const left = x + 1, width = 718;
      const moving = await sharp(before).extract({ left, top: c.oldToolbar, width, height: c.footer - c.oldToolbar }).png().toBuffer();
      const bottom = await sharp(before).extract({ left, top: c.footer - 2, width, height: 1 }).resize(width, shift, { kernel: 'nearest' }).png().toBuffer();
      layers.push({ input: moving, left, top: c.newToolbar });
      layers.push({ input: bottom, left, top: c.footer - shift });
    }
    layers.push({ input: chrome(c), left: 0, top: 0 });
    const reference = await fs.readFile(path.join(design, 'hifi/v2/boards', c.reference + '.png'));
    const logo = await sharp(reference).extract(c.logoCrop).png().toBuffer();
    for (const x of c.x) layers.push({ input: logo, left: x + c.logoCrop.left - c.referenceLeft, top: c.top + c.logoCrop.top - c.referenceTop });
    const after = await sharp(before).composite(layers).png().toBuffer();
    const oldPixels = await sharp(before).ensureAlpha().raw().toBuffer(), newPixels = await sharp(after).ensureAlpha().raw().toBuffer();
    for (let y = 0; y < 1024; y++) for (let x = 0; x < 1536; x++) {
      if (c.x.some(left => x >= left && x < left + 720 && y >= c.top && y < c.footer)) continue;
      const i = (y * 1536 + x) * 4;
      for (let k = 0; k < 4; k++) if (oldPixels[i+k] !== newPixels[i+k]) throw new Error('Changed outside window content: ' + c.id + ' ' + x + ',' + y);
    }
    const target = apply ? p : path.join(review, c.id + '-aligned.png');
    await fs.writeFile(target, after);
    console.log(JSON.stringify({ id: c.id, inputSha256: c.input, outputSha256: sha(after), bytes: after.length, reference: c.reference, referenceSha256: sha(reference), referenceHeight: c.referenceBottom - c.referenceTop, standaloneHeight: c.newToolbar - c.top, center: (c.top + c.newToolbar) / 2, shiftedBy: shift, applied: apply }));
  }
}
main().catch(e => { console.error(e); process.exitCode = 1; });
