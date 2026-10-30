/** Material integrity only: does not build, run or accept the xdnote application. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');
const ROOT = path.resolve(__dirname, '../..');
const DESIGN = path.join(ROOT, 'design');
const OUT = path.join(DESIGN, 'assets/ui-v2');
const errors = [];
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const read = async p => JSON.parse((await fs.readFile(p, 'utf8')).replace(/^\uFEFF/, ''));
const assert = (condition, message) => { if (!condition) errors.push(message); };
const inside = (root, target) => target === root || target.startsWith(root + path.sep);
async function walk(root) { const out = []; for (const e of await fs.readdir(root, { withFileTypes: true })) { if (e.name === '__pycache__') continue; const p = path.join(root, e.name); if (e.isDirectory()) out.push(...await walk(p)); else out.push(p); } return out; }
async function integrity(item, root) {
  const p = path.resolve(root, item.path);
  assert(inside(root, p), 'Escaping path ' + item.path);
  if (!inside(root, p)) return;
  const data = await fs.readFile(p);
  assert(data.length === item.bytes && sha(data) === item.sha256, 'File identity ' + item.path);
  if (item.pixelSize) { const m = await sharp(data).metadata(); assert(m.width === item.pixelSize.width && m.height === item.pixelSize.height, 'Raster metadata ' + item.path); }
}
async function main() {
  const catalog = await read(path.join(DESIGN, 'sources/ui-suite-v2.json'));
  const material = await read(path.join(DESIGN, 'manifest.json'));
  const assets = await read(path.join(OUT, 'manifest.json'));
  const tokens = await read(path.join(ROOT, 'docs/visual/tokens.json'));
  const geometry = await read(path.join(ROOT, 'docs/visual/asset-geometry.json'));
  const registry = await read(path.join(ROOT, 'docs/visual/window-registry.json'));
  const surfaces = await read(path.join(ROOT, 'docs/visual/surface-registry-v2.json'));
  const brandPlacements = await read(path.join(DESIGN, 'sources/brand-placements.json'));
  const all = [path.join(ROOT, 'README.md'), path.join(ROOT, 'AGENTS.md'), ...await walk(path.join(ROOT, 'docs')), ...await walk(DESIGN)];
  assert(!all.some(p => inside(DESIGN, p) && /\.zip$/i.test(p)), 'Design directory must not contain generated ZIP packages');
  let jsonFiles = 0, localLinks = 0, pngChecks = 0, alphaChecks = 0, exactCrops = 0;
  for (const p of all.filter(p => p.endsWith('.json'))) { await read(p); jsonFiles++; }
  for (const p of all.filter(p => p.endsWith('.md') || p.endsWith('.html'))) {
    const content = await fs.readFile(p, 'utf8');
    assert(!content.includes('\uFFFD'), 'Replacement character ' + path.relative(ROOT, p));
    if (p.endsWith('.md')) assert((content.match(/^```/gm) || []).length % 2 === 0, 'Unpaired fence ' + path.relative(ROOT, p));
    const links = p.endsWith('.html') ? [...content.matchAll(/(?:src|href)="([^"]+)"/g)].map(m => m[1]) : [...content.matchAll(/!?\[[^\]\n]*\]\(([^\n)]+)\)/g)].map(m => m[1].trim().replace(/^<|>$/g, ''));
    for (const link of links) {
      if (/^(?:[a-z]+:|#|\/\/)/i.test(link)) continue;
      const target = path.resolve(path.dirname(p), decodeURIComponent(link.split('#')[0]));
      assert(inside(ROOT, target), 'Nonportable link ' + link);
      try { await fs.access(target); } catch { errors.push('Broken link ' + path.relative(ROOT, p) + ' -> ' + link); }
      localLinks++;
    }
  }
  const assetIds = new Set(material.assets.map(a => a.id));
  assert(assetIds.size === material.assets.length, 'Duplicate top-level asset IDs');
  assert(material.boardAssetIds.length === catalog.boards.length, 'Board material coverage');
  for (const item of material.assets) {
    await integrity(item, DESIGN);
    for (const ref of item.referenceAssetIds || []) assert(assetIds.has(ref), 'Missing material reference ' + ref);
    if (item.promptSource) { const receipt = await read(path.join(DESIGN, item.promptSource.path)); assert(typeof receipt.prompt === 'string' && receipt.prompt.length > 20, 'Missing prompt ' + item.id); }
  }
  for (const item of material.authoritySnapshots) await integrity(item, ROOT);
  for (const w of registry.windows) {
    let size = tokens;
    for (const key of w.sizeToken.split('.')) size = size?.[key];
    assert(size, 'Unknown window token ' + w.id);
    assert(w.implementationStatus === 'not-started' && w.nativeValidation === 'not-run', 'Design phase boundary ' + w.id);
    for (const ref of w.designAssetIds) assert(material.assets.some(a => a.id === ref && ['baseline', 'current'].includes(a.status)), 'Window reference ' + w.id + '/' + ref);
  }
  assert(Object.keys(tokens.accents).length === 6 && geometry.unit === 'logical-pixel', 'Shared parameter contract');
  const frames = catalog.boards.flatMap(b => b.frames);
  assert(frames.length === catalog.surfaceCount && frames.length === surfaces.surfaces.length, 'Surface coverage count');
  assert(new Set(frames.map(f => f.id)).size === frames.length, 'Duplicate surface IDs');
  for (const b of catalog.boards) {
    const data = await fs.readFile(path.join(DESIGN, b.path));
    assert(data.length === b.bytes && sha(data) === b.sha256, 'Current board identity ' + b.id);
    assert(b.path === 'hifi/v2/boards/' + b.id + '.png', 'Stable board filename ' + b.id);
    assert(b.generationHistory.filter(h => h.adopted).length === 1, 'Adopted revision ' + b.id);
    for (const h of b.generationHistory) {
      const r = await read(path.join(DESIGN, h.receipt));
      assert(r.sha256 === h.sha256 && r.prompt.length > 20, 'Receipt identity ' + h.id);
      if (h.adopted) {
        assert(h.path === b.path && r.path === b.path && sha(await fs.readFile(path.join(DESIGN, h.path))) === h.sha256, 'Current receipt image ' + h.id);
        for (const ref of r.references || []) await fs.access(path.join(DESIGN, ref));
      } else assert(h.imageRetained === false && !h.path && r.imageRetained === false && !r.path, 'Retired receipt must not retain an image ' + h.id);
    }
    for (const f of b.frames) assert(surfaces.surfaces.some(s => s.id === f.id && s.boardPath === 'design/' + b.path && s.priority === f.priority), 'Surface mapping ' + f.id);
  }
  const boardFiles = (await fs.readdir(path.join(DESIGN, 'hifi/v2/boards'))).filter(p => p.endsWith('.png')).sort();
  assert(JSON.stringify(boardFiles) === JSON.stringify(catalog.boards.map(b => b.id + '.png').sort()), 'Only current boards should be retained');
  assert(tokens.brand?.status === 'approved-design-direction' && brandPlacements.boards.length === 32, 'Approved brand coverage');
  assert(new Set(brandPlacements.boards.map(b => b.id)).size === brandPlacements.boards.length, 'Duplicate brand placement boards');
  for (const placement of brandPlacements.boards) {
    const b = catalog.boards.find(x => x.id === placement.id);
    assert(b && b.path === placement.path && b.sha256 === (placement.currentBoardSha256 || placement.outputSha256), 'Brand placement source identity ' + placement.id);
    assert(b && b.pixelSize.width === placement.sourcePixelSize.width && b.pixelSize.height === placement.sourcePixelSize.height, 'Brand placement image dimensions ' + placement.id);
    for (const r of placement.regions) assert(r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 && r.x + r.width <= placement.sourcePixelSize.width && r.y + r.height <= placement.sourcePixelSize.height, 'Brand placement bounds ' + placement.id);
  }
  const fileMap = new Map(assets.files.map(f => [f.path, f]));
  assert(fileMap.size === assets.files.length && fileMap.size === assets.counts.files, 'Asset file count');
  assert(new Set(assets.assets.map(a => a.id)).size === assets.assets.length, 'Duplicate reusable asset IDs');
  const actual = (await walk(OUT)).map(p => path.relative(OUT, p).replaceAll('\\', '/')).filter(p => p !== 'manifest.json');
  for (const p of actual) assert(fileMap.has(p), 'Unindexed asset ' + p);
  for (const item of assets.files) await integrity(item, OUT);
  for (const record of assets.assets) {
    for (const p of record.files) assert(fileMap.has(p), 'Asset record file missing ' + p);
    if (record.kind === 'literal-reference-crop') {
      const source = path.join(DESIGN, record.sourcePath);
      assert(sha(await fs.readFile(source)) === record.sourceSha256, 'Crop source identity ' + record.id);
      const m = await sharp(source).metadata();
      const r = record.pixelRect;
      assert(r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 && r.left + r.width <= m.width && r.top + r.height <= m.height && record.resizeApplied === false, 'Crop bounds ' + record.id);
      const first = await sharp(source).extract(r).ensureAlpha().raw().toBuffer();
      const second = await sharp(path.join(OUT, record.files[0])).ensureAlpha().raw().toBuffer();
      assert(first.equals(second), 'Crop pixels differ ' + record.id);
      exactCrops++;
    }
    for (const p of record.files.filter(p => p.endsWith('.png'))) {
      const file = path.join(OUT, p), m = await sharp(file).metadata();
      pngChecks++;
      const density = Number(p.match(/@(\d)x\.png$/)?.[1]);
      if (density && record.logicalCanvas) assert(m.width === record.logicalCanvas.width * density && m.height === record.logicalCanvas.height * density, 'Density geometry ' + p);
      if (['editable-vector-with-transparent-png', 'original-outline-icon', 'approved-brand-vector'].includes(record.kind)) {
        assert(m.hasAlpha, 'Missing alpha channel ' + p);
        const stats = await sharp(file).ensureAlpha().stats();
        assert(stats.channels[3].min === 0, 'Nontransparent outer background ' + p);
        if (!record.id.endsWith('/button-ghost-normal')) assert(stats.channels[3].max > 0, 'Empty artwork ' + p);
        alphaChecks++;
      }
    }
    for (const p of record.files.filter(p => p.endsWith('.svg'))) {
      const source = await fs.readFile(path.join(OUT, p), 'utf8');
      assert(!/<script|@font-face|<image|https?:\/\//i.test(source.replace('http://www.w3.org/2000/svg', '')), 'External or executable SVG content ' + p);
      if (record.textBakedIn === false) assert(!/<text\b/i.test(source), 'Baked control text ' + p);
      await sharp(Buffer.from(source)).metadata();
    }
  }
  for (const f of frames) assert(assets.assets.some(a => a.surfaceId === f.id && a.kind === 'literal-reference-crop'), 'Missing surface slice ' + f.id);
  async function pixel(p, x, y, expected) {
    const data = await sharp(path.join(OUT, p)).ensureAlpha().extract({ left: x, top: y, width: 1, height: 1 }).raw().toBuffer();
    const actual = '#' + [...data.subarray(0, 3)].map(n => n.toString(16).padStart(2, '0')).join('');
    assert(actual.toLowerCase() === expected.toLowerCase() && data[3] === 255, 'Exact color ' + p + ' ' + actual + ' != ' + expected);
  }
  for (const theme of ['light', 'dark']) for (const [name, a] of Object.entries(tokens.accents)) {
    await pixel(`components/${theme}/${name}/button-primary-normal@1x.png`, 52, 20, a[theme].solid);
    await pixel(`components/${theme}/${name}/toggle-on@1x.png`, 33, 16, a[theme].foreground);
  }
  await pixel('feedback/light/banner-error@1x.png', 9, 20, tokens.semantic.light.error);
  assert(exactCrops === assets.counts.referenceSlices, 'Crop count');
  const summary = { scope: 'static-design-material-integrity', boards: catalog.boards.length, surfaces: frames.length, currentBoardImages: boardFiles.length, brandPlacementRegions: brandPlacements.boards.reduce((n, b) => n + b.regions.length, 0), provenanceReceipts: catalog.boards.reduce((n, b) => n + b.generationHistory.length, 0), icons: assets.counts.icons, componentVariants: assets.counts.componentVariants, exactPixelCrops: exactCrops, fileIdentities: assets.files.length, pngDimensionChecks: pngChecks, alphaChecks, jsonFiles, localLinks, errors, applicationTestsRun: false, nativePlatformValidationRun: false };
  await fs.mkdir(path.join(ROOT, 'build/design-v2'), { recursive: true });
  await fs.writeFile(path.join(ROOT, 'build/design-v2/material-check.json'), JSON.stringify(summary, null, 2) + '\n');
  console.log(JSON.stringify(summary));
  if (errors.length) process.exitCode = 1;
}
main().catch(e => { console.error(e); process.exitCode = 1; });
