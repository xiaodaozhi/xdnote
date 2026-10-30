/** Finalize static design metadata. No application code or runtime validation. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');
const ROOT = path.resolve(__dirname, '../..');
const DESIGN = path.join(ROOT, 'design');
const read = async p => JSON.parse((await fs.readFile(path.join(ROOT, p), 'utf8')).replace(/^\uFEFF/, ''));
const write = async (p, value) => fs.writeFile(path.join(ROOT, p), JSON.stringify(value, null, 2) + '\n', 'utf8');
const sha = data => crypto.createHash('sha256').update(data).digest('hex');
async function finalize() {
  const catalog = await read('design/sources/ui-suite-v2.json');
  const extension = await read('design/sources/ui-suite-v2-extensions.json');
  for (const b of extension.boards) if (!catalog.boards.some(x => x.id === b.id)) catalog.boards.push(b);
  const receipts = [];
  for (const name of (await fs.readdir(path.join(DESIGN, 'sources/v2-receipts'))).sort()) {
    const r = await read('design/sources/v2-receipts/' + name);
    const b = catalog.boards.find(b => b.id === (r.boardId || r.id));
    if (!b) throw new Error('Unknown receipt ' + r.id);
    if (!r.prompt) {
      r.prompt = catalog.commonPrompt + '\n\n' + catalog.layoutInstructions[b.layout] + '\n\nBOARD: ' + b.title + '\n' + b.frames.map((f, i) => 'Panel ' + (i + 1) + ', label "' + f.title + '": ' + f.description).join('\n\n');
      await write('design/sources/v2-receipts/' + name, r);
    }
    if (r.imageRetained !== false) {
      const bytes = await fs.readFile(path.join(DESIGN, r.path));
      if (r.sha256 !== sha(bytes) || r.bytes !== bytes.length) throw new Error('Receipt integrity ' + r.id);
      r.pixelSize = await sharp(bytes).metadata().then(m => ({ width: m.width, height: m.height, meaning: 'raster-metadata-not-logical-window-size' }));
    }
    r.receiptPath = 'sources/v2-receipts/' + name;
    receipts.push(r);
  }
  for (const b of catalog.boards) {
    const history = receipts.filter(r => (r.boardId || r.id) === b.id).sort((a, z) => Number(a.id.match(/-r(\d+)$/)?.[1] || 1) - Number(z.id.match(/-r(\d+)$/)?.[1] || 1));
    if (!history.length) throw new Error('Missing board ' + b.id);
    const retained = history.filter(r => r.imageRetained !== false);
    if (retained.length !== 1) throw new Error('Expected one current image for ' + b.id);
    const current = retained[0];
    b.path = current.path;
    b.generationStatus = history.length > 1 ? 'generated-revised' : 'generated';
    b.currentReceipt = current.receiptPath;
    b.generationHistory = history.map(r => ({ id: r.id, ...(r.id === current.id ? { path: r.path } : {}), receipt: r.receiptPath, sha256: r.sha256, adopted: r.id === current.id, imageRetained: r.id === current.id }));
    b.bytes = current.bytes;
    b.sha256 = current.sha256;
    b.pixelSize = current.pixelSize;
    delete b.sourceFilename;
    delete b.previousPath;
  }
  const framePriorities = { 'compare-external': 'P1', 'external-dirty-dialog': 'P0', 'external-clean-banner': 'P0', 'rename-inline': 'P1', 'duplicate-name': 'P1', 'trash-confirm': 'P1', 'missing-file': 'P0', 'encoding-unsupported': 'P1', 'large-file-choice': 'P1', 'loading-document': 'P0', 'preview-failed': 'P0', 'settings-search-results': 'P1', 'settings-search-empty': 'P1', 'settings-write-error': 'P0', 'settings-protected-close': 'P0', 'external-deleted-dirty': 'P0', 'external-renamed-clean': 'P1', 'file-busy-save': 'P0', 'reopen-encoding-dirty': 'P1' };
  for (const b of catalog.boards) {
    for (const f of b.frames) f.priority = framePriorities[f.id] || b.tier;
    if (new Set(b.frames.map(f => f.priority)).size > 1) b.tier = 'P0 / P1';
  }
  catalog.boards.find(b => b.id === '03-welcome-blank').frames[1].description = 'Untouched empty source buffer 未命名.md with caret and placeholder, no dirty dot, neutral 未命名 status. Saving asks for the first path; typing changes the status to 未保存.';
  catalog.boards.find(b => b.id === '11-settings-general-editor').frames[0].description = 'General settings: standalone external open, reading default, fixed existing-session focus, native association handoff, auto-save OFF and 1.5 second delay disabled. Close protection is a static enabled lock policy, not a toggle. Changes apply immediately; 完成 closes settings.';
  catalog.boards.find(b => b.id === '18-file-lifecycle').frames[0].description = 'Inline rename input 产品/设计.md contains an invalid slash, shows the filename validation and Enter/Esc hints. Other rows remain stable.';
  const allFrames = catalog.boards.flatMap(b => b.frames);
  if (new Set(allFrames.map(f => f.id)).size !== allFrames.length) throw new Error('Duplicate surface ID');
  catalog.boardCount = catalog.boards.length;
  catalog.surfaceCount = allFrames.length;
  catalog.authority = { geometry: 'docs/visual/tokens.json', behavior: 'docs/product/interaction-spec.md', rasterRole: 'visual-reference-with-approximate-text-and-colors', assetManifest: 'design/assets/ui-v2/manifest.json' };
  catalog.implementationStatus = 'not-started';
  catalog.notes = ['P1 and compatibility studies are design coverage, not additions to the first release commitment.', 'Generation prompts and provenance remain in receipts; only the current board image is retained at a stable path.', 'The approved brand mark was applied locally to the current raster boards; placement rectangles describe image pixels, not component geometry.', 'Empty untouched buffers have no dirty dot; close protection is a fixed policy; reading task boxes do not modify Markdown.', 'Raster glyphs, code syntax and small icons are illustrative. Use product copy and exact editable assets rather than tracing those pixels.'];
  await write('design/sources/ui-suite-v2.json', catalog);
  const surfaces = catalog.boards.flatMap(b => b.frames.map((f, i) => ({ id: f.id, label: f.title, boardId: b.id, boardPath: 'design/' + b.path, frameIndex: i, priority: f.priority, status: 'reference-available', implementationStatus: 'not-started', slicePath: 'design/assets/ui-v2/reference-slices/' + f.id + '.png' })));
  await write('docs/visual/surface-registry-v2.json', { schemaVersion: 2, scope: 'static-design', catalog: 'design/sources/ui-suite-v2.json', surfaceCount: surfaces.length, surfaces });
  const registry = await read('docs/visual/window-registry.json');
  const id = n => 'ui-v2-' + catalog.boards[n - 1].id;
  const updates = { main: [1, 2, 3, 4, 7, 8, 9, 21, 22, 28, 29, 31], standalone: [5, 6, 29], settings: [11, 12, 13, 14, 22, 30], 'unsaved-confirmation': [15], 'external-conflict': [16, 33] };
  for (const w of registry.windows) {
    if (!updates[w.id]) continue;
    w.designAssetIds = updates[w.id].map(id);
    w.designStatus = 'reference-available';
    if (w.id === 'settings') w.referencePages = ['general', 'appearance-and-accent', 'editor', 'reader-and-rendering', 'shortcuts', 'about'];
  }
  const more = [
    ['comparison', '磁盘与当前编辑比较', 'application-window', 'windows.comparison', [16]],
    ['export', '文档导出', 'owner-export-window', 'windows.export', [20, 32]],
    ['file-operation-dialogs', '保存、文件与兼容提示', 'owner-modal-dialog-family', 'windows.standardDialog', [17, 18, 19, 29, 33]],
    ['batch-and-encoding-dialogs', '批量打开与编码提示', 'owner-modal-dialog-family', 'windows.largeDialog', [15, 19, 29, 33]]
  ];
  for (const [wid, label, kind, sizeToken, boards] of more) if (!registry.windows.some(w => w.id === wid)) registry.windows.push({ id: wid, label, kind, sizeToken, sizeRationale: 'Shared proposed logical geometry; content grows or scrolls; no measurement from generated PNG.', designAssetIds: boards.map(id), designStatus: 'reference-available', implementationStatus: 'not-started', nativeValidation: 'not-run' });
  registry.proposedAuxiliaryWindows = true;
  await write('docs/visual/window-registry.json', registry);
  const tokens = await read('docs/visual/tokens.json');
  tokens.validation.fullDarkScreens = 'static-reference-available';
  tokens.validation.nativeVisual = 'not-run';
  await write('docs/visual/tokens.json', tokens);
  const old = await read('design/manifest.json');
  old.schemaVersion = 2;
  old.assets = old.assets.filter(a => !a.id.startsWith('ui-v2-') && !['previous-baseline', 'historical'].includes(a.status));
  old.baselineAssetIds = [id(1), id(5), id(12)];
  old.boardAssetIds = catalog.boards.map(b => 'ui-v2-' + b.id);
  old.notes = ['The adopted suite has 33 static boards and 89 named surfaces; it does not demonstrate a running app.', 'Graphite is a provisional display default. Six accents and both themes remain available in the design.', 'Exact logical geometry and colors come from tokens.json, not generated image pixels.', 'Current boards use stable filenames; prior raster revisions are not retained. Generation provenance remains textual.', 'The folded-light brand direction was approved for the design assets.', 'Native platform integration and application behavior have not been validated by this design task.'];
  for (const r of receipts.filter(r => r.imageRetained !== false)) {
    const b = catalog.boards.find(b => b.id === (r.boardId || r.id));
    const adopted = b.path === r.path;
    const currentId = 'ui-v2-' + b.id;
    old.assets.push({ id: adopted ? currentId : 'ui-v2-' + r.id + '-previous', path: r.path, kind: 'ui-mockup-board', status: adopted ? (old.baselineAssetIds.includes(currentId) ? 'baseline' : 'current') : 'previous-revision', bytes: r.bytes, sha256: r.sha256, pixelSize: r.pixelSize, referenceAssetIds: [], source: { kind: 'built-in-image-generation', sourceFilename: r.sourceFilename, receipt: r.receiptPath }, promptSource: { path: r.receiptPath, jsonPointer: '/prompt' }, surfaceIds: adopted ? b.frames.map(f => f.id) : [] });
  }
  await write('design/manifest.json', old);
  const preface = '# 界面设计覆盖清单\n\n本轮以 **33 张图板、89 个页面与状态**覆盖 xdnote 当前产品范围。所有图均为静态设计；组件状态、六色和明暗变体进一步覆盖共享控件，未开始应用实现。\n\n[完整画廊](../../design/index.html) · [总览图](../../design/hifi/v2/overview.png) · [切图与素材](../../design/assets/README.md)\n\n';
  const table = '| 编号 | 图板 | 页面／状态标识 | 设计范围 |\n| --- | --- | --- | --- |\n' + catalog.boards.map(b => '| ' + b.id.slice(0, 2) + ' | [' + b.title + '](../../design/' + b.path + ') | ' + b.frames.map(f => '`' + f.id + '`').join('、') + ' | ' + b.tier + ' |').join('\n');
  const after = '\n\n## 覆盖与优先级\n\n图板覆盖工作区、独立阅读与编辑、六类设置、菜单、导航、查找、插入、文件打开与保存、外部变化、编码与大文件、复杂 Markdown、资源加载、导出和系统界面交接。浅深色布局共用参数，六种强调色分别选择。\n\n`P0` 表示基础闭环的设计，`P1` 表示提前绘制的后续方案；出现图板不自动提升开发优先级。内容比较和未知编码等兼容设计仍需技术决策。产品行为以[需求](../product/requirements.md)、[交互](../product/interaction-spec.md)和[设置](../product/settings.md)为准。\n\n系统文件选择器、默认应用和平台窗口装饰归操作系统管理；图板 27 是交接示意，不是三平台原生截图。账户、云同步、AI、插件市场、所见即所得和崩溃恢复不进入本轮既定范围。\n\n## 切图交付\n\n每个页面／状态均有原图矩形切片，记录源图摘要和像素坐标；这些切片用于设计对照。可复用素材另提供原始 SVG、透明 PNG 1×／2×／3×、文字内容区和可拉伸边距，精确色值读取[tokens.json](tokens.json)。按钮文字、正文和系统标题栏继续作为文本或平台界面，不烘焙成通用控件皮肤。\n\n当前图板与生成提示词见[来源目录](../../design/sources/README.md)；旧版图片不再保存。原稿中的少量字形与颜色近似不能覆盖精确参数；素材索引将界面参考切片和矢量控件明确标注用途。\n';
  await fs.writeFile(path.join(ROOT, 'docs/visual/screen-inventory.md'), preface + table + after, 'utf8');
  console.log(JSON.stringify({ boards: catalog.boardCount, surfaces: catalog.surfaceCount, currentImages: receipts.filter(r => r.imageRetained !== false).length, provenanceReceipts: receipts.length, stage: 'static-design-only' }));
}
finalize().catch(e => { console.error(e); process.exitCode = 1; });
