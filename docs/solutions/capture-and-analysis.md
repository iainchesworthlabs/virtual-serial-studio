---
template: marketing.html
title: Capture and analysis
title_tag: "Capture and analysis: Virtual Serial Studio"
description: Watch the traffic on serial ports without disturbing it, record every byte with its time as .cap, CSV or JSON Lines, and play a recording back into a port.
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Capture and analysis</p>
<h1>Capture and analysis</h1>
<p class="icl-lead">See what crossed a port, keep it with its exact time, and play it back.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>When a program and a device disagree, you need to see the bytes each side sent and when. A serial port gives you no way to look without a tap, and a log kept by the program shows what the program believes happened.</p>

<h2>How Virtual Serial Studio does it</h2>
<ul>
<li><strong>Watch traffic</strong> (<strong>Topology, Watch traffic</strong>) makes a read-only copy of a port. It never drives modem lines, so it does not disturb the device. At the Simple level, <strong>Show me the data going through</strong> on a setup's page shows the traffic without making a watcher port.</li>
<li>The <strong>Monitor</strong> shows a row for each transfer with its <strong>Time</strong>, <strong>Port</strong>, <strong>Direction</strong> (RX, TX or DROP), <strong>Bytes</strong> and <strong>Preview</strong>. You can show the preview as <strong>ASCII</strong> or <strong>Hex</strong>, filter by port and direction, search the payload with Ctrl+F, pause, and <strong>Export</strong> the rows as text.</li>
<li><strong>Export capture</strong> and <code>vss_cli capture</code> write every byte, with the time the engine read when the bytes moved. That is one clock for every port, including a port whose other end is on another computer. The other computer's own clock is never used.</li>
<li><code>vss_cli play</code> writes the records of a <code>.cap</code> or <code>.jsonl</code> file into a port at their recorded times, at a speed you set, once or in a loop.</li>
<li>A drop is shown too. A DROP row appears each time a link's dropped count grows, with the count and not the bytes.</li>
</ul>
<div class="vss-table"><table>
<thead><tr><th scope="col">Format</th><th scope="col">What it holds</th></tr></thead>
<tbody>
<tr><th scope="row"><code>.cap</code></th><td>Text, one line for each transfer, with the offset in milliseconds, a direction and the bytes. It has no port names, so capture one port for each file. <code>play</code> reads it.</td></tr>
<tr><th scope="row"><code>.csv</code></th><td>A header row <code>time_utc,offset_ms,port,direction,bytes,hex</code>, then one row for each transfer. <code>play</code> does not read it.</td></tr>
<tr><th scope="row"><code>.jsonl</code></th><td>One JSON object for each transfer, with the port and direction, and a first line with the start time. <code>play</code> reads it.</td></tr>
</tbody></table></div>
<p>The direction is the port's own. <strong>R</strong> is bytes the port received, from the program, device or remote client attached to it. <strong>W</strong> is bytes written to the port, which the hub sent on. One transmission over a link is R on the sender and W on each receiver, so capture the port you care about to keep one row for each message.</p>

<h2>Example</h2>
<pre class="vss-code"><code>vss_cli capture topology.json --out bus.cap --port master
vss_cli capture --uri=serial://COM5 --out run.jsonl --seconds=60
vss_cli play bus.cap --uri=serial://COM6 --speed=2</code></pre>
<p>A line of a <code>.cap</code> file looks like this:</p>
<pre class="vss-code"><code>[         142] R 0x10|0x02|0x50</code></pre>
<p>Each line is flushed as it is written, so a file is complete up to the last transfer even if the process is killed. <code>--format</code> chooses the format, or it comes from the file name. The file is readable only by its owner where the system has such a setting.</p>

<h2>What is not covered</h2>
<ul>
<li>The Monitor keeps the latest 10,000 rows and no more than 32 MiB of bytes, and its <strong>Preview</strong> stops at 256 bytes. For a record that does not stop there, use <code>vss_cli capture</code>.</li>
<li>Offsets are whole milliseconds, so two transfers in the same millisecond have the same offset. The order of the lines is the order the engine delivered them.</li>
<li>Studio decodes a protocol only through a plugin that has a decoder, with <strong>Decode as...</strong> on a port. No official decoder exists. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>The support bundle never holds the bytes your ports sent and received.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/monitor-and-capture/#the-monitor-and-the-send-panel">The Monitor and the Send panel</a></li>
<li><a href="../../guide/monitor-and-capture/#capture-files">Capture files</a></li>
<li><a href="../../guide/command-line/#replaying-with-vss_cli-play">Replaying with vss_cli play</a></li>
<li><a href="../../products/studio/">Studio</a></li>
</ul>
</div></div></section>
