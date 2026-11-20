# Product site and documentation plan

Status: draft for the owner, 19 November 2026. Nothing here is built yet. This page stays in the repository and is
excluded from the published site.

The goal is what ARINC Data Loader (ADL) got on 19 November 2026: a commercial product site, a rewritten guide and
developer pages, the site checks in CI, a deploy to the company site with a preview for each pull request, a private
"Talk to us" form, links from the company site, and the site live. Virtual Serial Studio (VSS) is not part of any
certification or safety case, and the pages must not read as if it were.

Everything below was checked against the repositories on 19 November 2026. Where the brief and the repositories
disagree, the repositories are followed and the difference is listed in "Findings".

## 1. Decisions

| | Decision | Source |
|---|---|---|
| A | Canonical site `https://iainchesworthlabs.com/virtual-serial-studio/`. `virtual-serial-studio.com` is served from the same distribution through the company stack's second-domain block. | Owner, 19 Nov |
| B | A new branch, `site`, deploys the live site. A job in `release.yml`, after the `feed` job, moves it on every release and release candidate. | Owner, 19 Nov |
| C | `website/` is ported into MkDocs on the company kit. `tools/site_build.py` and `tools/site_check.py` are retired once their release data, feed publishing and checks have moved. | Owner, 19 Nov |
| D | Not decided, so the default applies: Pro and Enterprise say "Price on request"; trials, response times and sales terms are `OWNER DECISION` comments; the DRAFT marks on the licence, privacy and editions pages stay visible. | Default |
| E | Sales route is the company contact form, and `support@virtual-serial-studio.com` is shown on the support page as plain text (not a `mailto:` link). | Owner, 19 Nov |
| F | Palette B, graphite and blue (section 8). Whether it also replaces the placeholder Test equipment family palette in icl-site is still to ask. | Owner, 19 Nov |
| G | Pictures are desktop only: 1280 px, light and dark. There are no phone-width pictures and the 375/390 px checks in the brief are dropped. | Owner, 19 Nov |

Still to ask when reached: which developer pages are published, the menu labels, whether Boost.VirtualSerial is a
product on the site, whether the chosen palette replaces the placeholder Test equipment palette in icl-site, and
whether to build the messages and exit-codes check.

### What decision A means

- **Compiled-in addresses stay as they are** (`https://virtual-serial-studio.com/updates/v1/feed.json`,
  `/downloads.html`, `/docs/`, `support@virtual-serial-studio.com`). Changing them would change the program, which
  this work does not do.
- **The update feed must answer with a direct 200.** Studio sets a manual redirect policy and refuses any redirect
  for the feed (`apps/vss_gui/src/updates/network_transport.cpp`, `docs/updates.md`). The signature is over the
  payload, not the URL, so a byte-identical copy at the company address also verifies. No feed key is built in yet
  (`builtin_feed_keys()` returns none), so update checks are inert until the owner adds one.
- **`/docs/` and `/downloads.html` are opened in a browser**, so a redirect is fine for them.
- **Edge function rule for the second domain** (an icl-site change; today the function has no host-aware rule):

  | Request on `virtual-serial-studio.com` | Answer |
  |---|---|
  | `/updates/v1/feed.json` (any `/updates/...`) | Served directly from the bucket key `virtual-serial-studio/updates/...`, status 200, no redirect |
  | `/` | 301 to `https://iainchesworthlabs.com/virtual-serial-studio/` |
  | `/docs/...` | 301 to `https://iainchesworthlabs.com/virtual-serial-studio/guide/` |
  | `/downloads.html` | 301 to `https://iainchesworthlabs.com/virtual-serial-studio/downloads/` |
  | `www.virtual-serial-studio.com` anything | 301 to the same path on the apex |
  | anything else | 404 |

  Serving only these paths avoids the second problem in the company kit: its pages link `/assets/v1/icl.css` and
  other company paths with root-absolute links, which a blanket prefix rewrite would send to the wrong key.
- **Mail:** `virtual-serial-studio.com` had no MX records on 19 Nov; on 20 Nov its zone holds MX records to iCloud Mail
  and the matching SPF and DKIM records, so mail to `support@` can arrive. I change no DNS and the second-domain change
  leaves those records alone. The owner confirms that `support@` is an address or alias of that mailbox before the
  address is published (owner step 5).
- **GitHub Pages:** turn it off for the repository once the second domain answers. Today it answers "Site not found"
  for every address and `iainchesworthlabs.github.io/virtual-serial-studio/` forwards to the domain, so nothing is
  lost. The Pages custom domain and the A, AAAA and `www` CNAME records in the `virtual-serial-studio.com` Route 53
  zone must be removed before CloudFront aliases can be added (owner step 4).
- **Duplicate content:** every page carries the canonical address on `iainchesworthlabs.com`, and the second domain
  forwards, so there is one canonical host.

### What decision B means

- Release candidates never move `main`: the shared `promote` job (icl-common-infra v0.1.30) only runs for final
  releases and has no input to change that. So this repository adds its own job instead of changing the shared one.
- New job `site` in `.github/workflows/release.yml`: `needs: feed`, environment `release`, uses the existing
  `RELEASE_BOT_*` App token and `gh api PATCH git/refs/heads/site` with `force=false`, to the tagged commit. The tag
  commit is already required to be on `develop`, so `site` only ever fast-forwards along `develop`.
- Order matters: the `feed` job uploads `update-feed.json` as a release asset. Moving `site` afterwards starts the
  docs deploy, which then sees the feed. This also removes today's manual "run the Docs workflow" step in
  `docs/releasing.md`.
- `docs.yml` production runs on a push to `site` and passes `production-ref: refs/heads/site`.
- icl-site `apps.ts` entry gets `publishRef: 'site'`.
- **First go-live does not need a release:** once the PRs are merged the owner creates `site` at a `develop` commit
  (owner step 6), and that push deploys the site. After that every release moves it.
- Owner-only: creating `site`, any ruleset that lets the App push to it, and the OIDC claim.

## 2. Information architecture

Menus follow ADL's `links.html` shape (a native `<details>` dropdown each, no script), with VSS content.

| Menu | Entries |
|---|---|
| Products | All products; Studio; Command line (`vss_cli`); Agent and gateway (`vss_agent`, `vss_gate`, Python client `vss_control`); Plugins and the plugin SDK; Windows drivers |
| Platforms | Windows; macOS; Linux (one page each) |
| Solutions | Develop and test serial software without hardware; Device emulation and scripted responses; Test automation and CI; Capture and analysis; Sharing and bridging ports; Test benches; Fault injection and response timing |
| Developers | Control API and Python client; Plugin SDK; Command line reference; Topology JSON format; Building from source; Architecture (curated, see section 3) |
| Quality and security | Quality; Security policy and releases |
| Pricing | Community, Pro, Enterprise, comparison table |
| Resources | Guide; Release notes ("What's new", generated from the changelog); FAQ; Support; Licensing; Privacy; Accessibility; Press |
| Bar buttons | Contact sales (to Support, "Talk to us"); Download |

Plan cards use ADL's `adl-plans` pattern, renamed `vss-plans`. Not built features carry a "Planned" chip
(`vss-chip--plan`): official plugins, the Microsoft-signed driver, remote use from another computer (no TLS) and
language bindings other than Python. Limits (unsigned macOS builds, the test-signed Windows driver, x86_64 only on
Windows, glibc 2.42 on Linux) go in the guide's install, troubleshooting and known-limits pages, linked from the front
pages but never leading them.

### Not on the site

A qualification kit, assurance data packs, any named standard as something VSS supports, standard-by-standard
coverage pages and their traceability check, a "Certification projects" solution, an Enterprise edition defined by
evidence, and any suggestion of fitness for safety-critical, avionics, medical or automotive use. Enterprise has no
feature of its own: it is negotiated terms, seats and machines.

### Quality and Trust

Presented as a strength, naming only what the repository shows: the test suites and sanitizers (ASan, UBSan, TSan at
-O1), fuzzing, coverage, static analysis, CodeQL and Scorecard; signed releases (a GPG `.asc` for every file,
`SHA512SUMS`, an SPDX SBOM, provenance attestations); the security policy; and the no-telemetry rule. Each sentence
is checked against a workflow or file before it is written.

### Changed on 20 November 2026

The owner reviewed the built site and asked for it to be simpler and aimed at customers. Removed: the Platforms section (its pages
and menu), the Products overview, and the Command line and Drivers product pages. The command line stays in the guide
(`user-guide/command-line.md`), and what each system needs is on Downloads and in the guide's Windows and "When the system asks"
pages. Quality and trust and Security are now entries of the Resources menu, and "User guide" is a link in the bar. The Pro edition card no
longer has a stronger border or button than the other two.

## 3. Page list

Published at `/virtual-serial-studio/`:

| Group | Pages |
|---|---|
| Front | `index`, `products/` (+ studio, command-line, agent-gateway, plugins, drivers), `platforms/` (+ windows, macos, linux), `solutions/` (seven pages), `quality/`, `security/`, `pricing/`, `downloads/`, `whats-new/`, `faq/`, `support/`, `licensing/`, `privacy/`, `accessibility/`, `press/` |
| Guide | `guide/` split by task from `docs/user-guide.md` (about 2,100 lines): getting started, ports, the command line, automation, plugins, licences, updates, troubleshooting, limits |
| Developers | `developers/` with a curated set (proposed: control API, plugin SDK, CLI reference, topology format, building, architecture). The rest stays repository-only. |
| Generated | `updates/v1/feed.json` (byte copy of the signed feed), `theme.css` (the palette), `icon.svg`, `icl-app.json` (the company tile manifest: name, summary, tags, status, icon, order), `404.html`, `sitemap.xml` |

Repository-only (in `exclude_docs`): `gui-design/`, `plan/`, `ci.md`, `releasing.md`, `migration.md`,
`workstreams.md`, `vm-testing.md`, `fuzzing.md`, `profiling.md`, `app-icon.md`, `i18n.md`, `contributing.md`,
`testing.md`.

## 4. URL map, old to new

Nothing was ever public at the old addresses (every one answers 404 or "Site not found"), so no forwarding pages
are needed, except for what the program has compiled in.

| Old address | New |
|---|---|
| `virtual-serial-studio.com/` | forwards to `iainchesworthlabs.com/virtual-serial-studio/` |
| `virtual-serial-studio.com/docs/` | forwards to `.../virtual-serial-studio/guide/` |
| `virtual-serial-studio.com/downloads.html` | forwards to `.../virtual-serial-studio/downloads/` |
| `virtual-serial-studio.com/updates/v1/feed.json` | answers 200 itself (no redirect), same bytes as `.../virtual-serial-studio/updates/v1/feed.json` |
| `iainchesworthlabs.github.io/virtual-serial-studio/` | ends when Pages is turned off |
| Website pages: `features.html`, `editions.html`, `plugins.html`, `documentation.html`, `support.html`, `security.html`, `licence.html`, `privacy.html` | never published; mapped in `site_layout.json` for the record: `products/`, `pricing/`, `products/plugins/`, `guide/`, `support/`, `security/`, `licensing/`, `privacy/` |
| `docs/user-guide.md` anchors linked from `docs/scenarios.md` | kept as headings in the split pages, or mapped in the layout table: `#command-line-equivalents`, `#network-addresses`, `#keeping-it-safe`, `#rs-232-rs-422-and-rs-485`, `#old-programs-on-windows` |

`site_url` in `mkdocs.yml` is the only place the address is written; pages use `{{ site_url }}`.

## 5. Checks

Python, standard library only, each with tests in `tools/ci/test_site_checks.py` (picked up by the existing
`unittest discover` in the `changelog` job and by `tools-windows`):

- `check_site_kit.py`: the vendored kit against its `SHA256SUMS.txt` (line endings ignored), nothing missing or
  added, and the palette passes `check-palette.mjs` (Node 20 or later).
- `check_site_address.py`: `site_url` is the fixed address and no other file writes the host. The compiled-in
  `virtual-serial-studio.com` addresses are an explicit, listed exception.
- `check_site_layout.py`: the built site against `site_layout.json`: no page lost, canonical addresses right, every
  mapped old address leads to a page that exists.
- `check_site_pages.py`: every page has a title, one `h1` first, no skipped levels, a canonical address, alt text,
  resolving links and anchors, and no link in its own content starting `/virtual-serial-studio/`. The company header
  and footer are exempt. Front pages also need `lang="en-AU"`, a unique title and description, no script except
  JSON-LD, no `style`, width and height on images, nothing from another host, no `mailto:`, and at most one contact
  form held to the endpoint contract.
- `check_claims.py`: no page, README, SECURITY, CONTRIBUTING, CHANGELOG or fragment says the program is certified,
  qualified, compliant, approved or guaranteed. A denial goes in `tools/docs/claims_exceptions.json` with an empty
  `approved_by` that only the owner fills in. Plans are not read. There is no standards list: VSS has nothing to
  hold in one.
- `check_release_data.py` (new, carried over from `site_check.py`): the release data URLs all begin
  `https://github.com/iainchesworthlabs/virtual-serial-studio/releases/`, and a feed copy verifies against the keys
  in `libs/vss_update/src/trusted_keys_release.cpp` through `update_feed.verify_with_keys_file` (and fails if there
  is a feed but no trusted key, as today).
- `check_site_drafts` (carried over): the licence, privacy and editions successors still say DRAFT until the owner
  removes the marks.
- Strict MkDocs build with the `validation:` block (omitted nav files, missing links, missing anchors as warnings).
- `check_messages.py`: only if the owner agrees (asked in Phase 3).

One composite action `.github/actions/docs-site` installs MkDocs from a hash-locked requirements file, builds
strictly and runs the checks. `ci.yml` (no path filter, so it can be required), `docs.yml` and `release.yml`
(early, so a broken page stops a release in a minute) all use it. `.github/ci/plan.json` gets the new paths
(`mkdocs.yml`, `tools/docs/**`) so that a docs-only change does not start every build leg.

Pictures: one script writes light and dark 1280 px grabs for the owner to look at. They are not committed.

## 6. Pull requests

All against `develop`, as drafts, signed off, filled-in body, no attribution. Each stacked PR is based on the branch
below it, and the stack is merged down (top into its base, then the next), after a `git merge-tree` dry run.

| # | Branch | Holds |
|---|---|---|
| 0 | `docs/site-plan` | This plan only. |
| icl-1 | icl-site `feature/add-virtual-serial-studio` ([icl-site#18](https://github.com/iainchesworthlabs/icl-site/pull/18)) | `apps.ts` entry (`virtual-serial-studio`, repo id 1442223332, role `icl-site-deploy-virtual-serial-studio`, `publishRef: 'site'`), protected prefix, README, the test lists, and the fallback tile manifest `data/apps/virtual-serial-studio.json` (status coming soon) that the company site's tests require for every app. Small, so that previews work early. Until the site deploys, the company tile links to a page that answers 404. |
| 1 | `feature/site-shell` | Kit copy, `mkdocs.yml`, hooks, theme, palette, shell checks, composite action, requirements, `ci.md`. A placeholder home page. |
| 2 | `feature/site-deploy` | `docs.yml` (production, preview, remove-preview, failure notice), the `site` job in `release.yml`, release docs. Previews work for every later PR once the owner has done owner steps 1 to 3. |
| icl-2 | icl-site `feature/second-domain` | Certificate, aliases, edge function rule, tests. |
| 3 | `feature/site-front-pages` | Front pages, release-data hook, feed publishing, `website/` and `site_build.py` removed. |
| 4 | `feature/site-guide` | Guide split, text brought up to the code, screenshots. |
| 5 | `feature/site-claims` and `feature/site-contact-form` | The claims check; the form (after icl-3). |
| icl-3 | icl-site `feature/contact-source-vss` | `SOURCES` entry and topics (proposed: `pro`, `enterprise`, `trial`, `plugins`, `other`). |
| icl-4 | icl-site `feature/link-virtual-serial-studio` | The company site now builds its product lists from each app's published `/<slug>/icl-app.json` (icl-site#17), so there are no hand-written "Coming soon" lines to change. This PR switches `data/apps/virtual-serial-studio.json` to status `available` with the icon, and reruns `node tools/tiles.mjs`. A kit version bump is needed only if the kit files change. Merged once the address answers. |
| 6 | `chore/site-kit-repin` | Re-pin the kit copy to the bumped version. |

Only the release PR edits `CHANGELOG.md`. Website changes count as user-visible for the changelog policy, so PRs 3
and onward carry a fragment or the `no-changelog` label, set before the first push.

Draft PRs run only the planner and static checks, so each stack is marked ready one at a time. `release.yml` is
touched in PR 2 only, kept small so that it does not collide with the release candidate 7 work (#313).

## 7. Owner steps

Written out as exact PowerShell 5.1 commands when each one is due. In order:

1. Merge icl-1 and icl-2, then from the icl-site `infra` folder on `main`: `npx cdk diff`, `npx cdk deploy`. Stop
   on any new or replaced bucket, distribution, certificate or DNS record other than the VSS certificate and
   aliases.
2. Customise the OIDC subject claim for `iainchesworthlabs/virtual-serial-studio` (keys `repo`, `context`,
   `event_name`).
3. Set the four secrets `DOCS_ROLE_ARN`, `DOCS_PREVIEW_ROLE_ARN`, `DOCS_BUCKET`, `DOCS_DISTRIBUTION_ID` from the stack
   outputs `DeployRoleArnVirtualSerialStudio`, `PreviewRoleArnVirtualSerialStudio`, `BucketName`, `DistributionId`.
4. Second domain: remove the Pages custom domain and the GitHub A, AAAA and `www` CNAME records, then the
   certificate validation and the deploy in step 1.
5. Confirm that `support@virtual-serial-studio.com` is an address or alias of the iCloud mailbox that the zone's MX records point at. I change no DNS.
6. Create the `site` branch at a `develop` commit, and any ruleset that lets the release App move it.
7. Redeploy the contact endpoint after icl-3.
8. Tag the release, approve the `release` environment, publish.
9. Set the repository homepage and description.
10. Fill in `approved_by` on any claims exceptions.

`docs.yml` passes `region: us-east-1` explicitly, because the shared workflow defaults to ap-southeast-2 while the
stack is in us-east-1. ADL leaves the default and deploys, so this is a precaution.

## 8. Palette candidates

Each is derived from `apps/vss_gui/src/theme/resources/tokens.json` and passes the kit's `check-palette.mjs`. The
picture of all three, light and dark at 1280 px, was sent with this plan.

- **A, Studio blue** (the app accent `#2160E0`): `primary light-dark(#1f5fe0, #5082e7)`,
  `link light-dark(#1c55c9, #5585e7)`, gradient `#133986` to `#1c55c9` (dark `#091b3f` to `#133986`). Closest to
  the application.
- **B, Graphite and blue**: graphite primary `light-dark(#2b3647, #a9bcdb)`, accent `light-dark(#2160e0, #5b8def)`,
  gradient from graphite `#14171c` to blue `#1f4fb8`. Sits well beside the grey Test equipment family.
- **C, Violet** (the app's plugin colour `#6941C6`): `primary light-dark(#643bc4, #9072d5)`, gradient `#3c2376` to
  `#5a35b1`. Most distinct from the other products. One warning: dark tint against surface is 1.02 (minimum 1.03).

## 9. Findings

Differences between the brief and the repositories, and stale text to fix in the guide phase.

- **The `docs/website-rc6-match` commit is already in `develop`** (merging it gives the identical tree, via release
  candidate 7, #310), so there is nothing to fold in.
- **`website/README.md` rules versus the owner's direction:** it asks for no marketing wording and no stated email
  address; the owner's later direction is a product site and shows `support@`. The owner's direction is followed;
  plain Australian English still applies.
- **`tools/docs/release/release.json` still says rc.5**, and there is no `tools/docs/release/update-feed.json`.
- **No update can verify on `develop` today:** no feed key is built in.
- **Stale design pages** (the code and `changelog.d/` are right, the pages are wrong): `README.md:49` and
  `docs/plugins.md` say plugins do not run (the host, sandbox on Windows, Plugins page and `vss_cli plugin run` are
  built); `docs/editions.md` lines 8 to 19 and 246 say there is no plugin host or Plugins page;
  `docs/licensing.md` lines 499 and 700 to 706 say the gateway and plugin host do not read the licence (both do);
  `docs/updates.md:486` says the Plugins page is not built; two fragments say nothing in Studio uses the plugin host
  or licence check. Response-timing rules are a library only, with no way in from Studio or `vss_cli` yet, so
  `docs/editions.md:26` is wrong to promise them for the next release candidate.
- **Not built, so "Planned":** official plugins (none in the repository), the Microsoft-signed driver, remote use
  with TLS, bindings other than Python, user-reachable response timing and fault injection from tests, licence gating
  of features, and update checks (inert without keys). The agent and gateway are not in any package yet.
- **Company-site README is stale** in three places (it says "variables" where the workflow reads secrets, the
  protected-prefix sentence, and the `develop` publish note). The icl-1 PR corrects them.
- **The company site changed while this was being planned** (icl-site#17, 19 Nov): product lists in the menu, footer, family pages and home tiles are now generated from each app's `/<slug>/icl-app.json` and a fallback in `data/apps/`. VSS publishes `icl-app.json` and `theme.css`, and its tile then appears at the company site's next deploy.
- **`CHANGELOG.md`'s compare link** still names `iainchesworth/`. That is the release PR's to fix.
- **Branch names with dots fail precheck**, and `python3` on this machine is the Store stub, so tools are run with
  `python`.
