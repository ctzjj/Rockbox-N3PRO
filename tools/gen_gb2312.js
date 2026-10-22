// Build GB2312 charset (level 1+2, 6763 hanzi) + pass to lv_font_conv.
const { spawnSync } = require('child_process');
let dec;
try { dec = new TextDecoder('gbk'); }
catch (e) { console.error('no gbk decoder (ICU missing):', e.message); process.exit(1); }

let chars = [];
for (let hi = 0xA1; hi <= 0xF7; hi++) {
  for (let lo = 0xA1; lo <= 0xFE; lo++) {
    const buf = Buffer.from([hi, lo]);
    const s = dec.decode(buf);
    if (s && s.length === 1 && s.codePointAt(0) !== 0xFFFD) chars.push(s);
  }
}
const set = new Set(chars);
const symbols = [...set].join('');
console.error('glyphs (GB2312 blocks):', symbols.length);

const args = [
  '--font', 'NotoSansSC-Regular.otf',
  '-r', '0x20-0x7F,0x3000-0x301F,0xFF00-0xFF0F,0xFF1A-0xFF20,0xFF3B-0xFF40,0xFF5B-0xFF65,0x2013-0x2026,0x00B0,0x2190-0x2193,0x25B2-0x25BC,0x25CF-0x25D0',
  '--symbols', symbols,
  '--size', '16',
  '--bpp', '4',
  '--format', 'lvgl',
  '--lv-font-name', 'notosans_sc_16',
  '--no-compress',
  '-o', 'NotoSansSC_16.c',
];
const r = spawnSync('./node_modules/.bin/lv_font_conv', args, { stdio: ['ignore', 'inherit', 'inherit'] });
if (r.status !== 0) process.exit(r.status || 1);
// Rockbox builds this file with the app include chain: force the simple include.
const fs = require('fs');
let s = fs.readFileSync('NotoSansSC_16.c', 'utf8');
s = s.replace(/#ifdef LV_LVGL_H_INCLUDE_SIMPLE[\s\S]*?#endif/, '#include "lvgl.h"');
fs.writeFileSync('NotoSansSC_16.c', s);
console.error('include patched');

