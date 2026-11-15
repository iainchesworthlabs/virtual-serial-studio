# 06 Accessibility specification

Benchmark WCAG 2.2 AA. Qt maps `QAccessible` to UI Automation on Windows, AT-SPI on Linux and NSAccessibility on macOS, so correct names, roles and states in Qt are what screen readers read. Test with Narrator or NVDA, Orca, and VoiceOver.

## Rules
1. Every focusable widget has a non-empty accessible name. Icon-only buttons set `setAccessibleName("Copy COM7")` and a tooltip with the same text. Fields use `QLabel::setBuddy` or an explicit name.
2. Custom widgets expose role, name and state: StatusChip (text, role static text), Switch (role CheckBox or Switch with checked state), SegmentedControl (radio group), PickCard (radio button labelled by the card text), Stepper ("Step 2 of 3, Programs"), CodeBlock ("Address", value as description), NoteBanner ("Warning: ..." prefix for warn and danger).
3. State is never colour alone (text plus shape plus colour).
4. Focus: 2px `focus` ring just inside the edge of every focusable control, drawn by `FocusRingStyle` while the keyboard is in use (a click hides it until the next key press; it stays through window activation and a dialog opening). A focused control is never covered by a dock, toast or popup. Dialogs take focus on the first field (or the safest button for confirmations) and return it to the invoking control on close.
5. Keyboard reaches everything. No traps. Escape closes dialogs, popups and the palette. Enter runs the primary action. Standard shortcuts only (see `03-information-architecture.md`).
6. Targets are at least 24 by 24 px. Prefer 28 to 34.
7. Nothing requires dragging. Topology nodes move with arrow keys. See `screens/03-topology-view.md`.
8. Live regions: announce state changes politely ("terminal: program connected", "Copied", toasts). Never announce traffic or counters unless the user turns that on.
9. Text can scale to 200 percent without clipping. Use layouts, minimum sizes from font metrics, and word wrap.
10. Respect OS settings: colour scheme, high contrast (use the system palette and drop custom colour overrides, keeping text and shape cues), text size, display scale, reduced motion (zero durations).
11. Errors name the operation, the cause and the next step, and are associated with the field (accessible description) as well as shown.

## Focus order
Menu bar, toolbar (including the search field), Ports dock, Topology, Inspector, Send, Monitor, status bar. In dialogs: top to bottom through fields, then the footer with the primary action last. Use `setTabOrder` explicitly where Qt's default creation order would be wrong.

## Accessible names for key controls
| Control | Name and value | Keyboard |
|---|---|---|
| Port row | "terminal, COM7, waiting for program" | Up/Down, Enter inspects, Delete removes |
| Copy button | "Copy COM7", announces "Copied" | Space or Enter |
| Topology | view: "Topology", description is a generated summary | Tab, Enter, arrows, Delete |
| Link | "Split link, device to 3 ports, two-way" | Enter selects, Delete removes |
| Switch | "DTR, switch, off" | Space |
| Monitor row | "12:04:31.208, device, received 38 bytes, <preview>" | Up/Down, Ctrl+F finds |
| Status chip | its text | none |

## Automated checks to add to `vss_gui --selftest` (offscreen)
- Instantiate every dialog and the main window in light and dark, at 100 and 200 percent scale, and walk the widget tree: fail on any focusable widget with an empty accessible name; fail on any button smaller than 24 by 24; fail if the tab order does not reach every focusable widget exactly once.
- Run `tools/check_contrast.py` in CI.
- Fail on hard-coded colour literals outside `Theme`.
- Save a `grab()` PNG of each dialog to `build/gui-shots/` as a CI artifact for human review (do not pixel-compare).
