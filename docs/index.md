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
<h2>What it does today</h2>
<p>Studio is the application. <code>vss_cli</code> does the same jobs with no window. Each card goes to a page that says how, and what the job does not cover.</p>
</div>
<ul class="icl-grid vss-grid-3">
<li><article class="icl-card"><p class="vss-kicker">Virtual ports</p><h3><a href="solutions/develop-and-test-without-hardware/">Give a program a serial port</a></h3><p>Make a port for your program, or two ports joined like a null-modem cable, before the device exists or while it is somewhere else.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Port sharing</p><h3><a href="solutions/sharing-and-bridging-ports/">Share and bridge ports</a></h3><p>Give one real device to several programs, merge several senders into one device, share a bus, or put a port on the network.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Traffic monitor</p><h3><a href="solutions/capture-and-analysis/">Watch and record traffic</a></h3><p>See every byte on every port, in text or hex, record it with its time, export it, and play a recording back.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Device emulation</p><h3><a href="solutions/device-emulation/">Answer like a device</a></h3><p>Make a port reply from a script, or poll like a master, and replay what a device sent, with a delay you choose.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Test automation</p><h3><a href="solutions/test-automation-and-ci/">Run tests with no window</a></h3><p>Run a saved setup from a terminal or a CI job, drive ports from a script, wait for a reply and keep a JUnit report.</p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Test benches</p><h3><a href="solutions/test-benches/">Build a bench and keep it</a></h3><p>Lay out ports, links, modem control lines and a multi-drop bus in a diagram, add delay and faults, and save it as a file.</p></article></li>
</ul>
<ul class="vss-links"><li><a href="products/studio/">Studio</a></li><li><a href="solutions/">All seven solutions</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Who it is for</h2>
<p>One program, two ways to use it.</p>
</div>
<div class="vss-two">
<div>
<h3>An engineer at a bench</h3>
<p>You have a device on a serial port and a program that needs it, or a program that needs a port with nothing on the other end. Studio opens the device once and gives each program its own port, shows you what each one sent, and records it. This is the Community edition, and it is free.</p>
<ul class="vss-links"><li><a href="products/studio/">Studio</a></li><li><a href="pricing/">Community edition</a></li></ul>
</div>
<div>
<h3>A test lab that drives Studio from a harness</h3>
<p>You run the same setup every time, from a script, with no window. <code>vss_cli</code> runs a saved setup and records and replays traffic. A Python client lets a test make ports, send bytes, wait for a reply and hand back a JUnit report. The agent, the gateway and the client are built from source today. <span class="vss-chip vss-chip--src">Built from source only</span></p>
<ul class="vss-links"><li><a href="guide/command-line/">Command line</a></li><li><a href="products/agent-gateway/">Agent and gateway</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Editions</h2>
<p>Community is the whole application, free. Pro and Enterprise add a licence file, support and terms agreed with you, and are quoted on request.</p>
</div>
<ul class="vss-plans">
<li class="vss-plan"><h3>Community</h3><p class="vss-plan__price">Free<small>GPL-3.0-or-later</small></p><p class="vss-plan__for">Studio, the command line and the source, with help on GitHub.</p><a class="icl-button icl-button--ghost" href="downloads/">Download</a></li>
<li class="vss-plan"><h3>Pro</h3><p class="vss-plan__price">Price on request</p><p class="vss-plan__for">For test labs: a licence file and support.</p><a class="icl-button icl-button--ghost" href="pricing/">See Pro</a></li>
<li class="vss-plan"><h3>Enterprise</h3><p class="vss-plan__price">Price on request</p><p class="vss-plan__for">Negotiated terms, seats and computers, and a support agreement.</p><a class="icl-button icl-button--ghost" href="pricing/">See Enterprise</a></li>
</ul>
<ul class="vss-links"><li><a href="pricing/">Compare the editions</a></li><li><a href="licensing/">Licensing</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Quality and security</h2>
<p>What the repository shows, and where to find it.</p>
</div>
<div class="vss-two">
<ul class="vss-ticks">
<li>The libraries are built with AddressSanitizer, UBSan and ThreadSanitizer at <code>-O1</code>, and the tests run under them.</li>
<li>Ten libFuzzer harnesses read the files and network bytes that come from outside: setups, endpoint addresses, plugin manifests, licences and the update feed among them.</li>
<li>Coverage of the libraries has a gate of 90 percent of statements and 80 percent of branches.</li>
</ul>
<ul class="vss-ticks">
<li>Every release file has a GPG signature, and <code>SHA512SUMS</code> lists every file. Each release has an SPDX bill of materials, and each package has a provenance attestation.</li>
<li>Studio sends nothing about you or your computer. There is no telemetry, no account and no usage data.</li>
<li>Vulnerabilities are reported privately through GitHub.</li>
</ul>
</div>
<ul class="vss-links"><li><a href="quality/">Quality and trust</a></li><li><a href="security/">Security</a></li><li><a href="downloads/#verify">Verify a download</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Windows, Linux and macOS</h2>
<p>Studio and <code>vss_cli</code> run on Windows, Linux and macOS. <a href="downloads/">Downloads</a> has the files for each system and says what each one needs.</p>
</div>
</div></section>

<section class="icl-section icl-section--tint icl-cta"><div class="icl-wrap">
<h2>Try it on one computer</h2>
<p class="icl-lead">Make two linked ports and watch the bytes go through, with no hardware.</p>
<div class="icl-actions"><a class="icl-button" href="downloads/">Download</a><a href="guide/">Read the guide</a><a href="support/#talk-to-us">Talk to us</a></div>
</div></section>
