---
template: marketing.html
title: Solutions
title_tag: "Solutions: Virtual Serial Studio"
description: "Seven jobs people use Virtual Serial Studio for, from developing without hardware to fault injection, with the features each uses and what it does not cover."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Solutions</p>
<h1>What people use it for</h1>
<p class="icl-lead">Each page names a job, says how Studio does it using only what is built, shows an example, and says what it does not cover.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<h2 class="icl-sr">The seven solutions</h2>
<ul class="icl-grid vss-grid-3">
<li><article class="icl-card"><h3><a href="develop-and-test-without-hardware/">Develop and test without hardware</a></h3><p>Give a program a serial port, or two programs a null-modem cable, before the device exists or while it is somewhere else.</p></article></li>
<li><article class="icl-card"><h3><a href="device-emulation/">Device emulation</a></h3><p>Make a port answer like a device from a script, or replay what a device sent, with a delay and jitter you choose.</p></article></li>
<li><article class="icl-card"><h3><a href="test-automation-and-ci/">Test automation and CI</a></h3><p>Run a setup with no window, drive ports from a script, wait for a reply, and keep a JUnit report.</p></article></li>
<li><article class="icl-card"><h3><a href="capture-and-analysis/">Capture and analysis</a></h3><p>Watch the traffic on a port, record every byte with its time, and play a recording back.</p></article></li>
<li><article class="icl-card"><h3><a href="sharing-and-bridging-ports/">Sharing and bridging ports</a></h3><p>Give one device to several programs, merge several senders, share a bus, or put a port on the network.</p></article></li>
<li><article class="icl-card"><h3><a href="test-benches/">Test benches</a></h3><p>Build a bench of ports, links, control lines and a multi-drop bus, and keep it as a file.</p></article></li>
<li><article class="icl-card"><h3><a href="fault-injection-and-timing/">Fault injection and timing</a></h3><p>Slow a link, pace it at the speed of a wire, drop a line, break, or spoil bytes on a bus.</p></article></li>
</ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Before you rely on one</h2>
<ul class="vss-ticks">
<li>Windows needs the driver for virtual <code>COM</code> ports. It is test-signed, so Windows accepts it only where the certificate authority of Iain Chesworth Labs is installed.</li>
<li>The agent, the gateway and the Python client are built from source and are not in the packages.</li>
<li>Pages here say what Studio does with <code>tcp</code>, <code>udp</code> and <code>rfc2217</code>. None of them encrypts anything.</li>
</ul>
</div>
<div>
<h2>Where the detail is</h2>
<ul class="vss-links">
<li><a href="../guide/scenarios/">Scenario recipes</a></li>
<li><a href="../guide/">The guide</a></li>
<li><a href="../products/studio/">Studio</a></li>
<li><a href="../downloads/">Downloads</a></li>
</ul>
</div>
</div>
</div></section>
