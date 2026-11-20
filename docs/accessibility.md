---
template: marketing.html
title: Accessibility
title_tag: "Accessibility: Virtual Serial Studio"
description: "What Virtual Serial Studio and this site do for accessibility, and how each is checked: names, keyboard use, focus, target size, 200 percent text, contrast."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Accessibility</p>
<h1>Accessibility of the program and of this site</h1>
<p class="icl-lead">This page lists what the code and the checks in the repository establish. It does not claim a result from testing with a particular assistive technology.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>The program</h2>
<p>The rules are written for every widget, and a test walks the whole window to hold them.</p>
<ul class="vss-ticks">
<li>Every control has an accessible name and role.</li>
<li>Every control can be reached from the keyboard, and shows a visible focus ring.</li>
<li>Every target is at least 24 by 24 pixels.</li>
<li>Every dialog works at 200 percent text, and the same self-test runs again at 200 percent scale.</li>
<li>Both themes, light and dark, are held to the same rules. The theme follows the system, or you can choose one.</li>
<li>Colour is never the only signal. States are printed as text in the tables and the inspector, and the diagram labels its links.</li>
<li>The diagram has a list view of its links, under <strong>View</strong> at the Advanced level, for screen readers and for people who prefer a list.</li>
<li><code>Ctrl+K</code> opens a command palette over every action, and the Help menu lists the keyboard shortcuts.</li>
<li>Dialogs can be used from the keyboard, check what you typed when you accept, and keep it when a check fails.</li>
<li>Hebrew, Yiddish and Arabic switch the whole layout to right to left. The translations are machine translations that no speaker has reviewed.</li>
</ul>
</div>
<div>
<h2>How the program is checked</h2>
<ul class="vss-ticks">
<li><code>vss_gui --selftest</code> walks the main window and every dialog in both themes. It fails on a focusable control with no name, a target under 24 by 24, a control that the keyboard cannot reach, and clipped text. It runs as two tests, one at the normal scale and one at 200 percent.</li>
<li>A script, <code>tools/check_contrast.py</code>, computes WCAG 2.2 contrast for every pair of colours that carries meaning, and CI runs it. Text needs 4.5 to 1, and borders and the focus ring need 3 to 1.</li>
<li>The window follows the system's reduced-motion setting on Windows, and the environment variables <code>VSS_REDUCE_MOTION=1</code> and <code>VSS_HIGH_CONTRAST=1</code> turn on reduced motion and the system palette.</li>
</ul>
<p>Qt maps its accessibility information to UI Automation on Windows, AT-SPI on Linux and NSAccessibility on macOS, so what a screen reader reads is the name, role and state that the controls carry. The self-test checks the names and roles. It is not a test with a screen reader.</p>
<ul class="vss-links"><li><a href="../guide/interface-design/">Interface design</a></li><li><a href="../products/studio/">Studio</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>This site</h2>
<ul class="vss-ticks">
<li>Every page has one <code>h1</code>, headings that do not skip a level, a title of its own, and the language set to Australian English.</li>
<li>Every picture has a description in its alternative text, and a width and a height so the page does not jump.</li>
<li>The first link on a page skips to the content.</li>
<li>The menus are native HTML elements that open with no script. The front pages run no script at all.</li>
<li>The page follows the browser's light or dark setting, and the text uses the system font.</li>
<li>A link goes to a file or a heading that exists.</li>
</ul>
</div>
<div>
<h2>How this site is checked</h2>
<ul class="vss-ticks">
<li><code>tools/ci/check_site_pages.py</code> reads every built page. It fails on a missing title, more than one <code>h1</code>, a skipped heading level, a picture with no alternative text, a front page with no language, a script, a style attribute, a link that goes nowhere and a front page that loads anything from another site.</li>
<li>The colours come from the company's palette, and the company's check, <code>check-palette.mjs</code>, runs over it in CI.</li>
</ul>
<p>The checks do not read the text of a page for clarity or hold a page to a standard by themselves.</p>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Tell us about a barrier</h2>
<p>If something in the program or on this site is hard to use, open an issue on GitHub and say what you were doing, what you used to do it, and what happened.</p>
</div>
<ul class="vss-links"><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/issues">GitHub issues</a></li><li><a href="../support/">Support</a></li></ul>
</div></section>
