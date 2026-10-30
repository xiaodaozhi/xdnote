/** One-time raster correction; coordinates describe image pixels, not UI geometry. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');

const file = path.resolve(__dirname, '../hifi/v2/boards/05-standalone-light.png');
const expectedInput = '56b09916e291550c2ecdd92bbe86d6929052e57b69b16a4b98f9051912657739';
const regions = [
  { left: 200, top: 71, width: 185, height: 26 },
  { left: 956, top: 71, width: 185, height: 26 },
];
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const background = [248, 249, 250, 255];

async function main() {
  const input = await fs.readFile(file);
  if (hash(input) !== expectedInput) throw new Error('Board has changed; refusing to reapply this edit.');
  const { data: old, info } = await sharp(input).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  if (info.width !== 1536 || info.height !== 1024 || info.channels !== 4) throw new Error('Unexpected image format.');
  const updated = Buffer.from(old);
  let changed = 0;
  for (const r of regions) {
    for (const y of [r.top, r.top + r.height - 1]) {
      for (let x = r.left; x < r.left + r.width; x++) {
        const i = (y * info.width + x) * 4;
        if (background.some((value, channel) => old[i + channel] !== value)) throw new Error('Title crop is not safely bounded.');
      }
    }
    for (let y = r.top + 1; y < r.top + r.height; y++) {
      const dest = (y * info.width + r.left) * 4;
      const source = ((y - 1) * info.width + r.left) * 4;
      old.copy(updated, dest, source, source + r.width * 4);
    }
  }
  for (let y = 0; y < info.height; y++) for (let x = 0; x < info.width; x++) {
    const i = (y * info.width + x) * 4;
    const different = [0, 1, 2, 3].some(channel => old[i + channel] !== updated[i + channel]);
    if (!different) continue;
    if (!regions.some(r => x >= r.left && x < r.left + r.width && y >= r.top && y < r.top + r.height)) {
      throw new Error(`Pixel outside title regions changed: ${x},${y}`);
    }
    changed++;
  }
  if (!changed) throw new Error('No title pixels changed.');
  const output = await sharp(updated, { raw: { width: info.width, height: info.height, channels: 4 } }).png().toBuffer();
  const decoded = await sharp(output).ensureAlpha().raw().toBuffer();
  if (!decoded.equals(updated)) throw new Error('Encoded image pixels differ from the verified result.');
  await fs.writeFile(file, output);
  console.log(JSON.stringify({ inputSha256: expectedInput, outputSha256: hash(output), bytes: output.length,
    changedPixels: changed, regions, verticalShiftImagePixels: 1, scope: 'static-raster-only' }));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
