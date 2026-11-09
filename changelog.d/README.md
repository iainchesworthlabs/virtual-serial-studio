<!-- Copyright (c) 2026 VirtualSerial contributors -->
<!-- SPDX-License-Identifier: BSL-1.0 -->
<!-- Derived from the Boost.VirtualSerial repository. -->

# Changelog fragments

This directory holds one file per user-visible change. `CHANGELOG.md` is
assembled from these fragments at release time, following
[Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/).

The point is merge-conflict avoidance: every pull request adds its own file, so
two branches never edit the same lines of `CHANGELOG.md`. The release manager
folds the pending files into a dated version section with one command.

## Adding a fragment

Create `changelog.d/<slug>.<category>.md`:

| Part | Values |
|---|---|
| `<slug>` | lowercase letters, digits and hyphens, e.g. `async-timeouts` or `1217` |
| `<category>` | `added`, `changed`, `deprecated`, `removed`, `fixed`, `security` |

Examples:

```
changelog.d/async-contract.added.md
changelog.d/1217.fixed.md
```

The content is Markdown. Write one entry, or several `-` bullets:

```markdown
Added `read_timeout` instrumentation that reports the elapsed wait when a read
times out.
```

```markdown
- `mock_port` can now inject an overrun independently of the FIFO capacity.
- `rfc2217_port` reports a parity error from `NOTIFY-LINESTATE`.
```

A bullet may continue over several lines; a blank line starts a new entry.
Fragments are plain text, so no front matter and no tooling beyond Python's
standard library is required.

Files whose name starts with `_` or `.`, and this `README.md`, are ignored by
the tooling, which is why `_template.added.md` can live here.

## What a fragment is for

A fragment is a line in the release notes that a person who installs the program
will read. The notes are the product's changelog, not the project's diary, so
write for that reader:

- **Start with a bold headline that says what the user can now do or what no longer
  goes wrong**, then explain it in plain words: what it does, where to find it, what
  it does not do, and what to do about a limit. Name the menu, button, command or
  setting. Mention a library function only when users call it.
- **One fragment per thing a user would name**, not one per pull request. Several pull
  requests that build one feature extend that feature's fragment.
- **Leave out what only developers see**: continuous integration, test suites,
  sanitizers, static analysis, build presets, refactors, dependency updates and
  internal renames. Those are in the pull request and in `docs/`. If a build or
  contribution change matters to someone building from source, it is a `changed`
  fragment that says so.
- **A fix to something that has not been released is not a `fixed` entry.** Fold it
  into the entry of the feature. A `fixed` fragment describes a problem a user could
  have hit in a published version, in terms of what they saw.
- No work-package numbers, test counts or "X is removed in the next change".
- Say what is not done yet when a user could run into it ("The driver package is not
  production-signed yet").

## The release page

The release page is the changelog section for the version plus a short summary
above it and the install section below it. `docs/releasing.md` ("The release
notes") has the layout.

## Commands

```bash
python tools/changelog.py --check            # validate (run in CI)
python tools/changelog.py --dry-run          # preview the Unreleased section
python tools/changelog.py                    # merge into [Unreleased], delete files
python tools/changelog.py --release 1.1.0    # cut a dated release
```

`--release` requires the new version to be greater than every version already
in the changelog, writes the version heading and date, moves the entries under
it, empties `[Unreleased]`, and refreshes the link references at the bottom of
`CHANGELOG.md`. The repository URL defaults to
`https://github.com/iainchesworthlabs/virtual-serial-studio` and can be overridden with
`--repo-url`.

The same rules are enforced by the `Changelog fragments` CI job, which also
runs the tool's own unit tests (`tools/test_changelog.py`).
