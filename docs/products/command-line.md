---
template: marketing.html
title: Command line
title_tag: "Command line, vss_cli: Virtual Serial Studio"
description: vss_cli makes virtual ports, runs saved setups, records and replays traffic, answers like a device, and keeps a JUnit test report, all without a window.
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Products</a> / Command line</p>
<h1>The command line, vss_cli</h1>
<p class="icl-lead">Everything you can build in the Studio window you can run from a terminal or a script: make ports, run a saved setup, record and replay traffic, make a port answer like a device, and read a test report.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../guide/command-line/#command-line-equivalents">Command line in the guide</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>What it does</h2>
<p><code>vss_cli</code> has no window. It runs the same engine as Studio and reads the same JSON setup file. A command that keeps ports open runs until you press Ctrl-C or the program receives SIGTERM, or for <code>N</code> seconds with <code>--seconds=N</code>. It takes its ports down before it ends.</p>
</div>
<div class="vss-table"><table>
<caption>Ports and setups</caption>
<thead><tr><th scope="col">Command</th><th scope="col">What it does</th></tr></thead>
<tbody>
<tr><th scope="row"><code>devices</code></th><td>Lists the serial ports and com0com pairs on this computer. <code>--json</code> is for a script.</td></tr>
<tr><th scope="row"><code>virtual</code></th><td>Makes one virtual port and prints the path to open from another program. <code>--com=N</code> asks the Windows driver for <code>COM</code><var>N</var>.</td></tr>
<tr><th scope="row"><code>vport</code></th><td>Makes pseudo terminals with stable names on Linux and macOS. <code>--link=a:b</code> joins two of them like a null-modem cable.</td></tr>
<tr><th scope="row"><code>share</code></th><td>Opens one port and gives every listener its own virtual port.</td></tr>
<tr><th scope="row"><code>join</code></th><td>Merges several publishers into one serial port.</td></tr>
<tr><th scope="row"><code>bus</code></th><td>Puts ports on one shared bus, where each hears every other and none hears itself. <code>--virtual=N</code> adds virtual ports for other programs, and <code>--tap</code> adds a listen-only port.</td></tr>
<tr><th scope="row"><code>bridge</code></th><td>Joins two transports back to back, for example serial to tcp. A <code>-listen</code> address waits for other computers, and <code>?allow=</code> says which.</td></tr>
<tr><th scope="row"><code>loopback</code></th><td>A virtual port, a redirect and a monitor, with no hardware.</td></tr>
<tr><th scope="row"><code>run</code></th><td>Applies a saved setup and keeps it until you stop it. <code>--reconnect</code> opens a <code>tcp://</code> or <code>rfc2217://</code> client again after its connection drops.</td></tr>
</tbody></table></div>
<div class="vss-table"><table>
<caption>Traffic, devices and checks</caption>
<thead><tr><th scope="col">Command</th><th scope="col">What it does</th></tr></thead>
<tbody>
<tr><th scope="row"><code>capture</code></th><td>Records every byte that crosses the ports, with its time and port, as <code>.cap</code>, CSV or JSON Lines.</td></tr>
<tr><th scope="row"><code>play</code></th><td>Writes the records of a <code>.cap</code> or JSON Lines capture into one port at the times they were recorded. <code>--speed</code> and <code>--loop</code> change the pace.</td></tr>
<tr><th scope="row"><code>respond</code></th><td>Makes one port act as a device that answers requests, or as a master that polls, from a JSON script.</td></tr>
<tr><th scope="row"><code>bench</code></th><td>Measures a real transfer from one port to another.</td></tr>
<tr><th scope="row"><code>selftest</code></th><td>Checks the wiring of the program without touching hardware.</td></tr>
<tr><th scope="row"><code>permissions check</code></th><td>Says what the operating system may stop Studio doing, and the smallest step to fix it. It changes nothing.</td></tr>
<tr><th scope="row"><code>licence</code></th><td>Shows the edition and the licence files found on this computer. <code>licence import &lt;file&gt;</code> checks a file and copies it into your licence folder.</td></tr>
<tr><th scope="row"><code>debug</code></th><td>Asks a running Studio what its engine, ports and windows are doing. Studio must have debug access on, and the person at the computer decides for each program.</td></tr>
</tbody></table></div>
<div class="vss-table"><table>
<caption>Plugins, the agent and the gateway</caption>
<thead><tr><th scope="col">Command</th><th scope="col">What it does</th></tr></thead>
<tbody>
<tr><th scope="row"><code>plugin list</code>, <code>verify</code></th><td>Shows the plugins on this computer and checks a plugin folder the way Studio does before a run. Neither runs anything.</td></tr>
<tr><th scope="row"><code>plugin install</code>, <code>remove</code></th><td>Copies a checked plugin folder into your plugin folder, or removes an installed plugin.</td></tr>
<tr><th scope="row"><code>plugin run</code></th><td>Runs one installed plugin under the plugin host and prints each change of its state. See <a href="../plugins/">Plugins</a>.</td></tr>
<tr><th scope="row"><code>agent</code></th><td>Works with the ports of this user's <code>vss_agent</code>: status, ports, create-port, remove-port, connect, write, watch and stop.</td></tr>
<tr><th scope="row"><code>control</code></th><td>Talks to this user's <code>vss_gate</code>: ports, links, send, expect, watch, tokens, sessions and test reports. See <a href="../agent-gateway/">Agent, gateway and Python client</a>.</td></tr>
</tbody></table></div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Examples</h2>
<p>Record a port to a file for a minute, then play the file into another port at twice the speed.</p>
<pre class="vss-code"><code>vss_cli capture --uri=serial://COM5 --out run.jsonl --seconds=60
vss_cli play run.jsonl --uri=serial://COM6 --speed=2</code></pre>
<p>Run a saved setup for an hour. The setup file is the one Studio saves.</p>
<pre class="vss-code"><code>vss_cli run topology.json --seconds=3600</code></pre>
<p>Make a port answer from a script. The script has rules, "when these bytes arrive, send those bytes after N ms", and an optional poll list.</p>
<pre class="vss-code"><code>vss_cli respond device.json --uri=serial://COM7</code></pre>
</div>
<div>
<h2>Headless use</h2>
<ul class="vss-ticks">
<li>No window and no display are needed. <code>vss_cli selftest</code> and <code>vss_cli loopback</code> need no hardware.</li>
<li>A mistake in the command line is one line on standard error and exit code 2. A command for the agent or the gateway ends with exit code 3 when none is running.</li>
<li>A test script keeps a JUnit report with <code>vss_cli control run begin</code>, <code>expect</code>, <code>run record</code>, <code>run end</code> and <code>run report --out=smoke.xml</code>, as <a href="../../guide/control-from-a-script/">Controlling Studio from a script</a> shows. The gateway makes the report.</li>
<li><code>vss_cli --help</code> lists every option.</li>
</ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Getting it on the path</h2>
<p>Each package has <code>vss_cli</code> beside Studio. How it gets onto your path depends on the system.</p>
</div>
<ul class="vss-ticks">
<li>Windows: the installer has a page with a box, not ticked, that adds the program folder to the system path. A silent install adds it only with <code>/ADDTOPATH=1</code>.</li>
<li>Linux: the deb and rpm put a link in <code>/usr/bin</code>. The archive has <code>bin/vss_cli</code>.</li>
<li>macOS: the disk image holds <code>bin/vss_cli</code> beside the app. Copy it out of the mounted image to where you want it.</li>
</ul>
<ul class="vss-links"><li><a href="../../downloads/">Downloads</a></li><li><a href="../../guide/command-line/#command-line-equivalents">Command line equivalents</a></li><li><a href="../../guide/command-line/#recording-with-vss_cli-capture">Recording</a></li><li><a href="../../guide/command-line/#scripted-devices-with-vss_cli-respond">Scripted devices</a></li></ul>
</div></section>
