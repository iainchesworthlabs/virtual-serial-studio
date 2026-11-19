# icl-site kit v1.1.2

For app repos that publish a page under `https://iainchesworthlabs.com/<slug>/`. Design: `docs/design/` in the icl-site repo.

## Files

All files are public, plain files under `https://iainchesworthlabs.com/assets/v1/`:

| File | Use |
|---|---|
| `icl.css` | The shared stylesheet. Link it, do not copy it. |
| `kit/app-page.html` | Page skeleton. Replace each `{{PLACEHOLDER}}`. |
| `kit/partials/header.html`, `kit/partials/footer.html` | Header and footer, already in the skeleton. For MkDocs or other templates. |
| `kit/theme.css` | Template for `<slug>/theme.css` (8 colours). |
| `kit/icl-app.json` | Template for `<slug>/icl-app.json`, the tile manifest (below). |
| `kit/check-palette.mjs` | Contrast and shape check for `theme.css`. Node 20 or later, no dependencies. |
| `kit/derive-palette.mjs` | Suggests `theme.css` from one brand colour: `node derive-palette.mjs <slug> "#rrggbb" > <slug>/theme.css`. Check the result with `check-palette.mjs`. |
| `kit/VERSION.txt` | The kit version. |
| `kit/SHA256SUMS.txt` | SHA-256 of every kit file and `icl.css`. |

## Use in an app repo

Download at build time, pinned to the SHA-256 recorded in your repo. Do not commit the downloaded files. Example:

```bash
base=https://iainchesworthlabs.com/assets/v1
mkdir -p kit && cd kit
for f in kit/SHA256SUMS.txt icl.css kit/check-palette.mjs kit/app-page.html; do curl -fsSLO "$base/$f"; done
```

Keep the expected hash of `SHA256SUMS.txt` in your repo and compare it before you trust the rest, then run `sha256sum -c SHA256SUMS.txt`
(the sums use paths relative to `/assets/v1/`, so download with the same folder layout if you want the check to find every file).
Then, in CI:

```bash
node kit/check-palette.mjs --shell icl.css <slug>/theme.css
```

## Show up in the product lists

The company site lists every app in the Products menu, on its family page, in the footer and on the home page. It builds those lists
from what the app itself publishes, so a new app appears without anyone editing the company site's pages, and an app's words, icon and
colours change when the app changes.

Publish `/<slug>/icl-app.json` (template: `kit/icl-app.json`) and `/<slug>/theme.css`:

| Field | Meaning |
|---|---|
| `version` | `1`. |
| `name` | The product name, up to 40 characters. |
| `family` | `audio`, `games`, `test-equipment` or `pool`. |
| `summary` | One plain sentence, up to 120 characters. Shown on the card and in the menu. |
| `tags` | Optional, up to 4 short labels, for example the platforms. |
| `status` | `available` (default) or `coming-soon` (the page exists, the product is not out yet). |
| `icon` | Optional. A file you publish: `.svg`, `.png` or `.webp`, a path relative to `/<slug>/` such as `icon.svg`. Square. Without it the card shows the first letter of the name in your `--app-primary` colour. |
| `glyph` | Optional. SVG path data in a 64 by 64 box, drawn in your `--app-on-primary` colour on `--app-primary` (used when there is no `icon`). |
| `order` | Optional, 0 to 999, default 100. Lower comes first within the family. |

Unknown fields are an error, so a typo does not go unnoticed. The card is coloured by your own `theme.css`: the company site links it and
sets `data-app="<slug>"` on your card, so you change a colour in one place. The company site reads the manifest when it is deployed;
a change to your manifest shows up at its next deploy. An app with no manifest keeps the entry the company site holds for it in `data/apps/<slug>.json`.

## Rules for app pages

- Set `data-app="<slug>"` on `<html>`. Link `/assets/v1/icl.css`, the four family files (`/audio/theme.css`, `/games/theme.css`, `/test-equipment/theme.css`, `/pool/theme.css`, used by the header menu), then your own `theme.css` (relative).
- App links and files are relative. Shell links (`/`, `/support/`, `/assets/v1/...`) are root-absolute.
- `/<slug>/privacy/` and `/<slug>/support/` are required, and never move once a store listing prints them.
- No scripts, no inline `style=`, no web fonts, no cookies, no analytics, no requests to other sites. Previews send a strict CSP.
- Each page has a unique title and description, one `h1`, an absolute canonical URL and the OpenGraph tags (the skeleton has them).
- Do not add `--app-*` properties other than the 8 roles. The shell derives the rest.
- Do not use the `/assets/v1/` files for anything but linking. They change only in backwards-compatible ways within v1.

## Changes

- 1.1.2: ARINC Data Loader is linked to `/adl/` in the header's Products menu and in the footer's Test equipment group, where it said "Coming soon". Text only; pages built on 1.1.1 still work.
- 1.1.1: the Privacy entry of the header menu says "What this site collects" instead of "This site collects no data", because a contact form collects what a visitor types. Text only; pages built on 1.1.0 still work.
- 1.1.0: header with a Products menu, footer by family, family theme links in the skeleton. New classes only in `icl.css`; pages built on 1.0.0 still work.
