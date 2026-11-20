---
template: marketing.html
title: Products
title_tag: "Products: Virtual Serial Studio"
description: "What is in Virtual Serial Studio: the Studio application, the command line, the agent and gateway, plugins and drivers, and what is in the packages today."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Products</p>
<h1>What is in the box</h1>
<p class="icl-lead">Studio is the application. Around it are a command line, a per-user agent with a control gateway and a Python client, a plugin host, and drivers for virtual ports. Each card says what the part does and whether it is in the packages today.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<h2 class="icl-sr">The parts</h2>
<ul class="icl-grid vss-grid-3">
<li><article class="icl-card"><p class="vss-kicker">Desktop application</p><h3><a href="studio/">Studio</a></h3><p>Makes virtual serial ports, shares a real device with several programs, links ports, and shows the traffic on them. It has a Simple level of guided jobs and an Advanced level with a diagram.</p><p class="vss-card__status"><span class="vss-chip vss-chip--ok">In the packages</span></p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Command line</p><h3><a href="command-line/">vss_cli</a></h3><p>Does the same jobs without a window: make ports, run a saved setup, record and replay traffic, make a port answer like a device, and check a plugin.</p><p class="vss-card__status"><span class="vss-chip vss-chip--ok">In the packages</span></p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Automation</p><h3><a href="agent-gateway/">Agent, gateway and Python client</a></h3><p><code>vss_agent</code> keeps ports running for one user, <code>vss_gate</code> lets a test script control them on this computer, and <code>vss_control</code> is the Python client.</p><p class="vss-card__status"><span class="vss-chip vss-chip--src">Built from source only</span></p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Extensions</p><h3><a href="plugins/">Plugins</a></h3><p>Studio and <code>vss_cli</code> can run a plugin as a program of its own, apart from Studio, after you have decided to trust it. There are no official plugins yet.</p><p class="vss-card__status"><span class="vss-chip vss-chip--ok">Plugin host in the packages</span> <span class="vss-chip vss-chip--plan">Official plugins: Planned</span></p></article></li>
<li><article class="icl-card"><p class="vss-kicker">Virtual ports</p><h3><a href="drivers/">Drivers</a></h3><p>On Windows a driver makes the <code>COM</code> ports that other programs open. On Linux and macOS a virtual port is a pseudo terminal and needs no driver.</p><p class="vss-card__status"><span class="vss-chip vss-chip--ok">Windows driver in the packages</span> <span class="vss-chip vss-chip--src">Test-signed</span> <span class="vss-chip vss-chip--plan">Microsoft-signed: Planned</span></p></article></li>
</ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>How the parts fit</h2>
<p>Studio and <code>vss_cli</code> run the same engine and read the same JSON setup file, so a setup you build in the window can be run from a script. <code>vss_agent</code> runs the engine for a user and keeps its ports when the program that asked for them has gone.</p>
<p>Today the agent makes loopback ports that only its own clients can use. Ports you make in the Studio window belong to Studio, so the gateway does not see them.</p>
<ul class="vss-links"><li><a href="../guide/">The guide</a></li><li><a href="../developers/architecture/">Architecture</a></li></ul>
</div>
<ol class="vss-stack" aria-label="How the parts are layered, from the programs at the top to the operating system at the bottom">
<li><span class="vss-stack__name">Programs</span><ul class="vss-stack__row"><li>Studio<small>window</small></li><li>vss_cli<small>command line</small></li><li>vss_agent<small>per-user agent</small></li><li>vss_gate<small>control gateway</small></li><li>vss_control<small>Python client</small></li></ul></li>
<li><span class="vss-stack__name">Engine</span><ul class="vss-stack__row"><li>Ports and links<small>split, join, bus, monitor</small></li><li>Capture and replay</li><li>Mock fixtures and scripted responses</li><li>Plugin host<small>in Studio and vss_cli</small></li></ul></li>
<li><span class="vss-stack__name">Ports</span><ul class="vss-stack__row"><li>Virtual ports</li><li>Real serial devices</li><li>tcp, udp and rfc2217<small>client and listener</small></li><li>Loopback</li></ul></li>
<li><span class="vss-stack__name">System</span><ul class="vss-stack__row"><li>Windows driver and helper service</li><li>Pseudo terminals<small>Linux and macOS</small></li></ul></li>
</ol>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>What is in which package</h2>
<p>Every release has the files for each system. Download them from the Downloads page.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">Part</th><th scope="col">Windows installer and zip</th><th scope="col">macOS disk image and archive</th><th scope="col">Linux archive, deb and rpm</th></tr></thead>
<tbody>
<tr><th scope="row">Studio (<code>vss_gui</code>)</th><td>Yes</td><td>Yes</td><td>Yes</td></tr>
<tr><th scope="row">Command line (<code>vss_cli</code>)</th><td>Yes</td><td>Yes</td><td>Yes</td></tr>
<tr><th scope="row">Windows driver and helper service</th><td>Yes, test-signed</td><td>Not needed</td><td>Not needed</td></tr>
<tr><th scope="row"><code>vss_agent</code>, <code>vss_gate</code></th><td>Not yet</td><td>Not yet</td><td>Not yet</td></tr>
<tr><th scope="row">Python client</th><td>Not yet</td><td>Not yet</td><td>Not yet</td></tr>
</tbody></table></div>
<ul class="vss-links"><li><a href="../downloads/">Downloads</a></li><li><a href="../platforms/">Platforms</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Planned</h2>
<p>These are not available. Nothing on this site describes them as built.</p>
</div>
<ul class="vss-ticks">
<li>Official plugins. The plugin host is built, and none exist. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>A Windows driver signed by Microsoft, so that Windows accepts it without the owner's certificate authority. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Controlling one computer from another, which needs TLS. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Client libraries for languages other than Python. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Packages that contain the agent and the gateway. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Response timing rules and fault injection that a test or <code>vss_cli</code> can reach. The library exists and has no way in yet. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>
</div></section>
