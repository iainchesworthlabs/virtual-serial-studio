# The website

This folder is the source of <https://virtual-serial-studio.com>. It is a static site: plain HTML and CSS, no
JavaScript, no cookies, no fonts or files from any other host. The folder is `website/` and not `site/` because
`site` is MkDocs' default output folder, and `mkdocs build` empties that folder before it writes.

## Why there is no site generator

`tools/site_build.py` is a few hundred lines of Python with the standard library only. It was chosen over a
generator such as Hugo, Jekyll or Eleventy because:

- the build needs nothing installed and nothing from the network, so it runs the same on a laptop and in CI;
- the site is ten pages that share one layout, which a generator's themes, plugins and upgrade path would outweigh;
- the build can read the application's own files: the colours come from
  `apps/vss_gui/src/theme/resources/tokens.json` and the icon from `apps/vss_gui/assets/icon/`, so the site and the
  application cannot drift apart.

The documentation is still built by MkDocs from `docs/`, and is published under `docs/` on the same host.

## Layout

| Path | Holds |
|---|---|
| `site.json` | The name, address, navigation, files to copy from the repository, the rules that label release files, and the settings of the checker |
| `templates/base.html` | The one layout: header, navigation, footer |
| `pages/*.html` | One file for each page: a comment with `title` and `description`, then the body |
| `static/css/site.css` | The styles. The build writes the colour tokens above it. It names no colour itself |
| `static/` | Files copied as they are (`robots.txt`) |
| `data/release.json` | The newest GitHub release, as last recorded. The downloads page is made from it |

## Build, check and look

```
python3 tools/site_build.py                    # writes build/website
python3 tools/site_check.py                    # links, outside resources, accessibility, contrast
python3 -m unittest discover -s tools -p "test_site_*.py"
```

Open `build/website/index.html` in a browser. Every link is relative, so the pages work from the folder. The links
into `docs/` work only on the published site, or after `mkdocs build --site-dir build/website/docs`.

`python3 tools/site_build.py --refresh-release` asks the GitHub API for the newest release (pre-releases included),
rewrites `data/release.json` and then builds. The Docs workflow does this when it publishes. A visitor's browser never
asks GitHub for anything: the downloads page holds plain links to the release files.

`python3 tools/site_check.py --external` also requests each outside link. CI does not, so that a pull request does not
fail because another site is down.

## What the checker enforces

- Every link and fragment inside the site resolves. Links to other hosts go only to the hosts in `allowed_link_hosts`.
- Nothing is loaded from another host, there is no script, frame or event handler, and no cookie can be set.
- A static subset of WCAG 2.2 AA: language, one `h1`, heading order, landmarks, a skip link, names for links and
  images, table headers, unique ids, a visible focus style, no text size in pixels.
- The contrast of every token pair in `contrast_pairs`, in the light and the dark set.
- `css/site.css` names no colour outside the generated token block.
- Each page in `draft_pages` still says DRAFT.

It does not replace a person with a keyboard and a screen reader. Check a new page by hand at 200 percent text, in
both themes and at a phone's width.

## Writing for the site

- British English, plain words, short sentences. Say what the program does today. Anything that is planned says so,
  with an "In development" chip.
- No marketing language. A heading names the thing ("Features", "Sharing a device"). A sentence states a fact. No
  slogans, no "not this, but that", no invented quotations, no exaggeration and no closing flourish. The readers are
  engineers. Prefer the wording of `docs/index.md` and the user guide.
- Do not state a price, an email address or a date that the owner has not decided.
- The licence and privacy pages are drafts for a solicitor. Keep the DRAFT mark until the owner says it can go.
- Pictures of the application are not committed. Each `figure.shot` on the features page is a placeholder with a
  `shot:` name, to be replaced by an offscreen grab of the application (`vss_gui --screenshot`) in the light and dark
  themes when the owner asks for them.

## Publishing: steps for the owner

The repository already publishes to GitHub Pages from the Docs workflow on a push to `main`. With this change the
artifact holds the website at the root and the documentation under `docs/`. Nothing below is done by the workflow or
by an agent.

1. **Custom domain.** In the repository, *Settings > Pages > Custom domain*, enter `virtual-serial-studio.com` and
   save. With the "GitHub Actions" build type a `CNAME` file in the repository is not used; this setting is what
   counts.
2. **DNS at the registrar.** For the apex `virtual-serial-studio.com`, four `A` records: `185.199.108.153`,
   `185.199.109.153`, `185.199.110.153`, `185.199.111.153`, and optionally four `AAAA` records: `2606:50c0:8000::153`,
   `2606:50c0:8001::153`, `2606:50c0:8002::153`, `2606:50c0:8003::153`. For `www`, a `CNAME` record to
   `iainchesworthlabs.github.io`. Check these addresses against GitHub's current Pages documentation before you
   enter them.
3. **Verify the domain** for the organisation (*Organisation settings > Pages > Add a domain*), which adds a `TXT`
   record and stops anyone else claiming the domain on GitHub.
4. **HTTPS.** When the DNS check on the Pages settings page passes, tick *Enforce HTTPS*. The certificate can take
   up to a day to be issued.
5. **Publish.** The site is published on the next push to `main` (a release), or run the Docs workflow by hand on
   `main`. Run it by hand after a release has been published as well, so the downloads page lists the new files.
6. **Before launch,** decide: the Pro price, an address for sales, trials and support (the support page says one
   will be published), and have a solicitor read the licence and privacy pages. Links elsewhere that point at the old
   documentation address need `docs/` added.
