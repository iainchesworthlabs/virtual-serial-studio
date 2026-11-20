---
template: marketing.html
title: Watch and record traffic
title_tag: "Watch and record traffic: Virtual Serial Studio"
description: Watch the bytes that cross a serial port without disturbing it, record them with their times, and play a recording back into a port.
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Watch and record traffic</p>
<h1>Watch and record traffic</h1>
<p class="icl-lead">See what crossed a port and when, keep it, and play it back.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>When a program and a device disagree, you need to see the bytes each side sent, and when. A serial port gives you no way to look without a tap. A log kept by your program shows what the program believes happened.</p>

<h2>What you do</h2>
<ol>
<li>Make the ports the usual way. On a setup's page, choose <strong>Show me the data going through</strong>. At the Advanced level, choose <strong>Topology, Watch traffic</strong>, pick the port and press <strong>Start watching</strong>. Both only look. They never drive the device's control lines.</li>
<li>Read the <strong>Monitor</strong>. Each row has a <strong>Time</strong>, the <strong>Port</strong>, a <strong>Direction</strong> (RX, TX or DROP), the <strong>Bytes</strong> and a <strong>Preview</strong> you can show as <strong>ASCII</strong> or <strong>Hex</strong>. Filter by port or direction, search with Ctrl+F, and press <strong>Pause</strong> to stop the list while you read.</li>
<li>To keep it, choose <strong>Export capture</strong>. It saves every byte with its time, as a <code>.cap</code>, <code>.csv</code> or <code>.jsonl</code> file.</li>
<li>To play a recording back, use <code>vss_cli play</code>. It sends the recorded bytes into a port at the times they were recorded, once or in a loop, at a speed you set. A recorded master or device can then drive the other programs without the program that made the recording.</li>
</ol>

<h2>Example</h2>
<p>Record what the master on a saved setup sent, for a minute, then play it into another port at twice the speed:</p>
<pre class="vss-code"><code>vss_cli capture topology.json --out bus.cap --port master --seconds=60
vss_cli play bus.cap --uri=serial://COM6 --speed=2</code></pre>
<p>To record a real device, with no setup, give its address:</p>
<pre class="vss-code"><code>vss_cli capture --uri=serial://COM5 --out run.jsonl --seconds=60</code></pre>
<p>Every port in a capture is stamped by one clock, including a port whose other end is on another computer, so the times line up across ports.</p>

<h2>Things to know</h2>
<ul>
<li>The Monitor keeps the latest 10,000 rows, and its preview stops at 256 bytes. For a record that is never cut short, use <code>vss_cli capture</code> or <strong>Export capture</strong>.</li>
<li>Times are whole milliseconds. The order of the rows is the order Studio saw the bytes.</li>
<li>A <code>.cap</code> file holds one port, so use <code>--port</code> to pick it. <code>.jsonl</code> keeps the port names, and <code>vss_cli play</code> reads both. It does not read <code>.csv</code>.</li>
<li>Studio decodes a protocol only through a plugin that has a decoder. No official decoder exists. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/monitor-and-capture/">The Monitor and capture files</a></li>
<li><a href="../../guide/command-line/#recording-with-vss_cli-capture">Recording with vss_cli capture</a></li>
<li><a href="../../guide/command-line/#replaying-with-vss_cli-play">Replaying with vss_cli play</a></li>
<li><a href="../../guide/scenarios/">Scenario recipes</a></li>
</ul>
</div></div></section>
