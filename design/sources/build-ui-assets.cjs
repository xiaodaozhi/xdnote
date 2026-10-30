/** Static art export and requested image slicing. Requires Node.js and sharp. */
'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');
const iconPaths = require('./ui-icon-paths.cjs');
const brandIdentity = require('./brand-mark.cjs');
const ROOT = path.resolve(__dirname, '../..');
const DESIGN = path.join(ROOT, 'design');
const OUT = path.join(DESIGN, 'assets/ui-v2');
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const esc = s => String(s).replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;');
const json = async p => JSON.parse((await fs.readFile(path.join(ROOT, p), 'utf8')).replace(/^\uFEFF/, ''));
const svg = (w, h, body, title = '') => `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${title ? '<title>' + esc(title) + '</title>' : ''}${body}</svg>`;
const rect = (x, y, w, h, r, fill, stroke = 'none', sw = 1) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="${fill}" stroke="${stroke}" stroke-width="${sw}"/>`;
const circle = (x, y, r, fill, stroke = 'none', sw = 1) => `<circle cx="${x}" cy="${y}" r="${r}" fill="${fill}" stroke="${stroke}" stroke-width="${sw}"/>`;
const line = (d, stroke, sw = 1.5) => `<path d="${d}" fill="none" stroke="${stroke}" stroke-width="${sw}" stroke-linecap="round" stroke-linejoin="round"/>`;
const text = (x, y, label, size = 14, color = '#1F2937', weight = 400) => `<text x="${x}" y="${y}" fill="${color}" font-family="Microsoft YaHei UI,Microsoft YaHei,Segoe UI,sans-serif" font-size="${size}" font-weight="${weight}">${esc(label)}</text>`;
const inner = source => source.replace(/^<svg[^>]*>/, '').replace(/<\/svg>$/, '').replace(/<title>.*?<\/title>/, '');
const place = (source, x, y, scale = 1) => `<g transform="translate(${x} ${y}) scale(${scale})">${inner(source)}</g>`;
const records = [];
const files = new Map();
let T, G, C;
async function put(relative, data) {
  const p = path.join(OUT, relative);
  await fs.mkdir(path.dirname(p), { recursive: true });
  const bytes = Buffer.isBuffer(data) ? data : Buffer.from(data);
  await fs.writeFile(p, bytes);
  const entry = { path: relative.replaceAll('\\', '/'), bytes: bytes.length, sha256: hash(bytes) };
  files.set(entry.path, entry);
  return entry;
}
async function vectorAsset(id, category, source, w, h, metadata = {}) {
  const base = `${category}/${id}`;
  const variants = [await put(base + '.svg', source)];
  for (const density of G.densityExports) {
    const png = await sharp(Buffer.from(source)).resize(w * density, h * density).ensureAlpha().png().toBuffer();
    variants.push({ ...await put(base + `@${density}x.png`, png), density, width: w * density, height: h * density });
  }
  records.push({ id: category + '/' + id, kind: 'editable-vector-with-transparent-png', logicalCanvas: { width: w, height: h }, files: variants.map(v => v.path), ...metadata });
}
function glyph(name, color, size = 16, x = 0, y = 0) {
  return `<g transform="translate(${x} ${y}) scale(${size / G.icon.viewBox})" stroke="${color}" stroke-width="${G.icon.strokeWidth}" fill="none" stroke-linecap="round" stroke-linejoin="round" color="${color}">${iconPaths[name][1]}</g>`;
}
function focused(w, h, color, r = T.radius.control) { return rect(1, 1, w - 2, h - 2, r + 3, 'none', color, T.controls.focusRingWidth); }
function disabledOpacity(content, disabled) { return disabled ? `<g opacity="0.38">${content}</g>` : content; }
function spinner(x, y, color) { return `<g transform="translate(${x} ${y})">${circle(0, 0, 6, 'none', color, 1.5).replace('stroke-width="1.5"', 'stroke-width="1.5" opacity=".25"')}${line('M0-6a6 6 0 0 1 6 6', color)}</g>`; }
function button(theme, accent, state, style = 'primary') {
  const t = T.themes[theme], a = T.accents[accent][theme], pad = G.focusPadding, h = T.controls.height, w = G.button.width;
  const fill = style === 'primary' ? (state === 'hover' ? a.hover : state === 'pressed' ? a.pressed : a.solid) : style === 'danger' ? (state === 'hover' || state === 'pressed' ? t.codeSurface : t.surface) : style === 'ghost' && state === 'normal' ? 'none' : state === 'hover' || state === 'pressed' ? t.sidebar : t.surface;
  const stroke = style === 'primary' || style === 'ghost' ? 'none' : style === 'danger' ? T.semantic[theme].error : t.border;
  const fg = style === 'primary' ? a.foreground : style === 'danger' ? T.semantic[theme].error : t.text;
  let body = rect(pad + .5, pad + .5, w - 1, h - 1, T.radius.control, fill, stroke);
  if (state === 'busy') body += spinner(20, 20, fg);
  body = disabledOpacity(body, state === 'disabled');
  if (state === 'focus') body += focused(w + pad * 2, h + pad * 2, a.solid);
  return { svg: svg(w + pad * 2, h + pad * 2, body), width: w + pad * 2, height: h + pad * 2, coreRect: { x: pad, y: pad, width: w, height: h }, contentRect: { x: state === 'busy' ? 32 : 16, y: 4, width: state === 'busy' ? 64 : 72, height: h }, textColor: fg, nineSlice: { left: 12, top: 12, right: 12, bottom: 12 } };
}
function input(theme, state) {
  const t = T.themes[theme], a = T.accents.graphite[theme], p = G.focusPadding, w = G.input.width, h = T.controls.height;
  const border = state === 'error' ? T.semantic[theme].error : state === 'focus' ? a.solid : t.border;
  let body = rect(p + .5, p + .5, w - 1, h - 1, T.radius.control, state === 'readonly' ? t.sidebar : t.surface, border);
  if (state === 'error') body += glyph('error', border, 16, w - 18, 12);
  body = disabledOpacity(body, state === 'disabled');
  if (state === 'focus') body += focused(w + 8, h + 8, a.solid);
  return { svg: svg(w + 8, h + 8, body), width: w + 8, height: h + 8, coreRect: { x: p, y: p, width: w, height: h }, contentRect: { x: 16, y: 4, width: state === 'error' ? w - 44 : w - 24, height: h }, nineSlice: { left: 12, top: 12, right: 12, bottom: 12 } };
}
function choice(theme, accent, family, state) {
  const t = T.themes[theme], a = T.accents[accent][theme];
  const selected = ['on', 'checked', 'mixed', 'focus', 'disabled-on'].includes(state);
  const disabled = state.startsWith('disabled');
  let w = 24, h = 24, body = '';
  if (family === 'checkbox') {
    body = rect(4.5, 4.5, 15, 15, G.checkbox.radius, selected ? a.solid : t.surface, selected ? a.solid : t.textSecondary);
    if (selected) body += state === 'mixed' ? line('M8 12h8', a.foreground, 1.8) : line('m8 12 3 3 5-6', a.foreground, 1.8);
    if (state === 'focus') body += focused(24, 24, a.solid, 3);
  } else if (family === 'radio') {
    body = circle(12, 12, 7.5, t.surface, selected ? a.solid : t.textSecondary);
    if (selected) body += circle(12, 12, 4, a.solid);
    if (state === 'focus') body += circle(12, 12, 11, 'none', a.solid, 2);
  } else if (family === 'toggle') {
    w = G.toggle.canvasWidth; h = G.toggle.canvasHeight;
    body = rect(4, 5, G.toggle.trackWidth, G.toggle.trackHeight, 11, selected ? a.solid : t.textSecondary);
    body += circle(selected ? 33 : 15, 16, G.toggle.thumbSize / 2, selected ? a.foreground : t.surface);
    if (state === 'focus') body += rect(1, 2, 46, 28, 14, 'none', a.solid, 2);
  }
  return { svg: svg(w, h, disabledOpacity(body, disabled)), width: w, height: h, hitTargetMinimum: T.controls.iconButtonHit, stretch: 'none', stateMeaning: selected ? 'selected' : 'unselected' };
}
function selected(theme, accent, family, state = 'normal') {
  const t = T.themes[theme], a = T.accents[accent][theme];
  if (family === 'segment') return { svg: svg(96, 40, rect(4, 4, G.segment.width, 32, 6, a.solid)), width: 96, height: 40, contentRect: { x: 16, y: 4, width: 64, height: 32 }, textColor: a.foreground, nineSlice: { left: 12, top: 12, right: 12, bottom: 12 } };
  if (family === 'row') return { svg: svg(232, 40, rect(4, 4, G.selectionRow.width, 32, 4, a.soft) + rect(4, 6, G.selectionRow.markerWidth, 28, 1.5, a.solid)), width: 232, height: 40, contentRect: { x: 36, y: 4, width: 180, height: 32 }, textColor: a.softForeground, nineSlice: { left: 12, top: 8, right: 8, bottom: 8 } };
  if (family === 'tab') {
    const body = rect(4.5, 4.5, G.tab.width - 1, G.tab.height - 1, 6, t.surface, t.border) + rect(4, 38, 160, G.tab.indicatorHeight, 1, a.solid) + (state === 'dirty' ? circle(145, 21, 3, T.semantic[theme].warning) : '');
    return { svg: svg(168, 44, body), width: 168, height: 44, contentRect: { x: 28, y: 4, width: 108, height: 32 }, nineSlice: { left: 12, top: 12, right: 28, bottom: 8 } };
  }
  if (family === 'swatch') {
    let body = circle(16, 16, 12, T.accents[accent].base);
    if (state === 'selected') body += circle(16, 16, 15, 'none', a.solid, 2) + line('m10 16 4 4 8-8', '#FFFFFF', 2);
    return { svg: svg(32, 32, body), width: 32, height: 32, stretch: 'none', selectionMeaning: state };
  }
  if (family === 'focus-ring') return { svg: svg(40, 40, focused(40, 40, a.solid)), width: 40, height: 40, nineSlice: { left: 12, top: 12, right: 12, bottom: 12 } };
  throw new Error(family);
}
async function component(id, family, state, theme, accent, definition) {
  const { svg: source, width, height, ...meta } = definition;
  await vectorAsset(id, `components/${theme}/${accent}`, source, width, height, { family, state, theme, accent, textBakedIn: false, ...meta });
}
async function icons() {
  for (const [name, [label, paths]] of Object.entries(iconPaths)) {
    const base = svg(T.controls.iconSize, T.controls.iconSize, `<g transform="scale(${T.controls.iconSize / G.icon.viewBox})" fill="none" stroke="currentColor" stroke-width="${G.icon.strokeWidth}" stroke-linecap="round" stroke-linejoin="round">${paths}</g>`, label);
    const names = [(await put(`icons/svg/${name}.svg`, base)).path];
    for (const theme of ['light', 'dark']) for (const d of G.densityExports) {
      const source = base.replaceAll('currentColor', T.themes[theme].text);
      const png = await sharp(Buffer.from(source)).resize(16 * d, 16 * d).ensureAlpha().png().toBuffer();
      names.push((await put(`icons/png/${theme}/${name}@${d}x.png`, png)).path);
    }
    records.push({ id: 'icon/' + name, kind: 'original-outline-icon', accessibleLabel: label, logicalCanvas: { width: 16, height: 16 }, svgViewBox: 24, strokeWidthInViewBox: G.icon.strokeWidth, color: 'currentColor', files: names, hitTargetMinimum: T.controls.iconButtonHit });
  }
}
async function components() {
  for (const theme of ['light', 'dark']) {
    for (const accent of Object.keys(T.accents)) {
      for (const state of ['normal', 'hover', 'pressed', 'focus', 'disabled', 'busy']) await component('button-primary-' + state, 'button-primary', state, theme, accent, button(theme, accent, state));
      for (const state of ['checked', 'mixed', 'focus']) await component('checkbox-' + state, 'checkbox', state, theme, accent, choice(theme, accent, 'checkbox', state));
      for (const state of ['on', 'focus']) await component('radio-' + state, 'radio', state, theme, accent, choice(theme, accent, 'radio', state));
      for (const state of ['on', 'focus', 'disabled-on']) await component('toggle-' + state, 'toggle', state, theme, accent, choice(theme, accent, 'toggle', state));
      for (const family of ['segment', 'row', 'focus-ring']) await component(family + '-selected', family, 'selected', theme, accent, selected(theme, accent, family));
      for (const state of ['normal', 'dirty']) await component('tab-' + state, 'tab', state, theme, accent, selected(theme, accent, 'tab', state));
      for (const state of ['selected', 'unselected']) await component('swatch-' + state, 'swatch', state, theme, accent, selected(theme, accent, 'swatch', state));
    }
    for (const style of ['secondary', 'ghost', 'danger']) for (const state of ['normal', 'hover', 'pressed', 'focus', 'disabled', 'busy']) await component('button-' + style + '-' + state, 'button-' + style, state, theme, 'neutral', button(theme, 'graphite', state, style));
    for (const state of ['normal', 'focus', 'error', 'readonly', 'disabled']) await component('input-' + state, 'input', state, theme, 'neutral', input(theme, state));
    for (const family of ['checkbox', 'radio', 'toggle']) for (const state of ['off', 'disabled-off']) await component(family + '-' + state, family, state, theme, 'neutral', choice(theme, 'graphite', family, state));
    const t = T.themes[theme];
    for (const [name, color] of Object.entries(T.semantic[theme])) {
      const icon = name === 'success' ? 'check-circle' : name === 'warning' ? 'warning' : 'error';
      await vectorAsset('status-' + name, `feedback/${theme}`, svg(24, 24, glyph(icon, color, 20, 2, 2)), 24, 24, { semantic: name, theme, textBakedIn: false, stretch: 'none' });
      await vectorAsset('banner-' + name, `feedback/${theme}`, svg(336, 56, rect(8.5, 8.5, 319, 39, 6, t.surface, color) + rect(8, 12, 3, 32, 1.5, color)), 336, 56, { semantic: name, theme, contentRect: { x: 40, y: 8, width: 252, height: 40 }, nineSlice: { left: 16, top: 16, right: 16, bottom: 16 }, textBakedIn: false });
    }
    await vectorAsset('dirty-dot', `feedback/${theme}`, svg(16, 16, circle(8, 8, 3, T.semantic[theme].warning)), 16, 16, { semantic: 'dirty', theme, textBakedIn: false, stretch: 'none' });
    await vectorAsset('popup-surface', `surfaces/${theme}`, svg(336, 216, rect(8.5, 8.5, 319, 199, T.radius.menu, t.surface, t.border)), 336, 216, { theme, textBakedIn: false, nineSlice: { left: 20, top: 20, right: 20, bottom: 20 } });
    await vectorAsset('scrollbar-thumb', `surfaces/${theme}`, svg(12, 80, rect(3, 4, 6, 72, 3, t.textSecondary)), 12, 80, { theme, nineSlice: { left: 4, top: 8, right: 4, bottom: 8 }, textBakedIn: false });
    await vectorAsset('separator-horizontal', `surfaces/${theme}`, svg(80, 8, rect(0, 3.5, 80, 1, 0, t.border)), 80, 8, { theme, stretch: 'horizontal', textBakedIn: false });
  }
}
async function emptyStates() {
  for (const theme of ['light', 'dark']) {
    const t = T.themes[theme];
    const paper = rect(48, 17, 56, 78, 5, t.surface, t.textSecondary, 2) + line('M84 17v20h20M61 52h30M61 63h24M61 74h18', t.textSecondary, 2);
    const shapes = {
      'empty-document': paper,
      'empty-folder': line('M29 40h43l9 10h49v48H29ZM29 40V26h41l10 14', t.textSecondary, 2),
      'search-empty': paper + circle(107, 81, 18, t.surface, t.textSecondary, 2) + line('m120 94 15 15', t.textSecondary, 3),
      'file-missing': paper + glyph('error', t.textSecondary, 36, 90, 72),
      'read-only': paper + glyph('lock', t.textSecondary, 38, 90, 70),
      'preview-unavailable': paper + glyph('warning', t.textSecondary, 38, 92, 72),
      'image-unavailable': rect(30, 25, 100, 70, 6, t.surface, t.textSecondary, 2) + glyph('image-off', t.textSecondary, 48, 56, 36),
      'external-conflict': rect(30, 25, 54, 76, 5, t.surface, t.textSecondary, 2) + rect(76, 15, 54, 76, 5, t.surface, t.textSecondary, 2) + glyph('compare', t.textSecondary, 28, 66, 54)
    };
    for (const [name, body] of Object.entries(shapes)) await vectorAsset(name, `empty-states/${theme}`, svg(G.emptyState.width, G.emptyState.height, body), 160, 120, { theme, stretch: 'none', source: 'original-geometric-artwork', textBakedIn: false });
  }
}
async function brand() {
  const definitions = [
    ['mark-light', brandIdentity.markSvg('light'), 32, 32],
    ['mark-dark', brandIdentity.markSvg('dark'), 32, 32],
    ['logo-light', brandIdentity.logoSvg('light'), 256, 64],
    ['logo-dark', brandIdentity.logoSvg('dark'), 256, 64],
    ['app-icon', brandIdentity.appSvg(), 128, 128]
  ];
  for (const [name, source, width, height] of definitions) {
    const names = [(await put('brand/' + name + '.svg', source)).path];
    if (name === 'app-icon') {
      for (const size of [16, 24, 32, 48, 64, 128, 256, 512, 1024]) names.push((await put(`brand/png/${name}-${size}.png`, await sharp(Buffer.from(source)).resize(size, size).ensureAlpha().png().toBuffer())).path);
    } else {
      for (const density of G.densityExports) names.push((await put(`brand/png/${name}@${density}x.png`, await sharp(Buffer.from(source)).resize(width * density, height * density).ensureAlpha().png().toBuffer())).path);
    }
    records.push({ id: 'brand/' + name, kind: 'approved-brand-vector', approval: 'approved-design-direction', files: names, source: 'design/sources/brand-mark.cjs', stretch: 'none' });
  }
}
async function crop(source, rectSpec, relative, record) {
  const bytes = await fs.readFile(path.join(DESIGN, source));
  const m = await sharp(bytes).metadata();
  const left = Math.floor(rectSpec[0] * m.width), top = Math.floor(rectSpec[1] * m.height);
  const right = Math.min(m.width, Math.ceil(rectSpec[2] * m.width)), bottom = Math.min(m.height, Math.ceil(rectSpec[3] * m.height));
  const pixelRect = { left, top, width: right - left, height: bottom - top };
  if (pixelRect.width <= 0 || pixelRect.height <= 0) throw new Error('Invalid crop');
  const data = await sharp(bytes).extract(pixelRect).png().toBuffer();
  const f = await put(relative, data);
  records.push({ ...record, kind: 'literal-reference-crop', runtimeUse: false, sourcePath: source, sourceSha256: hash(bytes), sourcePixelSize: { width: m.width, height: m.height }, pixelRect, resizeApplied: false, files: [f.path] });
}
async function slices() {
  for (const b of C.boards) {
    let rectangles;
    if (b.layout === 'single') rectangles = [[0, 0, 1, 1]];
    else if (b.layout === 'two') rectangles = [[0, 0, .5, 1], [.5, 0, 1, 1]];
    else if (b.layout === 'top-wide-two') rectangles = [[0, 0, 1, .65], [0, .65, .44, 1], [.44, .65, 1, 1]];
    else {
      const rows = b.layout === 'six' ? [0, .35, .70, 1] : [0, b.id === '09-file-tab-menus' ? .60 : b.id === '18-file-lifecycle' ? .52 : .5, 1];
      rectangles = [];
      for (let row = 0; row < rows.length - 1; row++) for (let col = 0; col < 2; col++) rectangles.push([col / 2, rows[row], (col + 1) / 2, rows[row + 1]]);
    }
    if (rectangles.length !== b.frames.length) throw new Error('Slice count ' + b.id);
    for (let i = 0; i < b.frames.length; i++) await crop(b.path, rectangles[i], 'reference-slices/' + b.frames[i].id + '.png', { id: 'surface/' + b.frames[i].id, boardId: b.id, surfaceId: b.frames[i].id, label: b.frames[i].title, cropRole: 'complete-panel-with-context-and-caption' });
  }
  const defs = [
    ['primary-normal', [.254, .133, .365, .19]], ['primary-hover', [.376, .133, .487, .19]], ['primary-pressed', [.496, .133, .608, .19]], ['primary-focus', [.616, .13, .73, .193]], ['primary-disabled', [.738, .133, .85, .19]], ['primary-busy', [.864, .133, .977, .19]],
    ['secondary-normal', [.254, .197, .365, .254]], ['danger-normal', [.254, .26, .365, .321]],
    ['input-normal', [.173, .395, .372, .458]], ['input-focus', [.376, .395, .566, .458]], ['input-error', [.577, .395, .767, .458]], ['input-readonly', [.781, .395, .976, .458]],
    ['toggle-off', [.173, .552, .218, .594]], ['toggle-on', [.238, .552, .283, .594]],
    ['checkbox-off', [.391, .552, .417, .594]], ['checkbox-on', [.444, .552, .47, .594]], ['checkbox-mixed', [.497, .552, .523, .594]], ['checkbox-disabled', [.55, .552, .576, .594]],
    ['radio-off', [.617, .552, .643, .594]], ['radio-on', [.675, .552, .701, .594]], ['segment', [.801, .552, .976, .61]],
    ['tabs', [.171, .72, .406, .783]], ['menu-open', [.846, .72, .978, .95]],
    ['toast-success', [.174, .842, .369, .912]], ['toast-warning', [.378, .842, .6, .912]], ['toast-error', [.611, .842, .831, .912]]
  ];
  for (const [theme, number] of [['light', 23], ['dark', 24]]) for (const [name, box] of defs) {
    const b = C.boards[number - 1];
    await crop(b.path, box, `reference-components/${theme}/${name}.png`, { id: `component-reference/${theme}/${name}`, label: name, boardId: b.id, cropRole: 'component-appearance-only-no-reuse-as-runtime-skin' });
  }
}
async function previews() {
  for (const theme of ['light', 'dark']) {
    const t = T.themes[theme];
    const entries = Object.entries(iconPaths), cols = 10, rows = Math.ceil(entries.length / cols);
    let body = rect(0, 0, 1200, 96 + rows * 96, 0, t.background) + text(32, 42, `xdnote · ${theme === 'light' ? '浅色' : '深色'}图标`, 24, t.text, 600) + text(32, 69, '原始几何 SVG · 实际逻辑尺寸 16 · 此处放大展示', 13, t.textSecondary);
    entries.forEach(([name], i) => { const x = i % cols * 120, y = 96 + Math.floor(i / cols) * 96; body += glyph(name, t.text, 28, x + 46, y + 10) + text(x + 8, y + 64, name, 11, t.textSecondary); });
    await put(`previews/icons-${theme}.png`, await sharp(Buffer.from(svg(1200, 96 + rows * 96, body))).png().toBuffer());
    let controls = rect(0, 0, 1280, 990, 0, t.background) + text(32, 44, `xdnote · ${theme === 'light' ? '浅色' : '深色'}控件`, 24, t.text, 600) + text(32, 72, 'SVG 与透明 PNG 同源；以下文字仅用于图册展示，不写入控件素材。', 14, t.textSecondary);
    ['默认', '悬停', '按下', '焦点', '禁用', '处理中'].forEach((s, i) => controls += text(210 + i * 170, 118, s, 13, t.textSecondary));
    for (const [ri, style] of ['primary', 'secondary', 'ghost', 'danger'].entries()) {
      controls += text(32, 163 + ri * 70, ['主要按钮', '次要按钮', '无底按钮', '危险操作'][ri], 14, t.text);
      ['normal', 'hover', 'pressed', 'focus', 'disabled', 'busy'].forEach((state, i) => { const d = button(theme, 'graphite', state, style), x = 190 + i * 170, y = 134 + ri * 70; const label = state === 'busy' ? '处理中' : style === 'danger' ? '移到回收站' : '保存'; controls += place(d.svg, x, y) + text(x + (state === 'busy' ? 35 : style === 'danger' ? 17 : 38), y + 25, label, 14, state === 'disabled' && style !== 'primary' ? t.textSecondary : d.textColor); });
    }
    ['normal', 'focus', 'error', 'readonly', 'disabled'].forEach((state, i) => { const x = 32 + (i % 3) * 414, y = 470 + Math.floor(i / 3) * 84; const d = input(theme, state); controls += text(x, y - 8, ['输入框', '键盘焦点', '错误', '只读', '禁用'][i], 13, t.textSecondary) + place(d.svg, x, y) + text(x + 16, y + 25, '输入内容', 14, t.textSecondary); });
    ['checkbox', 'radio', 'toggle'].forEach((family, fi) => { controls += text(32, 700 + fi * 70, ['复选框', '单选框', '开关'][fi], 14, t.text); ['off', family === 'checkbox' ? 'checked' : 'on', 'focus', 'disabled-off'].forEach((state, si) => { const d = choice(theme, 'graphite', family, state); controls += place(d.svg, 200 + si * 190, 678 + fi * 70) + text(250 + si * 190, 698 + fi * 70, state, 12, t.textSecondary); }); });
    controls += text(32, 935, '已保存', 14, T.semantic[theme].success) + text(190, 935, '未保存', 14, T.semantic[theme].warning) + text(350, 935, '保存失败', 14, T.semantic[theme].error);
    await put(`previews/components-${theme}.png`, await sharp(Buffer.from(svg(1280, 990, controls))).png().toBuffer());
  }
  let matrix = rect(0, 0, 1440, 1080, 0, '#F4F5F7') + text(32, 44, 'xdnote · 六种强调色 × 明暗主题', 26, '#1F2937', 600) + text(32, 74, '以下样本由 tokens.json 精确绘制，几何相同。', 14, '#667085') + text(224, 116, '浅色', 16) + text(850, 116, '深色', 16);
  Object.entries(T.accents).forEach(([name, a], row) => {
    const y = 142 + row * 152;
    matrix += text(32, y + 30, a.label, 18, '#1F2937', 600) + text(32, y + 55, a.base, 14, '#667085');
    for (const [ci, theme] of ['light', 'dark'].entries()) {
      const x = 208 + ci * 610, t = T.themes[theme], p = a[theme];
      matrix += rect(x, y, 584, 132, 8, t.surface, t.border) + text(x + 18, y + 25, p.solid, 12, t.textSecondary);
      matrix += rect(x + 18, y + 38, 92, 32, 6, p.solid) + text(x + 50, y + 60, '保存', 14, p.foreground);
      matrix += rect(x + 126, y + 38, 144, 32, 6, t.sidebar, t.border) + rect(x + 126, y + 38, 72, 32, 6, p.solid) + text(x + 148, y + 60, '阅读', 14, p.foreground) + text(x + 220, y + 60, '编辑', 14, t.text);
      matrix += place(choice(theme, name, 'toggle', 'on').svg, x + 294, y + 38);
      matrix += rect(x + 18, y + 84, 546, 32, 4, p.soft) + rect(x + 18, y + 86, 3, 28, 1.5, p.solid) + glyph('document', p.softForeground, 16, x + 32, y + 92) + text(x + 58, y + 106, '写作，从这里开始.md', 14, p.softForeground);
    }
  });
  const matrixSvg = svg(1440, 1080, matrix);
  await put('previews/accent-matrix.svg', matrixSvg);
  await put('previews/accent-matrix.png', await sharp(Buffer.from(matrixSvg)).png().toBuffer());
  const cols = 4, cellW = 420, cellH = 330, rows = Math.ceil(C.boards.length / cols);
  let labels = rect(0, 0, cols * cellW + 32, 110 + rows * cellH, 0, '#F4F5F7') + text(24, 42, 'xdnote · 完整界面设计总览', 28, '#1F2937', 600) + text(24, 76, '33 张图板 / 89 个页面与状态 / 静态设计 / 六种强调色 / 浅深色', 16, '#667085');
  const images = [];
  for (let i = 0; i < C.boards.length; i++) {
    const b = C.boards[i], x = 16 + i % cols * cellW, y = 110 + Math.floor(i / cols) * cellH;
    const thumb = await sharp(path.join(DESIGN, b.path)).resize(396, 270, { fit: 'contain', background: '#FFFFFF' }).png().toBuffer();
    images.push({ input: thumb, left: x + 6, top: y + 35 });
    labels += text(x + 8, y + 23, b.id.slice(0, 2) + '  ' + b.title, 16, '#1F2937', 500);
    await fs.mkdir(path.join(DESIGN, 'hifi/v2/thumbnails'), { recursive: true });
    await fs.writeFile(path.join(DESIGN, 'hifi/v2/thumbnails/' + b.id + '.jpg'), await sharp(path.join(DESIGN, b.path)).resize(800, 540, { fit: 'inside' }).jpeg({ quality: 84 }).toBuffer());
  }
  const overview = await sharp(Buffer.from(svg(cols * cellW + 32, 110 + rows * cellH, labels))).composite(images).png().toBuffer();
  await fs.writeFile(path.join(DESIGN, 'hifi/v2/overview.png'), overview);
  const featured = [1, 2, 5, 12, 14, 25].map(n => C.boards[n - 1]);
  const heroImages = []; let heroLabels = rect(0, 0, 1600, 1470, 0, '#F4F5F7') + text(30, 45, 'xdnote · 阅读、写作与个性化', 28, '#1F2937', 600) + text(30, 76, '完整图组包含 33 张图板与 89 个页面、状态', 16, '#667085');
  for (let i = 0; i < featured.length; i++) { const x = 20 + i % 2 * 790, y = 110 + Math.floor(i / 2) * 450; heroLabels += text(x + 12, y + 20, featured[i].title, 18, '#1F2937', 500); heroImages.push({ input: await sharp(path.join(DESIGN, featured[i].path)).resize(760, 400, { fit: 'contain', background: '#FFFFFF' }).png().toBuffer(), left: x + 5, top: y + 35 }); }
  await fs.writeFile(path.join(DESIGN, 'hifi/v2/featured.png'), await sharp(Buffer.from(svg(1600, 1470, heroLabels))).composite(heroImages).png().toBuffer());
}
async function extraPreviews() {
  const cuts = records.filter(r => r.id.startsWith('component-reference/'));
  let body = rect(0, 0, 1400, 100 + Math.ceil(cuts.length / 6) * 130, 0, '#F4F5F7') + text(24, 40, 'xdnote · 从采用原稿直接裁出的组件参考', 24, '#1F2937', 600) + text(24, 70, '这些局部保留位图背景与文字；透明可复用素材见独立控件目录。', 14, '#667085');
  const overlays = [];
  for (let i = 0; i < cuts.length; i++) {
    const x = 12 + i % 6 * 232, y = 100 + Math.floor(i / 6) * 130;
    body += text(x + 6, y + 17, cuts[i].id.split('/').slice(1).join(' / '), 11, '#667085');
    overlays.push({ input: await sharp(path.join(OUT, cuts[i].files[0])).resize(216, 92, { fit: 'contain', background: '#FFFFFF' }).png().toBuffer(), left: x + 3, top: y + 24 });
  }
  await put('previews/reference-components.png', await sharp(Buffer.from(svg(1400, 100 + Math.ceil(cuts.length / 6) * 130, body))).composite(overlays).png().toBuffer());
  const art = records.filter(r => r.id.startsWith('empty-states/'));
  let artBody = rect(0, 0, 1440, 470, 0, '#F4F5F7') + text(24, 40, 'xdnote · 空态插画', 24, '#1F2937', 600);
  const artImages = [];
  for (let i = 0; i < art.length; i++) { const x = i % 8 * 180, y = 80 + Math.floor(i / 8) * 190, dark = i >= 8; artBody += rect(x + 5, y, 170, 175, 6, dark ? '#22272F' : '#FFFFFF') + text(x + 12, y + 155, art[i].id.split('/').at(-1), 10, dark ? '#AAB4C0' : '#667085'); artImages.push({ input: await fs.readFile(path.join(OUT, art[i].files.find(p => p.endsWith('@1x.png')))), left: x + 10, top: y + 8 }); }
  await put('previews/empty-states.png', await sharp(Buffer.from(svg(1440, 470, artBody))).composite(artImages).png().toBuffer());
  let brandBody = rect(0, 0, 1000, 370, 0, '#F4F5F7') + text(28, 44, 'xdnote · 折光品牌与应用图标', 24, '#1F2937', 600) + text(28, 76, '已验收的视觉方向；小尺寸图标须在各平台原生环境继续核对。', 14, '#667085');
  const brandImages = [];
  for (const [i, size] of [16, 32, 64, 128, 256].entries()) { const x = 32 + i * 145; brandBody += text(x, 322, size + ' px', 13, '#667085'); brandImages.push({ input: await fs.readFile(path.join(OUT, `brand/png/app-icon-${size}.png`)), left: x, top: 100 + (180 - Math.min(size, 180)) / 2 }); if (size === 256) brandImages.at(-1).input = await sharp(brandImages.at(-1).input).resize(180, 180).png().toBuffer(); }
  brandImages.push({ input: await fs.readFile(path.join(OUT, 'brand/png/mark-light@3x.png')), left: 830, top: 142 });
  brandBody += text(830, 322, '双色品牌标记', 13, '#667085');
  await put('previews/brand-proposals.png', await sharp(Buffer.from(svg(1000, 370, brandBody))).composite(brandImages).png().toBuffer());
}
async function gallery() {
  const cards = C.boards.map(b => `<article id="${b.id}"><a href="${b.path}"><img src="hifi/v2/thumbnails/${b.id}.jpg" alt="${esc(b.title)}" loading="lazy"></a><h2>${b.id.slice(0, 2)} · ${esc(b.title)}</h2><p>${b.tier} · ${b.frames.length} 个页面／状态</p><div>${b.frames.map(f => `<a href="assets/ui-v2/reference-slices/${f.id}.png">${esc(f.title)}</a>`).join(' · ')}</div><p><a href="${b.path}">打开原图</a></p></article>`).join('\n');
  const html = `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>xdnote · 设计图与素材</title><style>html{color-scheme:light}body{font:15px/1.65 system-ui,"Microsoft YaHei UI",sans-serif;color:#1f2937;background:#f4f5f7;margin:0}main{max-width:1480px;padding:40px 28px;margin:auto}h1{font-size:32px;margin:0 0 10px}h2{font-size:18px;margin:16px 0 4px}a{color:#374151;text-underline-offset:3px}header{margin-bottom:30px}header p{max-width:900px}nav{display:flex;gap:20px;flex-wrap:wrap}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:24px}article{background:#fff;padding:16px;border:1px solid #dde2e8;border-radius:10px}article img{width:100%;height:290px;object-fit:contain;background:#f8f9fa}article p{color:#667085;margin:5px 0}.assets{margin:40px 0;background:white;padding:24px;border:1px solid #dde2e8;border-radius:10px}.assets img{max-width:100%;height:auto}footer{margin-top:32px;color:#667085}</style></head><body><main><header><h1>xdnote · 完整界面设计</h1><p>33 张静态图板，89 个页面与状态。覆盖工作区、独立文件、设置、菜单、编辑辅助、文件异常、兼容状态与导出。点击图片查看原图，点击状态名称查看对应切片。</p><nav><a href="hifi/v2/overview.png">整套总览</a><a href="#assets">切图与素材</a><a href="../docs/visual/screen-inventory.md">覆盖清单</a><a href="sources/ui-suite-v2.json">提示词与来源</a><a href="brand-proposals/README.md">品牌方向</a></nav></header><section class="grid">${cards}</section><section class="assets" id="assets"><h2>可复用素材</h2><p>SVG、透明 PNG 1×／2×／3×、六色与明暗变体、状态标记、空态插画、应用图标，以及逐图切片。</p><nav><a href="assets/README.md">使用说明</a><a href="assets/ui-v2/manifest.json">素材清单</a></nav><h2>精确配色样本</h2><img src="assets/ui-v2/previews/accent-matrix.png" alt="六色与明暗主题的精确样本"><h2>浅色控件</h2><img src="assets/ui-v2/previews/components-light.png" alt="浅色控件素材图册"><h2>深色控件</h2><img src="assets/ui-v2/previews/components-dark.png" alt="深色控件素材图册"><h2>图标</h2><img src="assets/ui-v2/previews/icons-light.png" alt="图标素材图册"></section><footer>本页只是静态材料索引，不是应用原型。图板 27 为平台交接示意；P1 图板不自动提升首版范围。精确参数以 docs/visual 为准。未分发字体、未引入应用框架。</footer></main></body></html>`;
  await fs.writeFile(path.join(DESIGN, 'index.html'), html, 'utf8');
}
async function main() {
  T = await json('docs/visual/tokens.json'); G = await json('docs/visual/asset-geometry.json'); C = await json('design/sources/ui-suite-v2.json');
  await fs.mkdir(OUT, { recursive: true });
  for (const [name, fn] of [['icons', icons], ['components', components], ['empty-states', emptyStates], ['brand', brand], ['reference-crops', slices], ['preview-sheets', previews], ['extra-previews', extraPreviews], ['static-gallery', gallery]]) { await fn(); console.log('Exported ' + name); }
  const manifest = { schemaVersion: 2, product: 'xdnote', purpose: 'static-design-assets', pathsRelativeTo: 'design/assets/ui-v2/', sourceGenerator: 'design/sources/build-ui-assets.cjs', tokenAuthority: 'docs/visual/tokens.json', geometryAuthority: 'docs/visual/asset-geometry.json', densities: G.densityExports, exportedFontFiles: false, origins: { vectorAssets: 'original project-local geometry based on adopted UI design style', referenceCrops: 'literal pixel extraction with source SHA-256 and exact rectangle', generatedBoards: 'built-in image_gen; prompts and revisions retained in sources/v2-receipts' }, counts: { boardCount: C.boards.length, surfaceCount: C.surfaceCount, icons: Object.keys(iconPaths).length, logicalAssets: records.length, referenceSlices: records.filter(r => r.kind === 'literal-reference-crop').length, componentVariants: records.filter(r => r.id.startsWith('components/')).length, files: files.size, svg: [...files.keys()].filter(p => p.endsWith('.svg')).length, png: [...files.keys()].filter(p => p.endsWith('.png')).length }, assets: records, files: [...files.values()].sort((a, b) => a.path.localeCompare(b.path)) };
  await fs.writeFile(path.join(OUT, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n', 'utf8');
  console.log(JSON.stringify(manifest.counts));
}
main().catch(e => { console.error(e); process.exitCode = 1; });
