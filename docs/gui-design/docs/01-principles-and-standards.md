# 01 Principles and standards

Benchmark: **WCAG 2.2 AA** (WCAG 3.0 is still a working draft). Also ISO 9241-110 dialogue principles and the platform's own conventions.

## Nine principles and what they mean in code
1. **Recognition over recall.** Pick ports and links from lists and cards. Addresses and commands are previews with a Copy button, never required input.
2. **Depth is revealed, never removed.** Launcher, workspace, power tools. Advanced adds controls to the same window. Do not build a second window or layout for Advanced.
3. **Plain language first.** Use: two-way, one-way, split, join, monitor, program, port. Keep engine names (endpoint, link_mode, URI) out of visible text except in clearly labelled detail areas.
4. **Say what will happen.** Every creation dialog shows a result sentence (an info note) that updates as the form changes.
5. **Prevent, then explain.** Self-links, loops and too few targets are blocked before anything is created. A refusal says what to do instead. Validation never closes a dialog or clears input.
6. **State is never colour alone.** Every state has text, a shape (filled dot, hollow dot, cross) and colour. Direction badges say RX, TX or DROP.
7. **Fail visibly and locally.** Errors name the operation, the likely cause and the next step as buttons. Engine text goes behind Copy details. The setup is never discarded.
8. **Keyboard first.** Every action has a menu item with a standard shortcut. Ctrl+K lists every action, including hidden Advanced ones.
9. **Follow the platform.** System font and size, system colour scheme, native file dialogs, Ctrl on Windows and Linux and Cmd on macOS, OS high contrast and reduced motion.

## WCAG 2.2 AA checklist (applied to this desktop UI)
| Criterion | Rule here |
|---|---|
| 1.4.1 Use of colour | State uses text plus shape plus colour. |
| 1.4.3 Contrast (text) | 4.5:1 for text. Tokens are pre-checked by `tools/check_contrast.py`. |
| 1.4.11 Non-text contrast | Control borders (`control` token), switch tracks, focus ring and chip dots reach 3:1. Decorative dividers (`line`) do not need to. |
| 1.4.4 Resize text | Layouts survive 200 percent text size. Use layouts and size policies, not fixed pixel sizes. |
| 1.4.12 Text spacing | Do not clip text when line height or spacing increases. |
| 2.1.1 Keyboard | Every action works without a pointer. No keyboard traps (2.1.2). |
| 2.4.3 Focus order | Reading order: menu, toolbar, ports, topology, inspector, send, monitor, status bar. |
| 2.4.7 Focus visible | 2px `focus` ring just inside the control's outer edge, on every focusable control (see `02-design-tokens.md`, item 5). |
| 2.4.11 Focus not obscured | Docks, toasts and popups never cover the focused control. |
| 2.5.7 Dragging movements | Nothing requires dragging. Topology nodes move with arrow keys, and every link can be made in the Connect dialog. |
| 2.5.8 Target size | 24 by 24 px minimum for any pointer target. Prefer 28 to 34. |
| 3.3.1, 3.3.3 Errors | Name the field or operation, and suggest a fix. |
| 3.3.4 Error prevention | Confirm destructive actions, offer Undo. |
| 4.1.2 Name, role, value | Every control has an accessible name and role. Custom widgets implement `QAccessibleInterface` or use standard widgets with names. |

## Definition of done for any UI task
- Matches the spec in `docs/screens/` including exact strings, defaults and states.
- Uses only tokens. No literal colours, no literal font families, no literal font sizes outside `Theme`.
- Light and dark themes both work. Switching theme at runtime updates everything with no restart.
- Keyboard complete, focus ring visible, tab order matches the spec, accessible names set.
- Works at 100 and 200 percent display scale, and at 200 percent text size, at the 960 by 640 minimum window.
- Strings go through `tr()` and live in one place per screen.
- Offscreen self-test covers the new widgets (`QT_QPA_PLATFORM=offscreen vss_gui --selftest`).
- A `changelog.d/` fragment, SPDX header on every new file, `clang-format` clean, builds with warnings as errors.
