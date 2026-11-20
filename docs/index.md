---
# The home page of the product site (docs/plan/site.md, section 3). The sentence under the name is the one that docs/icl-app.json
# carries for the company site's tile (summary), and mkdocs.yml (site_description) repeats it.
template: marketing.html
title: Virtual Serial Studio
title_tag: "Virtual Serial Studio: virtual serial ports and a traffic monitor"
description: "Virtual serial ports, port sharing, a traffic monitor and test automation on one computer. For Windows, Linux and macOS, free under the GPL."
---
<section class="icl-hero icl-hero--app"><div class="icl-wrap"><div class="icl-hero__grid">
<div>
<h1>Virtual Serial Studio</h1>
<p class="icl-lead">Virtual serial ports, port sharing, a traffic monitor and test automation on one computer.</p>
<div class="icl-actions"><a class="icl-button" href="downloads/">Download</a><a class="icl-button icl-button--ghost" href="guide/">Read the guide</a></div>
<ul class="icl-tags"><li>Windows, Linux and macOS</li></ul>
</div>
<figure class="vss-hero-shot"><picture><source srcset="images/site/advanced-diagram-dark.png" media="(prefers-color-scheme: dark)"><img src="images/site/advanced-diagram-light.png" width="1280" height="800" alt="The Studio window at the Advanced level. A Ports list on the left shows console, logger and modem. The diagram shows console linked two-way to logger, with a modem fixture not yet linked. An Inspector and a Send panel are on the right, and the Monitor below lists RX and TX rows of an AT command."></picture></figure>
</div></div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>What you can do with it</h2>
<p>Each card goes to a page that shows how Studio does the job, and what the job does not cover.</p>
</div>
<ul class="icl-grid vss-grid-3">
<li><article class="icl-card"><p class="vss-kicker">Virtual ports</p><h3><a href="solutions/develop-and-test-without-hardware/">Give a program a serial port</a></h3><p>Make a port for your program, or two ports joined like a null-modem cable, before the device exists or while it is somewhere else.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Port sharing</p><h3><a href="solutions/sharing-and-bridging-ports/">Share and bridge ports</a></h3><p>Give one real device to several programs, merge several senders into one device, share a bus, or put a port on the network.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Traffic monitor</p><h3><a href="solutions/capture-and-analysis/">Watch and record traffic</a></h3><p>See every byte on every port, in text or hex, record it with its time, export it, and play a recording back.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Device emulation</p><h3><a href="solutions/develop-and-test-without-hardware/">Answer like a device</a></h3><p>Make a port reply from a script, or poll like a master, and replay what a device sent, with a delay you choose.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Test automation</p><h3><a href="solutions/test-automation-and-ci/">Run tests with no window</a></h3><p>Run a saved setup from a terminal or a CI job, drive ports from a script, wait for a reply and keep a test report.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Test benches</p><h3><a href="solutions/test-automation-and-ci/">Build a bench and keep it</a></h3><p>Lay out your ports and links in a diagram, add delay and faults, and save the whole bench as a file you can open again.</p></article></li>
</ul>
<ul class="vss-links"><li><a href="products/studio/">Studio</a></li><li><a href="solutions/features/">Everything it can do</a></li><li><a href="solutions/">What people use it for</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Who it is for</h2>
<p>One program, used two ways.</p>
</div>
<div class="vss-two">
<div>
<h3>An engineer at a bench</h3>
<p>You have a device on a serial port and a program that needs it, or a program that needs a port with nothing on the other end. Studio opens the device once and gives each program its own port, shows you what each one sent, and records it. This is the Community edition, and it is free.</p>
<ul class="vss-links"><li><a href="products/studio/">Studio</a></li><li><a href="pricing/">Community edition</a></li></ul>
</div>
<div>
<h3>A test lab</h3>
<p>You run the same setup every time. A saved setup comes back the same way on every run, and Studio records traffic and plays it back. Test automation shows how a test or a CI job uses it.</p>
<ul class="vss-links"><li><a href="solutions/test-automation-and-ci/">Test automation</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Editions</h2>
<p>Community is the whole application, free. Pro and Enterprise add a licence file, support and terms agreed with you, and are quoted on request.</p>
</div>
<ul class="vss-plans">
<li class="vss-plan"><h3>Community</h3><p class="vss-plan__price">Free<small>GPL-3.0-or-later</small></p><p class="vss-plan__for">Studio and the source, with help on GitHub.</p><a class="icl-button icl-button--ghost" href="downloads/">Download</a></li>
<li class="vss-plan"><h3>Pro</h3><p class="vss-plan__price">Price on request</p><p class="vss-plan__for">For labs that depend on Studio: a licence file and support.</p><a class="icl-button icl-button--ghost" href="pricing/">See Pro</a></li>
<li class="vss-plan"><h3>Enterprise</h3><p class="vss-plan__price">Price on request</p><p class="vss-plan__for">For organisations: terms written for you, with seats, computers and a support agreement.</p><a class="icl-button icl-button--ghost" href="pricing/">See Enterprise</a></li>
</ul>
<ul class="vss-links"><li><a href="pricing/">Compare the editions</a></li><li><a href="licensing/">Licensing</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Quality and security</h2>
<p>What is done to keep Studio reliable, and what you can check for yourself.</p>
</div>
<div class="vss-two">
<ul class="vss-ticks">
<li>Studio is built and tested on Windows, Linux and macOS every night.</li>
<li>The tests also run under tools that catch memory mistakes and data races, and the code that reads files and network data is fuzzed.</li>
<li>Vulnerabilities are reported privately through GitHub.</li>
</ul>
<ul class="vss-ticks">
<li>Every release file is signed, and every release has a checksum list and a software bill of materials. You can check a download yourself.</li>
<li>Studio sends nothing about you or your computer. There is no telemetry, no account and no usage data.</li>
</ul>
</div>
<ul class="vss-links"><li><a href="quality/">Quality and trust</a></li><li><a href="security/">Security</a></li><li><a href="downloads/#verify">Verify a download</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Windows, Linux and macOS</h2>
<p>Studio runs on Windows, Linux and macOS. <a href="downloads/">Downloads</a> has the files for each system and says what each one needs.</p>
</div>
</div></section>

<section class="icl-section icl-section--tint icl-cta"><div class="icl-wrap">
<h2>Try it on one computer</h2>
<p class="icl-lead">Make two linked ports and watch the bytes go through, with no hardware.</p>
<div class="icl-actions"><a class="icl-button" href="downloads/">Download</a><a href="guide/">Read the guide</a><a href="support/#talk-to-us">Talk to us</a></div>
</div></section>
