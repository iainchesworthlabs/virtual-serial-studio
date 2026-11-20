#!/usr/bin/env node
// Suggests a theme.css (8 roles, light and dark) from one brand colour, then checks it with check-palette.mjs.
// Usage: node derive-palette.mjs <slug> <#rrggbb> [--shell icl.css] > <slug>/theme.css
// The result passes the contrast rules, but it is a starting point: look at it, and edit the values by hand if you want.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { checkContrast, parseNeutrals, parseHex, ratio, ROLES } from './check-palette.mjs';

function hexToHsl(hex) {
  const [r, g, b] = parseHex(hex).map((v) => v / 255);
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2, d = max - min;
  let h = 0, s = 0;
  if (d) {
    s = d / (1 - Math.abs(2 * l - 1));
    h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    h = (h * 60 + 360) % 360;
  }
  return [h, s, l];
}
function hslToHex(h, s, l) {
  s = Math.min(1, Math.max(0, s)); l = Math.min(1, Math.max(0, l));
  const c = (1 - Math.abs(2 * l - 1)) * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = l - c / 2;
  const [r, g, b] = h < 60 ? [c, x, 0] : h < 120 ? [x, c, 0] : h < 180 ? [0, c, x] : h < 240 ? [0, x, c] : h < 300 ? [x, 0, c] : [c, 0, x];
  return '#' + [r, g, b].map((v) => Math.round((v + m) * 255).toString(16).padStart(2, '0')).join('');
}

const rat = (x, y) => ratio(parseHex(x), parseHex(y));
/** Move lightness from `start` in direction `dir` (+1 lighter, -1 darker) until `ok(hex)` holds. */
function fit(h, s, start, dir, ok) {
  let l = start;
  for (let i = 0; i < 100; i++) {
    const hex = hslToHex(h, s, l);
    if (ok(hex)) return hex;
    l += dir * 0.01;
    if (l < 0.02 || l > 0.98) break;
  }
  return hslToHex(h, s, Math.min(0.98, Math.max(0.02, l)));
}

/** Build the palette from a base colour. Each role is moved lighter or darker until it meets its contrast target.
 *  `nudge` adds a safety margin to every target. */
export function build(base, neutrals, nudge = 0) {
  const [h, s0] = hexToHsl(base);
  const s = s0 < 0.08 ? s0 : Math.max(0.35, Math.min(0.9, s0));
  const m = nudge * 0.15;
  const sf = { light: neutrals.surface.light, dark: neutrals.surface.dark };
  const sf2 = { light: neutrals['surface-2'].light, dark: neutrals['surface-2'].dark };
  const tint = { light: hslToHex(h, Math.min(s, 0.7), 0.96), dark: hslToHex(h, Math.min(s, 0.35), 0.1) };
  const onPrimaryDark = hslToHex(h, Math.min(s, 0.5), 0.08);
  const onGradDark = hslToHex(h, Math.min(s, 0.6), 0.96);
  const primaryLight = fit(h, s, 0.5, -1, (x) => rat('#ffffff', x) >= 5 + m && rat(x, sf2.light) >= 3.2 + m);
  const primaryDark = fit(h, s, 0.5, 1, (x) => rat(onPrimaryDark, x) >= 5 + m && rat(x, sf.dark) >= 3.2 + m && rat(x, tint.dark) >= 3.2 + m);
  const gt = (x) => rat('#ffffff', x) >= 5 + m;
  return {
    primary: { light: primaryLight, dark: primaryDark },
    'on-primary': { light: '#ffffff', dark: onPrimaryDark },
    accent: {
      light: fit(h, s, 0.55, -1, (x) => rat(x, sf.light) >= 3.2 + m && rat(x, tint.light) >= 3.2 + m),
      dark: fit(h, s, 0.55, 1, (x) => rat(x, sf.dark) >= 3.2 + m && rat(x, tint.dark) >= 3.2 + m),
    },
    link: {
      light: fit(h, s, 0.45, -1, (x) => rat(x, sf2.light) >= 4.8 + m && rat(x, tint.light) >= 4.8 + m),
      dark: fit(h, s, 0.6, 1, (x) => rat(x, sf2.dark) >= 4.8 + m && rat(x, tint.dark) >= 4.8 + m),
    },
    tint,
    'gradient-from': { light: fit(h, s, 0.3, -1, (x) => rat('#ffffff', x) >= 8 + m), dark: hslToHex(h, s, 0.14) },
    'gradient-to': { light: fit(h, s, 0.45, -1, gt), dark: fit(h, s, 0.3, -1, (x) => rat(onGradDark, x) >= 5 + m) },
    'on-gradient': { light: '#ffffff', dark: onGradDark },
  };
}

export function derive(base, neutrals) {
  for (let nudge = 0; nudge <= 8; nudge++) {
    const palette = build(base, neutrals, nudge);
    if (checkContrast(palette, neutrals).every((r) => r.ok || r.warn)) return palette;
  }
  throw new Error(`no passing palette found for ${base}; choose the colours by hand`);
}

export function toCss(slug, palette) {
  const lines = ROLES.map((r) => `  --app-${r}:${' '.repeat(Math.max(1, 18 - r.length))}light-dark(${palette[r].light}, ${palette[r].dark});`);
  return `/* Suggested from ${slug}'s brand colour by derive-palette.mjs. Edit by hand if you like; check-palette.mjs must still pass. */\n[data-app="${slug}"] {\n${lines.join('\n')}\n}\n`;
}

function main(argv) {
  let shell = fileURLToPath(new URL('../icl.css', import.meta.url));
  const rest = [];
  for (let i = 0; i < argv.length; i++) { if (argv[i] === '--shell') shell = argv[++i]; else rest.push(argv[i]); }
  const [slug, base] = rest;
  if (!slug || !/^#[0-9a-fA-F]{6}$/.test(base || '')) { console.error('usage: derive-palette.mjs <slug> <#rrggbb> [--shell icl.css]'); return 2; }
  process.stdout.write(toCss(slug, derive(base, parseNeutrals(readFileSync(shell, 'utf8')))));
  return 0;
}
if (process.argv[1] && resolve(fileURLToPath(import.meta.url)) === resolve(process.argv[1])) process.exit(main(process.argv.slice(2)));
