/** Editable xdnote folded-page identity, reconstructed from the approved review board. */
'use strict';

const brandTokens = require('../../docs/visual/tokens.json').brand;
const palette = { light: brandTokens.light, dark: brandTokens.dark };

// Two independent filled shapes preserve the narrow, curved negative-space fold.
const mark = (theme = 'light') => {
  const p = palette[theme];
  if (!p) throw new Error('Unknown brand theme: ' + theme);
  return `<path fill="${p.primary}" d="M0 264V52C0 27 15 8 34 1c3-1 7-1 10 1l107 60c5 3 7 6 7 11v42c0 6-2 10-7 14l-62 52C47 207 20 233 0 264Z"/>` +
    `<path fill="${p.secondary}" d="M8 268c26-31 54-56 86-74l122-74v91c0 32-24 57-57 57H8Z"/>`;
};

// The wordmark is vector outlines rather than a dependency on an installed font.
const wordmark = (color = palette.light.word) => `<g fill="${color}" fill-rule="evenodd">` +
  '<path d="M0 49h44l36 46 36-46h43l-58 71 59 72h-44l-36-47-36 47H0l59-72Z"/>' +
  '<path d="M279 0h33v192h-33v-15c-11 12-27 18-45 18-45 0-75-31-75-73s30-73 75-73c18 0 34 6 45 17Zm-44 78c-25 0-42 18-42 44s17 44 42 44 43-18 43-44-18-44-43-44Z"/>' +
  '<path d="M331 192V50h32v15c10-11 25-16 41-16 42 0 65 26 65 69v74h-33v-73c0-26-12-40-34-40-23 0-38 16-38 41v72Z"/>' +
  '<path d="M561 49c46 0 76 31 76 73s-30 73-76 73-76-31-76-73 30-73 76-73Zm0 29c-25 0-43 18-43 44s18 44 43 44 43-18 43-44-18-44-43-44Z"/>' +
  '<path d="M657 22h33v28h38v29h-38v70c0 12 5 16 16 16 7 0 15-1 22-3v28c-9 3-19 5-29 5-29 0-42-14-42-42V79h-21V50h21Z"/>' +
  '<path d="M811 49c46 0 74 31 74 73 0 4 0 8-1 12H769c4 21 20 33 44 33 17 0 31-6 41-18l26 18c-16 19-39 28-68 28-46 0-77-29-77-73 0-42 30-73 76-73Zm0 27c-22 0-37 13-41 33h82c-3-20-18-33-41-33Z"/>' +
  '</g>';

const svg = (w, h, body, title) => `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}"><title>${title}</title>${body}</svg>`;
const markSvg = (theme = 'light') => svg(32, 32, `<g transform="translate(3.105 0) scale(0.119403)">${mark(theme)}</g>`, 'xdnote folded-page mark');
const logoSvg = (theme = 'light') => svg(256, 64, `<g transform="translate(2 4) scale(0.208956)">${mark(theme)}</g><g transform="translate(62 12) scale(0.208333)">${wordmark(palette[theme].word)}</g>`, 'xdnote logo');
const appSvg = () => svg(128, 128, `<rect x="3" y="3" width="122" height="122" rx="27" fill="${brandTokens.appIconSurface}"/><g transform="translate(29.44 21.12) scale(0.32)">${mark('light')}</g>`, 'xdnote application icon');

module.exports = { palette, mark, wordmark, markSvg, logoSvg, appSvg, name: 'folded-light' };
