# WP9: accessibility and QA pass
Model: deepseek-v4-pro (thinking on). Branch: `gui/wp9-qa`. Depends on WP3 to WP8 merged.

## Read first
`AGENTS.md`, `docs/06-accessibility.md`, `docs/09-qa-and-definition-of-done.md`, `docs/01-principles-and-standards.md`, `docs/gui-design/design-deviations.md`.

## Task
1. Build the automated checks in `docs/06-accessibility.md` into `vss_gui --selftest`: for every dialog and the main window, in light and dark, at 100 and 200 percent scale and the minimum window, walk the widget tree and fail on empty accessible names, targets under 24 by 24, unreachable or duplicated tab stops, hard-coded colour literals, and text that is clipped (compare `sizeHint` and `minimumSizeHint` with the actual size at 200 percent text).
2. Fix every failure you find, in the owning code, with the smallest change. List each fix in the PR.
3. Keyboard-only walkthrough of every story in `docs/00-product-brief.md` using a scripted `QTest` flow where feasible. Fix traps and lost focus.
4. High contrast and reduced motion behaviour per `docs/06-accessibility.md`.
5. Write offscreen `grab()` PNGs of every dialog and the main window (light, dark, compact) to `build/gui-shots/` as CI artifacts for human review.
6. Update `docs/user-guide/` and `docs/ux.md` to match what shipped, and remove the "Not yet in the interface" items that now exist. Summarise all deviations in `design-deviations.md` with owner and reason.
7. Final regression: full `ctest`, selftest, contrast tool, changelog check, warnings as errors on every available compiler.

## Constraints
Do not add features. Do not weaken a check to pass it.

## Acceptance
Selftest fails if any rule regresses. A human can review the PNG set. Docs match behaviour.
