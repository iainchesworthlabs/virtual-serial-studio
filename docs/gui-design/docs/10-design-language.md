# 10 Design language

This file sets the visual language for every screen. It replaces the look in the HTML mockups under `reference/mockups`.
Those mockups still define structure, strings and behaviour. Where they differ from this file on colour, shape, spacing
or component styling, this file wins. Values live in `tokens/tokens.json` and `apps/vss_gui/src/theme/resources/`.
Nothing outside `Theme` names a colour, font family or font size.

## The idea in one paragraph

The window is a tinted ground. Every region sits on it as a rounded surface with a gap around it, so space and tone
separate things instead of lines. Each surface has a clear order of importance: one title, at most one primary action,
quiet secondary actions, and content. Controls are fills, not outlines. Colour is kept for meaning: selection, state, RX
and TX. The language works with the system font and plain Qt widgets.

## Rules

1. **Panes float on the ground.** A pane is `panel` with a 1 px `line` border, radius `pane` (12), and an 8 px gap of
   `ground` around it. Do not separate regions with full-width rules.
2. **Pane header, not window chrome.** A pane header is 44 px. It holds the title (`heading`, 14/600), an optional count
   badge, then that pane's actions on the right as small secondary or ghost buttons. Panes have no float or close
   buttons. Those move to View, which also has Reset layout.
3. **Three button weights.** Primary is `accent` fill with `onAccent` text and no border. Secondary is `fill` with
   `ink` text and no border, and `fillHover` on hover. Ghost is `ink2` text with no fill, and `hover` on hover.
   Destructive is `badSoft` fill with `bad` text. A surface has at most one primary. Buttons are 32 px, or 28 px in
   pane headers and tool rows.
4. **Fields are the only outlined controls.** A field is `panel` with a 1 px `line2` border and radius `control` (8).
   On focus the border is 2 px `accent`.
5. **Switches for live settings.** A setting that takes effect at once (Hex, Follow, Add CRLF) is a `Switch`. A
   checkbox is for a choice confirmed later in a form.
6. **Segmented controls are a track and a raised thumb.** The track is `fill`, and the selected segment is `panel` with a
   hairline. The selected segment is never an accent fill.
7. **Chips are tinted capsules with no border.** A chip has a dot shape (filled, hollow or cross), text and a tint:
   `okSoft`/`ok`, `amberSoft`/`amber`, `badSoft`/`bad`, `accentSoft`/`accentText`, or `fill`/`ink2` for neutral.
8. **Objects have kind tiles.** A port or device row starts with a 32 px tile (radius 9) with the kind's icon. Virtual
   and pair tiles use `accentSoft`/`accentText`. Fixture tiles use `violetSoft`/`violet`. Real device tiles use
   `okSoft`/`ok`.
9. **Lists and tables breathe.** No grid lines and no zebra rows. Headers are `group` style (12/600, upper case, `ink3`).
   Rows highlight with `hover`. Selection is `accentSoft` with radius `inner` (8), inset 6 px from the pane edge.
   Times, bytes and addresses use the mono font.
10. **Group labels and grouped sections.** Inside a pane or dialog, related rows sit in a `sunken` group with radius
    10 and a `line` border, and `line` dividers between rows. A `group` label sits above it.
11. **Dialogs.** Padding is 24. The title (`title`, 20/600) has a one-line `ink2` subtitle under it. Choices are cards
    (radius 12, `line2` border; when selected, 2 px `accent` border on `accentSoft`). The result sentence is a note.
    The footer is right-aligned with no band or divider line, inset by the same 24, and the primary button is last.
    A list of checks or statuses (Setup check, "Why is nothing arriving?") is one group with a divider between rows,
    and each row starts with a kind tile in its state's tint. Figures inside a group (a benchmark's Sent, Received,
    Lost) sit on `raised` tiles: `panel`, radius 9, no border.
12. **Notes.** A note is a tinted block (radius 10) with an icon and no border: `accentSoft` for info, `amberSoft` for
    warning, `okSoft` for success, `badSoft` for danger.
13. **Act on what is selected.** An action sits next to the thing it acts on. Do not add a second picker for something
    already selected elsewhere.
14. **Keep the accessibility rules.** Text 4.5:1, non-text 3:1, 2 px focus ring with 2 px offset in `focus`, targets
    of at least 24 px, layouts that survive 200 percent text, and state that is never shown by colour alone.

## Colour roles added or changed

| Role | Light | Dark | Use |
|---|---|---|---|
| ground | #ECEEF1 | #14161A | Window background behind panes |
| panel | #FFFFFF | #22252B | Pane, dialog, card and field surface |
| sunken | #F4F5F7 | #1A1D22 | Grouped sections, canvas, tracks |
| fill | #EEF0F3 | #2D3139 | Secondary button, segment track, neutral chip |
| fillHover | #E4E7EB | #363B44 | Secondary button hover |
| hover | #F3F4F6 | #2A2E35 | Row and ghost button hover |
| pressed | #E4E7EB | #3A3F49 | Pressed state |
| line | #E2E5E9 | #343944 | Pane borders and dividers (decorative) |
| line2 | #CDD2D9 | #454A55 | Field and card borders |
| accent | #2160E0 | #2F6AE6 | Primary fill, selection border, checked controls |
| accentText | #1D56C9 | #9BBCF7 | Accent text and links on `panel` |
| violet, violetSoft | #6941C6, #EDE9FB | #B7A8F0, #2B2644 | TX tags and fixture tiles |

`tokens.json` has the full set and `tools/check_contrast.py` checks the pairs.

## Type, shape and space

| Token | Value |
|---|---|
| display | 28/600: wizard headlines |
| title | 20/600: dialog titles |
| heading | 14/600: pane titles |
| body | 13/400 |
| caption | 12/400 in `ink3` |
| group | 12/600, upper case, letter spacing 6 percent, in `ink3` |
| data | 12 mono |
| radius | control 8, inner 8, group 10, tile 9, pane 12, card 12, dialog 12, chip full |
| size | button 32, buttonSmall 28, input 32, paneHeader 44, header 48 |
| space | 4, 8, 12, 16, 24, 32, 48. Pane gap 8, dialog padding 24, group gap 16 |

## Shared building blocks

These widgets live in `apps/vss_gui/src/widgets/`. Use them rather than styling by hand.

- `QPushButton` with property `role` set to `primary`, `secondary`, `ghost` or `danger`.
- `Switch`, `SegmentedControl`, `PickCard`, `PickRow`, `StatusChip`, `DirectionBadge`, `NoteBanner` and
  `ResultSentence`, as before, restyled.
- `PaneHeader`: title, count and trailing actions for a pane.
- `KindTile`: the 32 px icon tile for a port kind.
- `ActionTile`: a large choice or job button with a kind tile, a title and a description on a card. It can be checkable.
- `DialogFrame`: title, subtitle, scrolling body and footer for every dialog and wizard page.
- `QFrame` with property `group` set to `true` for a grouped section, and `QLabel` with property `group` set to `true`
  for a group label.
