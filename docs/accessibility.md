---
template: marketing.html
title: Accessibility
title_tag: "Accessibility: Virtual Serial Studio"
description: "What Virtual Serial Studio and this site do for accessibility: names for screen readers, keyboard use, focus, target size, 200 percent text, contrast and themes."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Accessibility</p>
<h1>Accessibility</h1>
<p class="icl-lead">What Studio and this site do so that more people can use them, and how that is checked. We have not tested with a particular screen reader, and this page does not claim a result from one.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>In Studio</h2>
<ul class="vss-ticks">
<li>Every control has a name and a role that a screen reader can read.</li>
<li>Every control can be reached from the keyboard, and shows where the focus is.</li>
<li>Every target is at least 24 by 24 pixels.</li>
<li>Every dialog works at 200 percent text.</li>
<li>Light and dark themes follow the same rules. Studio follows your system's theme, or you can choose one.</li>
<li>Colour is never the only signal. States are written as text in the tables and the inspector, and the diagram labels its links.</li>
<li>The diagram has a list view of its links, under <strong>View</strong> at the Advanced level, for screen readers and for people who prefer a list.</li>
<li><code>Ctrl+K</code> opens a command palette over every action, and the Help menu lists the keyboard shortcuts.</li>
<li>Dialogs work from the keyboard, check what you typed when you accept, and keep it when a check fails.</li>
<li>Studio follows your system's reduced-motion setting on Windows. To turn on reduced motion or the system's own colours on any system, set the environment variable <code>VSS_REDUCE_MOTION=1</code> or <code>VSS_HIGH_CONTRAST=1</code>.</li>
<li>Hebrew, Yiddish and Arabic switch the whole layout to right to left. Those translations are machine translations that no speaker has reviewed.</li>
</ul>
</div>
<div>
<h2>How it is checked</h2>
<ul class="vss-ticks">
<li>A test opens the main window and every dialog in both themes, and again at 200 percent. It fails the build on a control with no name, a target under 24 by 24 pixels, a control the keyboard cannot reach, or text that is cut off.</li>
<li>A script works out the contrast of every pair of colours that carries meaning, on every pull request. Text needs a ratio of at least 4.5 to 1, and borders and the focus ring need at least 3 to 1.</li>
<li>Studio passes its names, roles and states to the accessibility service of each system, so a screen reader reads what the controls carry. The test checks the names and roles. It is not a test with a screen reader.</li>
</ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>On this site</h2>
<ul class="vss-ticks">
<li>Every page has one main heading, headings in order, a title of its own and the language set to Australian English.</li>
<li>Every picture has a description in its alternative text.</li>
<li>The first link on a page skips to the content.</li>
<li>The menus open with no script, and the pages run no script at all.</li>
<li>The page follows your browser's light or dark setting, and the text uses your system's font.</li>
</ul>
</div>
<div>
<h2>How this site is checked</h2>
<ul class="vss-ticks">
<li>Every built page is checked for a title, one main heading, headings that do not skip a level, alternative text on pictures, a language and links that go somewhere.</li>
<li>The colours come from the company's palette, which is checked for contrast.</li>
</ul>
<p>These checks do not read a page for clarity.</p>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Tell us about a barrier</h2>
<p>If something in Studio or on this site is hard to use, open an issue on GitHub and say what you were doing, what you used to do it, and what happened.</p>
</div>
<ul class="vss-links"><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/issues">GitHub issues</a></li><li><a href="../support/">Support</a></li></ul>
</div></section>
