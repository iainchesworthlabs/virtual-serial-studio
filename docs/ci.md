# Continuous integration

Development lands on `develop`, the trunk and default branch, through pull
requests and a merge queue. `main` is the release branch: the release workflow
moves it to each final release tag.

## The gate

`.github/workflows/ci.yml` runs the cheap checks on every pull request and the
expensive ones as late as it can. `tools/ci/plan_legs.py` decides which jobs a
run needs, from the event and the files changed, and the same answer goes to the
`CI Status` check, so a job and its check cannot disagree.

| Tier | When | Runs |
|---|---|---|
| Draft pull request | A draft is open or updated | The static jobs only: the shared policy checks (branch name, commit and pull request text, file text, action pinning, REUSE, vcpkg baseline), changelog fragments, the tests of `tools/` on Linux and on Windows, design tokens, the documentation site build and the workflow analysis |
| Ready pull request | A pull request is ready for review | Linux GCC build and tests, AddressSanitizer with UBSan, the fuzz corpus replay; ThreadSanitizer when threading code changed |
| Merge queue | An entry is queued | The same on the merged tree, plus Windows MSVC, ThreadSanitizer, the Qt GUI build with its self-test and journey tests, the performance floors when library code changed, macOS when a change can affect it, and the Windows driver when the driver changed |
| Push to `develop` | After the merge | The legs the queue does not run: Linux Clang, macOS, the coverage gates, the Tracy profile of the end-to-end path and the Windows driver; one run at a time with at most one waiting |
| `workflow_dispatch` | On request | Every job (`gh workflow run ci.yml --ref <branch>`) |

A change to documentation, the changelog or licence text skips the builds. The
list is in `.github/ci/plan.json`: `docs/`, the top-level `README.md`, `CHANGELOG.md`,
`CONTRIBUTING.md`, `SECURITY.md` and `AGENTS.md`, and the `README.md` files under
`apps/`, `libs/`, `cmake/` and `adapters/`, and the tooling that builds
and checks the documentation site (see [Documentation site](#documentation-site)). It
does not hold `THIRD_PARTY_NOTICES.md`, which a GUI test compares with
`third_party.json`, so a change to it builds.

The sanitizer jobs run the GUI suites too; see [Testing](testing.md#sanitizers).
Boost is fetched by `tools/fetch_boost.py`, so no job depends on a system Boost.

The branch protection ruleset requires two checks, `CI Status` and
`Scan dependency diff`. `CI Status` fails when any job the plan asked for failed,
was cancelled or was skipped.

## Other workflows

| Workflow | Runs | Does |
|---|---|---|
| `fuzz.yml` | Push to `develop`, every night | Fuzzes each target for 60 seconds, or 10 minutes at night, within a budget for the whole run; see [Fuzzing](fuzzing.md) |
| `release.yml` | A `v*` tag, every Monday, on request | Builds the packages and publishes them for a tag; see [Releasing](releasing.md) |
| `docs.yml` | A pull request that changes the docs (also when it closes), a push to `develop` or `site` | Checks the presets and badges against the repository, then builds this site with `mkdocs build --strict` and checks it (see [Documentation site](#documentation-site)). A push to `site` first refreshes the release data and the update feed from GitHub (see [Release data and the update feed](#release-data-and-the-update-feed)). A pull request from this repository gets a preview on the company site, and a push to `site` is checked, so that only site files or a release change it, and then deploys the live site (see [Publishing](#publishing)). The push to `develop` gives the branch a status of its own |
| `publish-site.yml` | A push to `develop` that changes a site file, on request | Shows which site files would change on the branch `site`, waits for the owner's approval in the environment `site-release`, then publishes them without a release (see [Publishing the site between releases](#publishing-the-site-between-releases)) |
| `cache-hygiene.yml` | After a CI run on `develop`, a closed pull request, on request | Deletes Actions caches that nothing can restore any more, to keep the repository within its cache quota |
| `dependabot-auto-merge.yml` | Dependabot pull requests | Turns on auto-merge for minor and patch updates of actions |

## Documentation site

The pages in `docs/` are built into a static site with MkDocs and Material (`mkdocs.yml` at the root of the repository). The site is
the `/virtual-serial-studio/` section of <https://iainchesworthlabs.com/virtual-serial-studio/>, one of the company's sites for its
programs. The Markdown stays the source and stays readable on GitHub; nothing in a page is written for the site. The plan for the whole
site, and what is built so far, is [plan/site.md](plan/site.md).

Locally:

```sh
python -m pip install --require-hashes -r requirements/docs.txt
mkdocs serve                                  # read it while editing
mkdocs build --strict                         # what CI runs; writes build/docs
python tools/ci/check_site_kit.py             # the checks of this section; the last three take the built site
python tools/ci/check_site_address.py
python tools/ci/check_site_layout.py --site build/docs
python tools/ci/check_site_pages.py --site build/docs
python tools/ci/check_release_data.py --site build/docs
python tools/ci/check_claims.py               # reads the repository's text, not the built site
python -I tools/ci/test_site_checks.py        # their own tests; no MkDocs and no built site needed
python -I tools/ci/test_site_release.py       # the tests of the release data hook and of the refresh; no network
python -I tools/ci/test_site_changelog.py     # the tests of the What's new hook
```

Set `NO_MKDOCS_2_WARNING=true` to keep MkDocs from printing its notice about version 2. `requirements/docs.txt` is made from
`requirements/docs.in` (the command is in the first lines of each file) with `uv pip compile --generate-hashes`, so every package is
checked against its hash. Dependabot's pip ecosystem keeps both files current. MkDocs stays on 1.6: version 2 removes the plugin
system and the theme API that `mkdocs.yml` uses.

**What strict means here.** MkDocs' own strict mode fails on warnings, but it reports a link to a missing page, a link to a heading
that is not there and a page missing from the navigation at a level that strict mode lets through. The `validation` block of
`mkdocs.yml` raises all of them to warnings, so each fails the build. A page that is added to `docs/` has to be in `nav` in
`mkdocs.yml`, or in `not_in_nav` if it is a front page, or in `exclude_docs` if it is not published, and in
`tools/docs/site_layout.json`.

### The pages of the site

The site has three kinds of page.

- **Front pages.** Markdown with `template: marketing.html` in the front matter. That template (`tools/docs/theme/marketing.html`) is
  plain HTML on the company shell. It does not extend Material and ships no script. Their links are relative. They are the home page,
  Products (Studio, Agent and gateway, Plugins) and Solutions, Downloads (`docs/downloads.md`, made from the release data: see [Release data and the update
  feed](#release-data-and-the-update-feed)), Pricing, Quality and trust, Security, What's new, FAQ, Support, Licensing (`docs/licence.md`,
  because `docs/licensing.md` is the design of licence files), Privacy, Accessibility and Press. What's new is made from `CHANGELOG.md`
  (see [What's new](#whats-new)). The menus of the app bar are one list in `tools/docs/theme/partials/links.html`; a menu with items
  opens as a native `<details>`, so it works with no script, and below 64rem the menus fold into one Menu. Only pages that exist are in it. The
  bar has the menus Products, Solutions and Resources (the User guide, Quality and trust, Security and the rest) and the plain links User guide,
  Developers and Pricing. There is no Platforms section and no Products overview: what each system needs is on Downloads and in the guide.
- **The guide** (`/virtual-serial-studio/guide/`) **and the pages for programmers** (`/virtual-serial-studio/developers/`). They are
  Material pages, with Material's colours set from the palette and from the browser's own light and dark colours
  (`docs/assets/guide.css`). The menu holds these two groups only.
- **Pages that are in the repository only.** The pages for the people who work on the project (this one, releasing, fuzzing, profiling,
  the virtual machines, the languages and the application icon), the design records, proposals and plans, and the design bundle
  (`docs/gui-design/`) are not published. `exclude_docs` in `mkdocs.yml` lists them, and a link to one from a published page becomes a
  link to the file on GitHub, on `develop` (`tools/docs/mkdocs_hooks.py`). The design records are excluded for now because the guide
  and the developer pages that describe what is built are written to replace them, and some of them still describe a part as not built
  that is.

### The site layout

The files of `docs/` do not move: the code, the tests, `CONTRIBUTING.md` and the other pages cite them by path. The site's addresses
are different. `tools/docs/site_layout.json` is the table: for each page of `docs/` it gives the address on the site, or `null` when
the page is published on GitHub only. Nothing was ever public at an older address, so the table has no old addresses and nothing
forwards. The user's guide is the folder `docs/user-guide/`: `index.md` is `/virtual-serial-studio/guide/`, and each other page is one address under it
(`docs/user-guide/windows.md` is `guide/windows/`). The pages that are about using the studio are under `guide/` beside them. The pages for
programmers are under `developers/`.

- `tools/docs/site_layout_hook.py` applies the table while the site is built, and stops the build when it and `exclude_docs` disagree.
- `tools/ci/check_site_layout.py` checks the table and the built site: every file in the table is in `docs/`, every page of `docs/` is
  in the table, every page of the navigation has an address, each page is in the built site with the right canonical address, and no
  page sits at an address that the table does not give.
- A new page needs an entry, with an address or `null`, and the matching line in `nav`, `not_in_nav` or `exclude_docs`.

### The site address

The address is fixed: `https://iainchesworthlabs.com/virtual-serial-studio/`. The company site serves each program under one path, and
the role that deploys this repository may write that path only. `site_url` in `mkdocs.yml` is the one place that holds it. A page
writes `{{ site_url }}` where it needs the address, and `tools/docs/site_address_hook.py` fills it in.

The program has another address compiled in, `https://virtual-serial-studio.com` (the update feed, the documentation and the downloads
page). It is not the site's address, and the program does not change for the site. `tools/ci/check_site_address.py` fails when
`site_url` is another address, when a page of `docs/` other than this one and the plans writes the site's host or the address of the old
GitHub Pages site, when a file writes the compiled-in host and is not one of the files that `COMPILED_IN` in the script lists (the
program's source, tests and translations, the changelog, a sample feed, the pages that say what the program does with
that address, and the Support and Press pages, which write the support address `support@virtual-serial-studio.com` as plain text), and when `README.md` links to the site's host at another address than `site_url`.

### The company kit and the palette

The pages use the company shell: its stylesheet, header and footer. `tools/docs/site-kit/v1/` is a pinned copy of the company's kit
(version 1.1.2, from the company site's repository, `iainchesworthlabs/icl-site`, at commit `bca4a31` on `main`). Nothing is fetched
when the site is built or checked, and the files are not edited. `tools/ci/check_site_kit.py` fails when a file differs from the hash
in the copy's own `SHA256SUMS.txt` (line endings are not compared), when a file is missing or added, and when the palette does not pass
the company's checker. To move to a newer kit, copy its files over the copy (the whole of `/assets/v1/`, same layout, without
`favicon.svg`, which `SHA256SUMS.txt` does not list), run the check and say the version in the pull request.

The palette is `docs/theme.css`: one rule, `[data-app="virtual-serial-studio"]`, and the eight colour roles, each
`light-dark(light, dark)`. The company checker (`kit/check-palette.mjs`, Node 20 or later) holds it to WCAG 2.2 AA contrast, light and
dark, against the shell's own neutrals; it wants the file in a folder named for the program, so the check copies it into one. The palette
is graphite and blue: the application's blue (`apps/vss_gui/src/theme/resources/tokens.json`) for the accent and links, graphite for
buttons and the hero. Change a colour in `docs/theme.css` and run `python tools/ci/check_site_kit.py`. The check does not cover the
guide's own pages (code blocks, admonitions, search); their colours come from Material and are looked at in the browser.

`docs/icl-app.json` is the company's tile manifest: the name, one sentence, the platforms, the status and the icon, which the company site
reads to list this program in its menus and on its family page. Its fields and limits are in the kit's README ("Show up in the product
lists"), and `tools/ci/test_site_checks.py` holds the file to them.

### The rules for the pages

`tools/ci/check_site_pages.py` reads the built site. It builds the site itself when it is not given one (`--site <folder>` checks a site
that is built already). Every page must have a title, one `h1` that is the first heading of its content, headings that do not skip a
level, a canonical address, an alternative text on every picture, no `mailto:` link, and links that lead to a file of the site and to an
anchor that is in it. A link that starts with `/` is the company shell's (`/`, `/support/`, `/assets/v1/...`) and is not followed, but
none in a page's own content may start with `/virtual-serial-studio/`: the pages use relative links so that they work under a preview
address. The company header and footer are the kit's, and link to the program's page as every page of the company site does; the check
leaves their links alone.

The front pages are held to more, because a preview blocks every script and the company's contract asks for plain pages: `lang="en-AU"`;
a description, and a title and description that no other front page has; no `script` except a JSON-LD data block; no `style` element or
attribute (the rules are in `docs/assets/vss.css`); a width and a height on every picture; nothing loaded from another host; and, at
most, one contact form. The form is not on a page yet. The script already holds one to the contract of the company's contact endpoint
(`FORM_*` in `tools/ci/check_site_pages.py`): `method="post"` to `/api/contact` only, the eight field names, `required` and `maxlength`
as the endpoint expects, the list of topics, a `label for` every field, a honeypot field inside an `aria-hidden` element and out of the
tab order, and the consent box with its link to the company's privacy text. Stylesheets may not `@import` or load from an address.

`FIXED` in the script lists the files that must be in the built site: the home page, the guide, developer, Downloads, Privacy and Support
pages (the company's contract asks for the last two), `theme.css`, `icon.svg` and `icl-app.json`.

The pages `licensing/`, `privacy/` and `pricing/` are drafts until the owner says they are not. `check_site_drafts` in the same script
fails when one of them is in the built site and its main content no longer contains the word DRAFT. A page that is not built yet is not
an error, and the script says which are not there. The check goes when the owner takes the marks off.

### Links out of `docs/`

A page may link to the README, the changelog or a source file with a relative link (`../README.md`), which works on GitHub. The site has
no such file, so `tools/docs/mkdocs_hooks.py` turns the link into one to the file on GitHub (`blob/develop/...`, or `tree/develop/...`
for a folder). It is `develop` and not `main`, because `main` only moves when a final release is published. The same applies to a link
to a page that is not published. A link whose target does not exist is left alone, and the build fails on it.

**Icon.** The logo and the favicon are the application's icon. The same hook adds it to the site as `icon.svg`, from
`apps/vss_gui/assets/icon/vss_gui.svg`, where the application takes it, so there is one copy. That is the address the company's contract
gives an app's icon.

**Version.** `tools/docs/site_version_hook.py` reads the version from `project(virtual_serial_studio VERSION ...)` in `CMakeLists.txt`,
and the templates say which version the pages describe.

### Release data and the update feed

The Downloads page and the update feed are made from three files in `tools/docs/release/`. The folder is outside `docs/`, so MkDocs does
not publish the files as they are.

| File | What it is | Who writes it |
|---|---|---|
| `release.json` | The newest release of the repository: its tag, date, whether it is a pre-release, its address on GitHub, and each file with its name, size and address | `tools/docs/refresh_release.py` |
| `download_rules.json` | How the files of the release are sorted on the Downloads page: the group of each file, its label and the order | A person |
| `update-feed.json` | The signed update feed of the newest release that has one (the `feed` job of `release.yml` attaches it to the release). The file may not exist | `tools/docs/refresh_release.py` |

`tools/docs/site_release_hook.py` uses them when the site is built:

- It replaces `<!-- releases:downloads -->` in a page's Markdown with the table of files: one table for each system, with the size of each
  file and a link to its `.asc` signature. Every address is a link to github.com, and the hook refuses any other. It also replaces
  `<!-- releases:version -->` (`0.1.0-rc.5`), `<!-- releases:tag -->` (`v0.1.0-rc.5`), `<!-- releases:date -->` (`15 November 2026`),
  `<!-- releases:kind -->` (`final release`, `release candidate` or `pre-release`) and `<!-- releases:url -->` (the release on GitHub).
  Any other `releases:` marker stops the build. The same values are in `config.extra.release` for the templates.
- If `update-feed.json` exists it copies that file, byte for byte, to `updates/v1/feed.json` of the built site. The file is not read as text
  and not changed. If it does not exist the site has no `updates/` folder, and the build logs a plain message.

**Who refreshes them.** The Docs workflow does, on a push to `site` and on no other event: a step before the build runs
`python3 tools/docs/refresh_release.py` with the job's own token (`GH_TOKEN: ${{ github.token }}` and `contents: read`). The release
workflow moves `site` after its `feed` job, so that run sees the release and its feed. A pull request, a push to `develop` and the
`Docs (strict)` job of the release workflow build from the files as they are committed. The refresh in the Docs run is not committed, so
the committed `release.json` is only as new as the last time someone committed it.

**To refresh by hand:**

```sh
python tools/docs/refresh_release.py
```

It makes one request to the GitHub API for the releases of the repository that `repo_url` of `mkdocs.yml` names, pre-releases included
and drafts not, rewrites `release.json`, and downloads `update-feed.json` of the release with the greatest version that has one. The
feed is limited to 64 KiB, each request to 15 seconds, only https is used, and a redirect is followed only to GitHub's own hosts. If a
variable `GH_TOKEN` or `GITHUB_TOKEN` is set, it is sent to the API as a Bearer header (and to nothing else), and never printed. Both
files are written only after everything was read, so a failure changes nothing. A feed that is committed must verify with the keys in
`libs/vss_update/src/trusted_keys_release.cpp`, or the check below fails.

**What the deploy does.** The deploy copies the built site, so `updates/v1/feed.json` arrives at
`https://iainchesworthlabs.com/virtual-serial-studio/updates/v1/feed.json` with the bytes of `update-feed.json`. Studio reads
`https://virtual-serial-studio.com/updates/v1/feed.json`, which it has compiled in. The plan (`docs/plan/site.md`, decision A) is that the
company stack answers that address directly with the same file, with no redirect, because Studio refuses a redirect for the feed. That
rule is in the company stack's repository and not here.

**The check.** `tools/ci/check_release_data.py` holds three things. Every address in `release.json` begins with `repo_url` of
`mkdocs.yml` and `/releases/`. If the built site has `updates/v1/feed.json`, it verifies with the keys in `trusted_keys_release.cpp`
through `update_feed.verify_with_keys_file` (a feed when that file lists no key is an error, because no Studio would accept it; no feed
means nothing to check). And the built feed is the same bytes as `update-feed.json`, with a feed in one place and none in the other
reported. The action `docs-site` runs it with the other checks.

### What's new

`docs/whats-new.md` holds the marker `<!-- changelog:releases -->` on a line of its own. `tools/docs/site_changelog_hook.py` replaces it
with the released sections of `CHANGELOG.md` (`## [1.2.3] - 2026-12-01`) as Markdown, each as an `h2` with an address of its own
(`#v1-2-3`), a link to the release on GitHub and the section's text. The categories under a version stay one level below it, so the page
keeps one `h1`. The Unreleased section, the link definitions at the foot of the file and the fragments in `changelog.d/` are not read. A
relative link in an entry is relative to the repository, and becomes a link to the file on GitHub; an address that starts with `http`
stays as written, and `check_site_pages.py` allows only GitHub and gnu.org on a front page. With no released section the marker becomes one
sentence that says so, and, if the changelog has entries under Unreleased, an Unreleased heading with them. An unknown `changelog:` marker
stops the build. `tools/ci/test_site_changelog.py` holds the hook, and the real `CHANGELOG.md`, to this.

### The claims

The project makes no claim of certification, qualification, compliance, approval by anyone or a guarantee, because it cannot show one.
`tools/ci/check_claims.py` reads `README.md`, `SECURITY.md`, `CONTRIBUTING.md`, `CHANGELOG.md`, the fragments in `changelog.d/` and
every Markdown page of `docs/` except `docs/plan/` (the plans have to discuss what the product is not) and `docs/gui-design/` (the design
bundle quotes its own review language), and fails on each use, in any case, of the five adjectives that name those claims (they are listed
in `WORDS` at the top of the script) that no exception covers. A longer word, such as the noun for a certificate or the negative of one of
them, is not a use. It also fails on the name of a standard for safety-critical software anywhere it reads (`STANDARDS`, in the script): the
project does not claim to support one, so there is no list of pages that may name them.

An exception is an entry of `tools/docs/claims_exceptions.json`: one file, the words around the word (line breaks and case do not matter),
and a reason the use is not a claim: a denial of a promise, a label that Studio shows, or a person accepting a key or a decision. An
exception that matches nothing is an error, so the list holds only what the pages need. Only the owner fills in
`approved_by` of an entry. The check passes while entries wait for that and says how many; `check_claims.py --strict` fails until none
waits, which is how the owner holds a release to a list in which every entry has a name against it. `tools/ci/test_check_claims.py` holds the check, and the real
repository, to this.

### In CI

`.github/actions/docs-site` installs MkDocs from the hash-locked `requirements/docs.txt` (`uv pip install --require-hashes --no-build`),
builds strictly, and runs the checks of this section in order (`check_site_kit.py`, `check_site_address.py`, `check_site_layout.py`,
`check_site_pages.py`, `check_release_data.py`, `check_claims.py`). Three workflows use it, so they cannot disagree:

- `ci.yml`, job `Docs build (strict)`: on every run of the workflow: a pull request, draft or ready, a queue entry, a push to `develop` and a run by hand. It has no path filter, because a required
  check that does not run blocks the pull request; `CI Status` waits for it like it does for the other static jobs.
- `docs.yml`, job `Build site`: on a pull request that changes the documentation, and on a push to `develop` or `site`.
- `release.yml`, job `Docs (strict)`: on a tag, before the packages are built, so that a broken page stops a release in a few minutes.

The checks' own tests are `tools/ci/test_site_checks.py`: each check passes a good tree and fails, with a reason, on a bad one. The
release data hook and the refresh have theirs in `tools/ci/test_site_release.py`, which never uses the network, and the What's new hook has
`tools/ci/test_site_changelog.py`; the claims check has `tools/ci/test_check_claims.py`. The `changelog` job of `ci.yml` runs all four with
the other tests of `tools/ci`.

A change to the documentation tooling alone (`mkdocs.yml`, `tools/docs/`, `tools/ci/site_*.py`, `tools/ci/check_site_*.py`,
`tools/ci/check_release_data.py`, `tools/ci/check_claims.py`, `tools/ci/test_check_claims.py`, `tools/ci/test_site_*.py`, `tools/ci/test_publish_site.py`, `requirements/docs.*` and `.github/actions/docs-site/`) is listed under
`docs` in `.github/ci/plan.json`, so it does not start the build legs. These are also the site paths of
[Publishing the site between releases](#publishing-the-site-between-releases), which can put such a change live without a release.

### Publishing

The site is the `/virtual-serial-studio/` section of <https://iainchesworthlabs.com>, served from the company stack in the
`iainchesworthlabs/icl-site` repository. `docs.yml` calls the shared `docs-deploy-aws.yml` of `icl-common-infra` (v0.1.30) three ways, all
with `prefix: virtual-serial-studio` and `region: us-east-1`:

- **Production**, on a push to the branch `site` (`production-ref: refs/heads/site`). The release workflow moves `site`; see
  [Releasing](releasing.md#the-site).
- **A preview** for each pull request from this repository that changes the documentation, at
  `https://iainchesworthlabs.com/_preview/virtual-serial-studio/pr-<n>/`. The workflow comments the address on the pull request. A preview
  is public to anyone with the link, is `noindex`, and cannot run script or submit a form (its content security policy forbids both), so
  the search box and a form are tried on the live site.
- **Removal of the preview** when the pull request closes.

A pull request from a fork gets no secrets, so no preview. A run without the four secrets skips the deploy with a warning, and a run by
hand cannot deploy, because the role the company stack creates trusts only a push to `site` and a pull request of this repository.

**What the owner does** (the company stack and this repository's settings are not changed by a pull request here):

1. In `iainchesworthlabs/icl-site`, merge the pull request that adds the `virtual-serial-studio` entry to `infra/lib/apps.ts`, then from
   `main` run `npx cdk diff` and `npx cdk deploy` in its `infra` folder. The outputs include `DeployRoleArnVirtualSerialStudio` and
   `PreviewRoleArnVirtualSerialStudio`.
2. Customise this repository's OIDC subject claim (immutable subject, claim keys `repo`, `context`, `event_name`), as the icl-site README says.
3. Set the four repository secrets `DOCS_ROLE_ARN`, `DOCS_PREVIEW_ROLE_ARN`, `DOCS_BUCKET` and `DOCS_DISTRIBUTION_ID` from the stack outputs
   (`DeployRoleArnVirtualSerialStudio`, `PreviewRoleArnVirtualSerialStudio`, `BucketName`, `DistributionId`).
4. Create the branch `site` at a commit of `develop`, and allow the release bot (the GitHub App of `RELEASE_BOT_APP_ID`) to push to it in
   the rulesets. That first push also publishes the site; every later release moves it.

The old GitHub Pages site never served anything and is no longer published from this repository. Turn Pages off in the repository settings
when the second domain answers (the plan, `docs/plan/site.md`, has the steps).

### Publishing the site between releases

A release moves `site`, so a fix to a page would wait for the next release. `publish-site.yml` ("Publish the site") puts a change that
touches only site files live without a release and without the application CI.

**The site paths.** `tools/docs/site_paths.json` is the one list of what the site is made of: `docs/**`, `mkdocs.yml`,
`requirements/docs.in` and `requirements/docs.txt`, `tools/docs/**`, `.github/actions/docs-site/**`, and the site's checks and their
tests in `tools/ci/`. A pattern is relative to the root of the repository; `*` stays within one folder name and `**` crosses folders. The
`paths:` of the push trigger in `publish-site.yml` is the same list, and `tools/ci/test_publish_site.py` fails when the two differ. The
list leaves out `CHANGELOG.md`, `README.md`, `CMakeLists.txt`, `libs/`, `apps/`, the workflows and everything else.

**What the workflow does.** A push to `develop` that changes a site path starts it, and so does a run by hand from `develop`.

1. `plan` compares the site paths of the pushed commit with the site paths of `site`, and writes the files that would change into the
   job summary (A added, M changed, D deleted). When nothing differs it says so and the run ends. When the branch `site` does not exist
   yet it says that and ends: the first release creates it.
2. `release` runs in the environment `site-release`. When the environment has a required reviewer, the job waits there until the owner
   approves it. Then it builds one commit with `tools/docs/publish_site.py build`: the tree of `site` with each file under a site path
   replaced by its version in the pushed commit (a site file that the pushed commit no longer has is deleted), and `site` as its only
   parent. Nothing outside the site paths is touched. It pushes the commit to `site` with the release bot's App token
   (`RELEASE_BOT_APP_ID` and `RELEASE_BOT_PRIVATE_KEY`, the secrets that `release.yml` uses). The push is never forced, so it is a
   fast-forward or it fails. The token reaches git through the environment and is masked, so it is on no command line. The job summary
   holds the commit and the files.
3. The push is what `docs.yml` listens for: it builds the site, and the deploy runs as it does for a release.

Runs go one at a time, in the order of the pushes (`concurrency: publish-site`), and a run that is waiting is not cancelled by a later
one. Each run publishes the site files of the commit that started it, so approve the newest run when there is more than one. A run whose
commit is already in the history of `site` (a release was cut after it) publishes nothing.

**What is never published this way.** Application code, the build files, the workflows, `CHANGELOG.md` and `README.md` reach `site` only
with a release, so the app code on `site` stays at the last release until the next one. A page that depends on a code or build change (a
new download, a changed command) goes out with the release that has the change.

**The guard.** The rulesets decide who may push to `site`, so the Docs workflow also checks every push to it before it deploys. The job
`guard` runs `tools/docs/publish_site.py guard --before <sha> --after <sha>`. The push passes when every file it changed is a site path, or
when the tree of the new tip is the tree of a tag that matches `v*` (the move of a release: a fast-forward to the tag, or the merge commit
described below). A push that creates the branch passes. Any other push fails the job and lists the files that are not site paths, and the
`deploy` job, which needs the guard, does not run.

**The environment `site-release`.** The workflow names the environment and the owner creates it, because a pull request does not change
repository settings. With a required reviewer on it, the `release` job waits for that person. Without a reviewer the environment does not
wait, and the workflow publishes at once. This command, in PowerShell 5.1, makes the environment with the owner as the reviewer:

```powershell
$id = gh api user --jq .id
$body = ConvertTo-Json -Compress -Depth 5 -InputObject @{ reviewers = @(@{ type = "User"; id = [int]$id }); deployment_branch_policy = $null }
$file = Join-Path $env:TEMP "site-release.json"
[System.IO.File]::WriteAllText($file, $body, (New-Object System.Text.UTF8Encoding($false)))
gh api --method PUT repos/iainchesworthlabs/virtual-serial-studio/environments/site-release --input $file
Remove-Item $file
```

The release bot must be allowed to push to `site` in the rulesets, as it is for a release (step 4 of [Publishing](#publishing)).

**How a release still moves `site`.** The `site` job of `release.yml` always makes a fast-forward. It reads where `site` is and asks
the compare API for `<site>...<tag>`. When the answer is `ahead` or `identical` (nothing was published between releases) it moves `site`
to the tag. When `site` has commits that the tag does not have, it makes a merge commit through the API, with the tree of the tag and
with the current `site` and the tag as parents, and the message "Publish the site from <tag>", and moves `site` to that commit with
`force=false`. The site files that were published in between are in the tag already, because they came from `develop`. When the answer is
`behind`, `site` is already past the tag (a re-run of an old release finds it so), and the job leaves `site` alone and says so. See
[Releasing](releasing.md#the-site).

The tests in `tools/ci/test_publish_site.py` use temporary git repositories: a site-only change is published and a code change is not
copied, a deleted site file is deleted, a file outside the site paths is left alone, nothing to publish exits with 3, and the guard
accepts a site-only push, a release tree and a new branch and refuses a push that changes code. They cannot run the workflows, which run
only on GitHub.

## Windows signing

Release packaging and the driver job sign the Windows programs with the
`sign-windows` action of `icl-common-infra`, through the local action
`.github/actions/sign-windows-binaries`, which also verifies the result. The
`SIGN_WINDOWS` repository variable chooses the mode (`false`, `test`,
`production`); the secrets `WIN_TESTSIGN_PFX_BASE64` and
`WIN_TESTSIGN_PFX_PASSWORD` hold the test certificate.

- **Never on a pull request build** of the packages (`release.yml` passes mode
  `false` on a pull request), and never for a pull request from a fork, which has no
  secrets.
- **Without the secrets** the step writes a warning and a note in the job summary
  and leaves the files unsigned; the leg passes.
- **With them** every file is checked: a signature, an RFC 3161 timestamp and an
  intact hash, plus a manifest that never asks for administrator rights (the
  installer, checked with `installer: "true"`, is the one file that may). A
  failure fails the leg. In `production` mode the chain must also be trusted
  and `signtool verify /pa` must pass.

`tools/ci/verify-windows-signatures.ps1` runs by hand too. How to test it with a
self-signed certificate is in [the Windows driver notes](windows-com-driver.md#signing-the-studios-windows-programs).

## Nightly analysis

These run on a schedule and on request, never as a required check, so a finding
never blocks a merge. The static analysers also run on a pull request that
changes source, workflows or analysis settings (not on a draft), over the files
the pull request changes. The first five rows report to Security >
Code scanning as SARIF, and a nightly failure on `develop` opens or updates one
issue labelled `nightly-analysis`; the owner closes it once the findings are
fixed. Each analyser has a local command; see [Testing](testing.md#static-analysis).

| Analysis | Workflow | Where the result lands |
|---|---|---|
| CodeQL: C++ (libraries, command line tool, GUI, tests), Python and Actions | `codeql.yml` | Code scanning alerts; a failed run opens an issue |
| MSVC code analysis (PREfast) over the Windows build, GUI and tests included | `msvc-analysis.yml` | Code scanning alerts under the tool PREfast; new alerts open an issue |
| clang-tidy: bugprone, concurrency, performance, portability, misc, analyzer, naming | `static-analysis.yml` | Code scanning alerts under the tool clang-tidy; a nightly failure opens an issue |
| cppcheck | `static-analysis.yml` | Code scanning alerts under the tool cppcheck; a nightly failure opens an issue |
| zizmor, with the same rules as the pull request gate | `static-analysis.yml` | Code scanning alerts under the tool zizmor |
| Newest GCC and Clang, GCC ASan/UBSan, TSan, MSVC ASan | `nightly.yml` | The job and an issue |
| Fuzzing, 10 minutes per target | `fuzz.yml` | The job and an issue |
| Hosted platform matrix | `platform-matrix.yml` | The job and an issue |

**A job that runs out of time.** GitHub cancels it; it does not fail it, and
`failure()` is false for a cancelled job. Every step that opens the issue (the
report of `nightly.yml`, `static-analysis.yml`, `codeql.yml`, `fuzz.yml` and the
platform matrix, and the PREfast job of `msvc-analysis.yml`) therefore runs when
the job failed, or was cancelled in a scheduled run. `tools/ci/test_workflows.py` fails
when one is written with `failure()` alone (a timeout would pass unseen) and when one
reports any cancelled run (a run started by hand and cancelled by hand would open an
issue for each job in progress, 14 for `nightly.yml`). What each does:

| The job | Issue | Crash input kept (fuzz) |
|---|---|---|
| Succeeds | None | None |
| Fails | Opened, or a comment added to the open one | Uploaded |
| Runs out of time (cancelled by GitHub) in a scheduled run | Opened or commented, with the same text as a failure | Uploaded |
| Runs out of time in a run started by hand | None; the person who started it is watching it, and `release_preflight.py` still lists the run as not successful | Uploaded |
| Is cancelled by hand | None for a run started by hand. A scheduled run cancelled by hand is reported like a timeout, because a step cannot tell the two apart | Uploaded |

A pull request run reports nothing. A failure on `develop` is reported for a scheduled
or a manual run; for `fuzz.yml` and the platform matrix only a scheduled run reports.

The platform matrix also looks at the result of its cell jobs: a cell that ran out of
time uploads no result, and the table would look complete without it, so the report
job fails when any cell job did not succeed. `release_preflight.py`
lists a run that did not succeed, or is older than two days, as unmet
([Releasing](releasing.md#before-you-tag)).

The sanitizers and fuzzing do not upload to code scanning. Their findings are
failed tests and reproducers, not alerts that can be dismissed.
Include-what-you-use, OSV-Scanner and OpenSSF Scorecard are not run yet.
