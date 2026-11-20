#!/usr/bin/env node
// Checks an app palette (theme.css) against the icl.css contract: shape + WCAG 2.2 AA contrast, light and dark.
// Usage: node check-palette.mjs [--shell path/to/icl.css] <theme.css>...   (no dependencies, Node 20+)
import { readFileSync } from 'node:fs';
import { basename, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const ROLES = ['primary', 'on-primary', 'accent', 'link', 'tint', 'gradient-from', 'gradient-to', 'on-gradient'];
export const NEUTRALS = ['surface', 'surface-2', 'fg', 'fg-muted'];
const HEX = /^#[0-9a-fA-F]{6}$/;

export function parseHex(h) { return [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16)); }
export function luminance([r, g, b]) {
  const f = (c) => { const s = c / 255; return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4; };
  return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b);
}
export function ratio(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}
// sRGB mix, like color-mix(in srgb, a (1-pct), b pct)
export const mix = (a, b, pct) => a.map((c, i) => Math.round(c * (1 - pct) + b[i] * pct));

/** Parse every flat `selector { ... }` rule: [{selector, decls: {name: {light, dark} | {raw}}}] */
export function parseRules(css) {
  css = css.replace(/\/\*[\s\S]*?\*\//g, '');
  const rules = [];
  for (const m of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    const decls = {};
    for (const d of m[2].split(';')) {
      const i = d.indexOf(':');
      if (i < 0) continue;
      const name = d.slice(0, i).trim();
      const value = d.slice(i + 1).trim();
      const ld = value.match(/^light-dark\(\s*(#[0-9a-fA-F]{3,8})\s*,\s*(#[0-9a-fA-F]{3,8})\s*\)$/);
      decls[name] = ld ? { light: ld[1], dark: ld[2] } : { raw: value };
    }
    rules.push({ selector: m[1].trim(), decls });
  }
  return rules;
}

/** Shape check of an app theme file. Returns {slug, palette, errors}. */
export function parseTheme(css, folderSlug) {
  const errors = [];
  const rules = parseRules(css);
  if (rules.length !== 1) errors.push(`theme.css must hold exactly one rule, found ${rules.length}`);
  const rule = rules[0] || { selector: '', decls: {} };
  const sm = rule.selector.match(/^\[data-app="([a-z0-9]+(?:-[a-z0-9]+)*)"\]$/);
  if (!sm) errors.push(`selector must be exactly [data-app="<slug>"], got "${rule.selector}"`);
  const slug = sm && sm[1];
  if (slug && folderSlug && slug !== folderSlug) errors.push(`slug "${slug}" does not match folder "${folderSlug}"`);
  const want = ROLES.map((r) => `--app-${r}`);
  for (const n of Object.keys(rule.decls)) if (!want.includes(n)) errors.push(`property ${n} is not allowed (only the 8 --app-* roles)`);
  const palette = {};
  for (const r of ROLES) {
    const v = rule.decls[`--app-${r}`];
    if (!v) { errors.push(`missing --app-${r}`); continue; }
    if (!v.light) { errors.push(`--app-${r} must be light-dark(#rrggbb, #rrggbb)`); continue; }
    for (const side of ['light', 'dark']) if (!HEX.test(v[side])) errors.push(`--app-${r} ${side} value ${v[side]} must be #rrggbb`);
    palette[r] = { light: v.light, dark: v.dark };
  }
  return { slug, palette, errors };
}

/** Read the shell neutrals from icl.css (first rule that declares each). */
export function parseNeutrals(shellCss) {
  const n = {};
  for (const r of parseRules(shellCss)) {
    for (const name of NEUTRALS) {
      const v = r.decls[`--icl-${name}`];
      if (v && v.light && !n[name]) n[name] = v;
    }
  }
  const missing = NEUTRALS.filter((k) => !n[k]);
  if (missing.length) throw new Error(`shell neutrals missing: ${missing.join(', ')}`);
  return n;
}

/** Run the contrast rules R1..R8. Returns [{rule, scheme, pair, value, min, ok, warn}] */
export function checkContrast(palette, neutrals) {
  const out = [];
  for (const scheme of ['light', 'dark']) {
    const p = (k) => parseHex(palette[k][scheme]);
    const nu = (k) => parseHex(neutrals[k][scheme]);
    const add = (rule, pair, a, b, min, warn = false) => {
      const value = ratio(a, b);
      out.push({ rule, scheme, pair, value, min, ok: value >= min, warn });
    };
    add('R1', 'on-primary on primary', p('on-primary'), p('primary'), 4.5);
    for (const bg of ['surface', 'surface-2']) add('R2', `primary vs ${bg}`, p('primary'), nu(bg), 3);
    add('R2', 'primary vs tint', p('primary'), p('tint'), 3);
    for (const bg of ['surface', 'surface-2']) add('R3', `link on ${bg}`, p('link'), nu(bg), 4.5);
    add('R3', 'link on tint', p('link'), p('tint'), 4.5);
    add('R4', 'accent vs surface', p('accent'), nu('surface'), 3);
    add('R4', 'accent vs tint', p('accent'), p('tint'), 3);
    const mid = mix(p('gradient-from'), p('gradient-to'), 0.5);
    add('R5', 'on-gradient on gradient-from', p('on-gradient'), p('gradient-from'), 4.5);
    add('R5', 'on-gradient on gradient-to', p('on-gradient'), p('gradient-to'), 4.5);
    add('R5', 'on-gradient on gradient midpoint', p('on-gradient'), mid, 4.5);
    add('R6', 'fg on tint', nu('fg'), p('tint'), 4.5);
    add('R6', 'fg-muted on tint', nu('fg-muted'), p('tint'), 4.5);
    add('R7', 'on-primary on primary hover', p('on-primary'), mix(p('primary'), nu('fg'), 0.12), 4.5);
    add('R8', 'tint distinct from surface', p('tint'), nu('surface'), 1.03, true);
  }
  return out;
}

/** The shell's own text/background pairs. */
export function checkNeutrals(neutrals) {
  const out = [];
  for (const scheme of ['light', 'dark']) {
    const v = (k) => parseHex(neutrals[k][scheme]);
    for (const bg of ['surface', 'surface-2']) {
      for (const fg of ['fg', 'fg-muted']) {
        const value = ratio(v(fg), v(bg));
        out.push({ rule: 'N', scheme, pair: `${fg} on ${bg}`, value, min: 4.5, ok: value >= 4.5, warn: false });
      }
    }
  }
  return out;
}

export function report(name, errors, results) {
  const lines = [];
  const fails = results.filter((r) => !r.ok && !r.warn);
  const warns = results.filter((r) => !r.ok && r.warn);
  for (const e of errors) lines.push(`  FAIL shape: ${e}`);
  for (const r of results) {
    const tag = r.ok ? 'ok  ' : r.warn ? 'WARN' : 'FAIL';
    if (!r.ok || process.env.VERBOSE) lines.push(`  ${tag} ${r.rule} ${r.scheme.padEnd(5)} ${r.pair}: ${r.value.toFixed(2)} (min ${r.min})`);
  }
  const pass = errors.length === 0 && fails.length === 0;
  lines.unshift(`${pass ? 'PASS' : 'FAIL'} ${name}${warns.length ? ` (${warns.length} warning)` : ''}`);
  return { pass, text: lines.join('\n') };
}

function main(argv) {
  let shell = fileURLToPath(new URL('../icl.css', import.meta.url));
  const files = [];
  for (let i = 0; i < argv.length; i++) { if (argv[i] === '--shell') shell = argv[++i]; else files.push(argv[i]); }
  if (!files.length) { console.error('usage: check-palette.mjs [--shell icl.css] <theme.css>...'); return 2; }
  const neutrals = parseNeutrals(readFileSync(shell, 'utf8'));
  let ok = true;
  const sh = report(`shell neutrals (${basename(shell)})`, [], checkNeutrals(neutrals));
  console.log(sh.text); ok = ok && sh.pass;
  for (const f of files) {
    const { palette, errors } = parseTheme(readFileSync(f, 'utf8'), basename(dirname(resolve(f))));
    const results = errors.length ? [] : checkContrast(palette, neutrals);
    const r = report(f, errors, results);
    console.log(r.text); ok = ok && r.pass;
  }
  return ok ? 0 : 1;
}
if (process.argv[1] && resolve(fileURLToPath(import.meta.url)) === resolve(process.argv[1])) process.exit(main(process.argv.slice(2)));
