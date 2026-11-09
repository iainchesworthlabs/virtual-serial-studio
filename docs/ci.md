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
| Draft pull request | A draft is open or updated | The static jobs only: the shared policy checks (branch name, commit and pull request text, file text, action pinning, REUSE, vcpkg baseline), changelog fragments, design tokens and the workflow analysis |
| Ready pull request | A pull request is ready for review | Linux GCC build and tests, AddressSanitizer with UBSan, the fuzz corpus replay; ThreadSanitizer when threading code changed |
| Merge queue | An entry is queued | The same on the merged tree, plus Windows MSVC, ThreadSanitizer, the Qt GUI build with its self-test and journey tests, the performance floors when library code changed, macOS when a change can affect it, and the Windows driver when the driver changed |
| Push to `develop` | After the merge | The legs the queue does not run: Linux Clang, macOS, the coverage gates, the Tracy profile of the end-to-end path and the Windows driver; one run at a time with at most one waiting |
| `workflow_dispatch` | On request | Every job (`gh workflow run ci.yml --ref <branch>`) |

A change to documentation, the changelog or licence text skips the builds. The
sanitizer jobs run the GUI suites too; see [Testing](testing.md#sanitizers).
Boost is fetched by `tools/fetch_boost.py`, so no job depends on a system Boost.

The branch protection ruleset requires two checks, `CI Status` and
`Scan dependency diff`. `CI Status` fails when any job the plan asked for failed,
was cancelled or was skipped.

## Other workflows

| Workflow | Runs | Does |
|---|---|---|
| `fuzz.yml` | Push to `develop`, every night | Fuzzes each target for 60 seconds, or 10 minutes at night; see [Fuzzing](fuzzing.md) |
| `release.yml` | A `v*` tag, every Monday, on request | Builds the packages and publishes them for a tag; see [Releasing](releasing.md) |
| `docs.yml` | A pull request that changes the docs, a push to `develop` or `main` | Checks the presets and badges against the repository and builds this site with `mkdocs build --strict`; from `main` it also publishes the site to GitHub Pages. The push to `develop` gives the branch a status of its own |
| `cache-hygiene.yml` | After a CI run on `develop`, a closed pull request, on request | Deletes Actions caches that nothing can restore any more, to keep the repository within its cache quota |
| `dependabot-auto-merge.yml` | Dependabot pull requests | Turns on auto-merge for minor and patch updates of actions |

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

The sanitizers and fuzzing do not upload to code scanning. Their findings are
failed tests and reproducers, not alerts that can be dismissed.
Include-what-you-use, OSV-Scanner and OpenSSF Scorecard are not run yet.
