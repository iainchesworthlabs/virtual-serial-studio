---
template: marketing.html
title: Device emulation
title_tag: "Device emulation: Virtual Serial Studio"
description: "Make a port answer like a serial device, or poll like a master, from a JSON script with a reply delay and jitter, or replay what a device sent."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Device emulation</p>
<h1>Device emulation</h1>
<p class="icl-lead">Put a scripted device, or a scripted master, on the other end of a port.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A program that polls a device needs answers: the right bytes, at about the right time. A real device is not always to hand, and a test cannot make one reply late, or reply to some requests and ignore others.</p>

<h2>How Virtual Serial Studio does it</h2>
<ul>
<li><strong>Mock fixture</strong> (<strong>Ports, Mock fixture</strong>) is a scriptable port. In the Inspector, <strong>Echo received bytes</strong> sends every byte back. The <strong>Generator</strong> sends a <strong>Text line</strong>, a <strong>Fixed pattern</strong> in hex, a <strong>Counter</strong> or <strong>Random bytes</strong> at a rate you choose, until you press <strong>Stop generator</strong>.</li>
<li><code>vss_cli respond</code> makes one port act as a device from a JSON script. A rule says: when these bytes arrive, exactly or under a mask, send those bytes after <code>delay_ms</code>, give or take <code>jitter_ms</code>. The delay is counted from the moment the request arrived. A reply is written to the port as one piece.</li>
<li>The same script can make a port act as a master. A poll list sends timed steps once or in cycles.</li>
<li>A seed makes the jitter repeat from run to run, and a key the script does not know is an error that names it, so a misspelt <code>delay_ms</code> is found.</li>
<li><code>vss_cli play</code> writes a recorded <code>.cap</code> or JSON Lines capture into a port at the times it was recorded, so a recorded master or device can drive a bus without the program that made the recording. See <a href="../capture-and-analysis/">Capture and analysis</a>.</li>
</ul>

<h2>Example</h2>
<p>This script answers any request from <code>02 30</code> to <code>02 3f</code> with <code>02 31 41 03</code>, 150 ms later give or take 10 ms. It also polls with two requests every second.</p>
<pre class="vss-code"><code>{
  "version": 1,
  "seed": 7,
  "rules": [
    { "name": "status", "match": "02 30", "mask": "ff f0",
      "reply": "02 31 41 03", "delay_ms": 150, "jitter_ms": 10 }
  ],
  "poll": {
    "start_ms": 100, "interval_ms": 1000, "cycles": 0,
    "steps": [ { "send": "02 31 03" },
               { "send": "02 32 03", "at_ms": 250, "jitter_ms": 5 } ]
  }
}</code></pre>
<p>Run it on the port a program would open, or on a port of a saved setup:</p>
<pre class="vss-code"><code>vss_cli respond device.json --uri=serial://COM7
vss_cli respond device.json topology.json --port=device --seconds=60</code></pre>
<p>It runs until Ctrl-C or <code>--seconds</code>, and prints how many requests it matched and what it sent. Bytes are matched as a stream, so a request that arrives in two reads is found. When several rules could match, the one that starts first wins, and then the one listed first. A script can have rules, a poll list, or both.</p>

<h2>What is not covered</h2>
<ul>
<li>A script has no variables and no state, so a reply is the same bytes each time. It does not work out a checksum.</li>
<li>A delay is limited to one hour.</li>
<li>A script is not part of the setup file. A saved setup cannot hold one.</li>
<li>Response timing rules, a richer rule engine with timing in microseconds, exist as a library and have no way in from Studio or <code>vss_cli</code> yet. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/command-line/#scripted-devices-with-vss_cli-respond">Scripted devices in the guide</a></li>
<li><a href="../../guide/adding-ports/#mock-fixture-advanced">Mock fixture</a></li>
<li><a href="../../guide/scenarios/">Scenario recipes</a></li>
<li><a href="../../products/command-line/">Command line</a></li>
</ul>
</div></div></section>
