/** Static comparison of two proposed logical titlebar heights, not a runtime capture. */
'use strict';

const fs = require('node:fs/promises');
const path = require('node:path');
const sharp = require('sharp');
const tokens = require('../../docs/visual/tokens.json');
const brand = require('./brand-mark.cjs');

const light = tokens.themes.light;
const accent = tokens.accents.graphite.light;
const width = 1280;
const height = 1060;
const density = 2;
const out = path.resolve(__dirname, '../hifi/v2/comparisons/titlebar-height.png');
const font = 'Microsoft YaHei, Noto Sans CJK SC, sans-serif';
if (tokens.typography.ui.size !== 14 || tokens.typography.ui.lineHeight !== 1.5 ||
    tokens.typography.secondary.size !== 12 || tokens.typography.secondary.lineHeight !== 1.5) {
  throw new Error('Review the comparison when the shared UI typography changes.');
}
const text = (x, y, value, size = 14, fill = light.text, weight = 400, extra = '') =>
  `<text x="${x}" y="${y}" font-family="${font}" font-size="${size}" font-weight="${weight}" fill="${fill}" ${extra}>${value}</text>`;

function controlIcons(w, barHeight) {
  const cy = barHeight / 2;
  return `<g stroke="${light.text}" stroke-width="1.35" stroke-linecap="round" fill="none">
    <path d="M${w - 106} ${cy}h10"/>
    <rect x="${w - 76}" y="${cy - 5}" width="10" height="10" rx="0.6"/>
    <path d="M${w - 42} ${cy - 5}l10 10m0-10l-10 10"/>
  </g>`;
}

function header(w, h) {
  const linesTop = (h - 39) / 2;
  return `<rect width="${w}" height="${h}" fill="${light.sidebar}"/>
    <g transform="translate(16 ${(h - 18) / 2}) scale(${18 / 268})">${brand.mark('light')}</g>
    <g transform="translate(38 ${(h - 12) / 2}) scale(${12 / 195})">${brand.wordmark()}</g>
    <path d="M108 ${h / 2 - 11}v22" stroke="${light.border}"/>
    ${text(122, linesTop + 10.5, '写作，从这里开始.md', 14, light.text, 500, 'dominant-baseline="central"')}
    ${text(122, linesTop + 30, '文档 / 个人笔记', 12, light.textSecondary, 400, 'dominant-baseline="central"')}
    ${controlIcons(w, h)}
    <path d="M0 ${h - 0.5}H${w}" stroke="${light.border}"/>`;
}

function sample(x, y, h) {
  const w = 560;
  const wh = 286;
  const toolbarY = h;
  const bodyY = h + 48;
  return `<g transform="translate(${x} ${y})">
    <defs><clipPath id="window-${h}"><rect width="${w}" height="${wh}" rx="8"/></clipPath></defs>
    <rect x="0" y="4" width="${w}" height="${wh}" rx="8" fill="#98A2B3" opacity="0.10"/>
    <g clip-path="url(#window-${h})">
      <rect width="${w}" height="${wh}" fill="${light.background}"/>
      ${header(w, h)}
      <g transform="translate(0 ${toolbarY})">
        <rect width="${w}" height="48" fill="${light.surface}"/>
        <g transform="translate(18 16)" stroke="${light.text}" stroke-width="1.4" fill="none" stroke-linejoin="round"><path d="M1 4V2h5l2 2h7v10H1z"/></g>
        ${text(42, 24, '在工作区打开', 14, light.text, 400, 'dominant-baseline="central"')}
        <rect x="223" y="8" width="132" height="32" rx="6" fill="#F1F3F5" stroke="${light.border}"/>
        <rect x="223" y="8" width="66" height="32" rx="6" fill="${accent.solid}"/>
        ${text(256, 24, '阅读', 14, '#FFFFFF', 500, 'text-anchor="middle" dominant-baseline="central"')}
        ${text(322, 24, '编辑', 14, light.textSecondary, 400, 'text-anchor="middle" dominant-baseline="central"')}
        ${text(515, 24, '···', 18, light.text, 600, 'text-anchor="middle" dominant-baseline="central"')}
        <path d="M0 47.5H${w}" stroke="${light.border}"/>
      </g>
      <g transform="translate(48 ${bodyY})">
        ${text(0, 56, '写作，从这里开始', 26, light.text, 600)}
        ${text(0, 91, '把想法写下来，让思考慢慢清晰。', 15, light.text)}
        <rect x="0" y="116" width="3" height="27" rx="1.5" fill="${accent.solid}" opacity="0.8"/>
        ${text(16, 134, '少一点干扰，多一点专注。', 14, light.textSecondary)}
      </g>
      <g transform="translate(0 ${wh - 26})">
        <rect width="${w}" height="26" fill="${light.surface}"/>
        <path d="M0 .5H${w}" stroke="${light.border}"/>
        <circle cx="18" cy="13" r="5.5" fill="#16834A"/>
        <path d="M15.5 13l1.8 1.8 3.3-3.6" stroke="white" stroke-width="1.1" stroke-linecap="round" fill="none"/>
        ${text(31, 13, '已保存', 11, light.text, 400, 'dominant-baseline="central"')}
        ${text(w - 18, 13, 'UTF-8   ·   100%', 11, light.textSecondary, 400, 'text-anchor="end" dominant-baseline="central"')}
      </g>
    </g>
    <rect x="0.5" y="0.5" width="${w - 1}" height="${wh - 1}" rx="8" fill="none" stroke="${light.border}"/>
  </g>`;
}

function detail(x, y, h) {
  const scale = 2;
  const w = 280;
  const linesTop = (h - 39) / 2;
  const pad = (h - 39) / 2;
  return `<g transform="translate(${x} ${y}) scale(${scale})">
    <rect x="0" y="0" width="${w}" height="${h}" rx="4" fill="${light.sidebar}" stroke="${light.border}" stroke-width="0.6"/>
    <rect x="16" y="${linesTop}" width="210" height="21" fill="#E4E9F0"/>
    <rect x="16" y="${linesTop + 21}" width="210" height="18" fill="#EDF0F4"/>
    ${text(20, linesTop + 10.5, '写作，从这里开始.md', 14, light.text, 500, 'dominant-baseline="central"')}
    ${text(20, linesTop + 30, '文档 / 个人笔记', 12, light.textSecondary, 400, 'dominant-baseline="central"')}
    <path d="M240 0h8 M244 0v${h} M240 ${h}h8" fill="none" stroke="#788597" stroke-width="0.75"/>
    ${text(252, h / 2, String(h), 11, '#58677A', 600, 'dominant-baseline="central"')}
    <path d="M16 ${pad}h210 M16 ${h - pad}h210" stroke="#BCC7D4" stroke-width="0.45"/>
  </g>`;
}

const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width * density}" height="${height * density}" viewBox="0 0 ${width} ${height}">
  <rect width="${width}" height="${height}" fill="#F5F6F8"/>
  ${text(52, 66, '标题栏高度对比', 30, light.text, 600)}
  ${text(52, 101, '同一窗口内容、字号与图标，仅调整标题栏高度', 16, light.textSecondary)}
  ${text(52, 158, '40', 27, light.text, 600)}
  ${text(103, 158, '逻辑像素', 16, light.text, 500)}
  ${text(260, 158, '较紧凑的参考方案', 14, light.textSecondary)}
  ${text(668, 158, '48', 27, light.text, 600)}
  ${text(719, 158, '逻辑像素', 16, light.text, 500)}
  ${text(876, 158, '当前统一建议值', 14, light.textSecondary)}
  ${sample(52, 185, 40)}
  ${sample(668, 185, 48)}
  ${text(52, 510, '两侧从同一高度开始；48 的标题栏底边向下增加 8 px。', 15, light.textSecondary)}
  <path d="M52 548H1228" stroke="${light.border}"/>
  ${text(52, 596, '文件名和路径的空间', 22, light.text, 600)}
  ${text(52, 628, '双行排版的 2 倍局部示意；当前设计已将路径移至下方工具栏。', 15, light.textSecondary)}
  ${detail(52, 655, 40)}
  ${detail(668, 655, 48)}
  ${text(52, 779, '40 px：两行之外共剩 1 px', 16, light.text, 600)}
  ${text(668, 779, '48 px：两行之外共剩 9 px', 16, light.text, 600)}
  <rect x="52" y="824" width="1176" height="138" rx="10" fill="#FFFFFF" stroke="${light.border}"/>
  ${text(76, 862, '计算依据', 16, light.text, 600)}
  ${text(76, 894, '文件名 14 × 1.5 + 路径 12 × 1.5 = 39 逻辑像素', 16, light.text)}
  ${text(76, 926, '假设两行紧贴排列；剩余空间用于上下留白，不计边线与额外行间距。', 14, light.textSecondary)}
  ${text(52, 1010, '示意图 · 同比例 · 应用绘制窗口示例；系统原生标题栏及最终字体效果仍需实际验证', 13, light.textSecondary)}
</svg>`;

async function main() {
  await fs.mkdir(path.dirname(out), { recursive: true });
  await sharp(Buffer.from(svg)).png().toFile(out);
  console.log(JSON.stringify({ output: out, width: width * density, height: height * density,
    density, comparedLogicalHeights: [40, 48], lineHeights: [21, 18],
    scope: 'static-comparison-not-native-validation' }));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
