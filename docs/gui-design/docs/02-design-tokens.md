# 02 Design tokens and Qt implementation

Authoritative values: `tokens/tokens.json`. This page explains how to use them in Qt.

## Colour roles (light / dark)
`10-design-language.md` says how the roles are used; this table lists them.

| Token | Role | Light | Dark |
|---|---|---|---|
| ground | Window background behind the panes | #ECEEF1 | #14161A |
| panel | Panes, dialogs, cards, fields | #FFFFFF | #22252B |
| sunken | Grouped sections, canvas, read-only fields | #F4F5F7 | #1A1D22 |
| fill / fillHover | Secondary buttons, segment tracks, neutral chips, menu rows under the pointer | #EEF0F3 / #E4E7EB | #2D3139 / #363B44 |
| hover / pressed | Row and ghost button under the pointer, and while pressed | #F3F4F6 / #E4E7EB | #2A2E35 / #3A3F49 |
| line | Pane borders and dividers (decorative) | #E2E5E9 | #343944 |
| line2 | Field and card borders, scrollbar thumbs | #CDD2D9 | #454A55 |
| control | Check box and radio edges, field hover (3:1 rule) | #808996 | #8A909B |
| ink / ink2 / ink3 | Text: primary, secondary, caption | #14171C / #4A515C / #646C79 | #E6E8EC / #B4B9C2 / #979DA8 |
| accent / accentHover | Primary fill, checked controls, selection edge; primary hover | #2160E0 / #1A52C4 | #2F6AE6 / #2558C9 |
| accentStrong / accentText | Accent text, links and accent icons | #1A52C4 / #1D56C9 | #9BBCF7 / #9BBCF7 |
| accentSoft / accentLine | Selection fill and its edge, RX tag | #E8EFFD / #B9CDF6 | #232F4A / #3D5A9C |
| violet / violetSoft | TX tag, fixture tiles and chips | #6941C6 / #EDE9FB | #B7A8F0 / #2B2644 |
| ok, okSoft, okLine | Healthy | #17744B ... | #5BC694 ... |
| amber, amberSoft, amberLine | Attention | #9A5806 ... | #E8B260 ... |
| bad, badSoft, badLine | Failure | #C4321F ... | #F28B7E ... |
| focus | Focus ring | #2160E0 | #9BBCF7 |
| onAccent, onDanger | Label on accent and danger fills | #FFFFFF | #FFFFFF, #2A0905 |

Run `python3 tools/check_contrast.py` after any colour change. It must report `FAILURES: 0`.

## Typography
- Never bundle fonts and never set a font family. Use `QFontDatabase::systemFont(QFontDatabase::GeneralFont)` and `QFontDatabase::systemFont(QFontDatabase::FixedFont)` for data, paths, addresses and payloads.
- Windows: keep Segoe UI for text. Qt 6 draws Segoe UI Variable at the wrong weight through DirectWrite (every regular label comes out semi-bold), so do not select it. Use Consolas for the fixed font when present (the Qt default is Courier New). Both ship with the OS, nothing is bundled. See design-deviations.
- Request the unhinted outline (`QFont::PreferNoHinting`) and `QFont::PreferAntialias`, and set `Qt::HighDpiScaleFactorRoundingPolicy::PassThrough` before the `QApplication` is built, so 125 and 150 percent displays are not rounded and resampled.
- Every style lands on a whole number of logical pixels (body 13, caption, label and group 12, large and heading 14, subtitle 16, title 20, display 28, port name 44). A fractional size such as 11.08 px is the cause of soft, uneven text. Convert to points with the screen's logical DPI so Qt gets the same whole pixel size back.
- Implement the scale as ratios of the system font's size (never below the 13px design body) so OS text size is honoured: caption and label 0.923, body 1.0, body large 1.077, subtitle 1.231, title 1.538, display 2.0, port name 3.385 (fixed font, weight 500), data 0.923 (fixed font). Weights: labels, headings, group labels, titles and display 600, button 500. Group labels are upper case with 6 percent extra tracking, set in `Theme::font(text_style::group)`.
- Minimum text is 12px equivalent, except key caps and uppercase badges at 11px.

## Size, spacing, shape
Use the `size`, `space`, `radius` and `layout` blocks in tokens.json. Radii: control 8, inner 8, group 10 (grouped sections, menus, notes), tile 9, pane 12, card 12, dialog 12, chip 11. The style sheet template reads them as `%radius.control%` and `%space.2%`, so change the token, not a widget. Spacing unit is 4px (4, 8, 12, 16, 24, 32, 48). Buttons are 32px high (28 small and in pane headers), inputs 32, toolbar buttons 32, pane headers 44, the header row 48, tab bars 32, status bar 32, kind tiles 32. Dialogs are padded 24 with 16 between groups. Panes sit 8 apart on the ground. Any pointer target is at least 24 by 24. Depth comes from tone: panes are `panel` on `ground` with a 1px `line` hairline. Only dialogs, menus and toasts cast shadows, and those come from the platform.

## Required Qt architecture
```
Theme (singleton, QObject)
  - tokens(): current colour set (light or dark)
  - colour(Role), font(TextStyle), metric(Size), spacing(Space)
  - styleSheet(): QString   // renders tokens/style.qss.in for the current theme
  - palette(): QPalette     // maps tokens to QPalette roles
  - signal themeChanged()
IconProvider
  - icon(name, ColourRole, logicalSize) -> QIcon   // loads :/icons/<name>.svg, replaces currentColor, caches by (name, size, dpr, colour)
  - clears its cache on Theme::themeChanged
FocusRingStyle : QProxyStyle    // paints the 2px ring with 2px offset for keyboard focus
```
1. Base style: `Fusion`, so the palette and QSS render the same on every platform. Keep native menu bar, native file dialogs and native message-box behaviour.
2. QPalette mapping: Window=ground, Base=panel, AlternateBase=sunken, WindowText and Text=ink, PlaceholderText=ink3, Highlight=accent (selected text uses onAccent), HighlightedText=onAccent, Link=accentStrong, ToolTipBase=toastBg, ToolTipText=toastText, Button=panel, ButtonText=ink, Disabled group text=disabledText.
3. Follow the OS colour scheme with `QGuiApplication::styleHints()->colorScheme()` and its change signal. Settings offers Follow the system, Light and Dark. When forced, apply your own palette (the colour scheme override API only exists in newer Qt, so do not depend on it).
4. Re-polish on theme change: `qApp->setPalette(...)`, `qApp->setStyleSheet(...)`, then rebuild icons and repaint custom widgets.
5. Focus ring: QSS cannot do it. In `FocusRingStyle::drawPrimitive`, for `PE_FrameFocusRect` draw a 2px `focus` rounded rectangle expanded by 2px, and only when the widget has keyboard focus (`Qt::TabFocusReason`, `BacktabFocusReason`, `ShortcutFocusReason`). Never show the ring for mouse clicks on buttons.
6. Custom painted widgets (StatusChip, Switch, Stepper, Topology nodes) read colours from `Theme`, never from literals. Add a unit test that fails if a hex literal appears in `apps/vss_gui` outside `Theme`.
7. Dynamic properties drive QSS: `role` (primary, quiet, danger), `size` (small, icon), `invalid`, `note` (info, ok, warn, danger), `card`, `pick`, `selected`, `code`, `muted`.
8. High DPI: Qt 6 scales automatically. Use logical pixels, `devicePixelRatioF()` for icon pixmaps, and never round positions yourself.
9. Reduced motion: if the OS requests it use 0 ms durations. Otherwise 100 ms hover, 150 ms disclosure, 200 ms dialogs, ease-out. Live counters never animate.

## Icons
`icons/*.svg` are 16px, 1.5 stroke, round caps, `currentColor`. Always paired with a text label, or a tooltip and accessible name when alone. Use Qt SVG (`Qt6::Svg`, approved) or hand-parse. `IconProvider` returns a `QIcon` backed by an icon engine that renders on demand at any size and device pixel ratio, so the icon stays sharp on every screen and at 200 percent text. An icon asked for by colour role follows the theme without being rebuilt, and its disabled mode is drawn in `disabledText`. Every action, button and field the specs give an icon shows one (mock-ups under `reference/mockups`).
