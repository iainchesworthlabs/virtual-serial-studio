---
template: marketing.html
title: Fault injection and timing
title_tag: "Fault injection and timing: Virtual Serial Studio"
description: "Slow a link with delay and jitter, pace it like a wire, raise a break, a parity error or a hang-up, and spoil bytes on a bus, to see how your software copes."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Fault injection and timing</p>
<h1>Fault injection and timing</h1>
<p class="icl-lead">Make a link slow, make a line drop, and make a device fail on purpose, to see what your software does.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>Serial software works on a bench and fails in the field. The wire is slower than the bench, a reply comes late, two stations talk at once, a cable is pulled, a line shows a framing error. A test needs to cause each of these when it chooses.</p>

<h2>How Virtual Serial Studio does it</h2>
<ul>
<li><strong>Delay and jitter on a link.</strong> A link can hold what it carries for <code>delay_ms</code>, from 0 to 60000, and vary each chunk by up to <code>jitter_ms</code> either side. Jitter cannot exceed the delay, and order is kept. On a two-way link each direction waits the full delay. In the window, right-click the link and choose <strong>Settings...</strong>, then <strong>Delay</strong> and <strong>Jitter</strong>.</li>
<li><strong>Wire timing on a link.</strong> With <strong>Wire timing</strong> on, each character takes its real bits, a start bit, 5 to 8 data bits, a parity bit if there is one and the stop bits, and a long write is spread over the time it would take. Some protocols depend on that time: Modbus RTU ends a frame with 3.5 characters of silence. A break holds back what follows it.</li>
<li><strong>Collisions on a bus.</strong> A bus can be a half-duplex line with a line rate and a turn-around time. It reports two stations on the line together, or an answer that came too soon, and with <code>"collisions": "garble"</code> every byte of both messages arrives inverted, so a checksum fails and a master that retries does.</li>
<li><strong>Faults from a mock fixture.</strong> At the Advanced level, <strong>Open line matrix and faults</strong> has <strong>Inject</strong> for a <strong>Parity error</strong>, <strong>Framing error</strong>, <strong>Overrun</strong>, <strong>Break</strong>, <strong>Hang-up</strong> and <strong>Disconnect</strong>. Each is reported as an error event on the port, and the fixture then stops and shows Failed, as a real device would. On Windows the same buttons work on a port of the Studio driver and raise the fault at the program that has the port open.</li>
<li><strong>Control lines.</strong> The Inspector switches DTR and RTS and has a <strong>Break</strong> button. The line matrix says which output drives which input on the other port.</li>
<li><strong>Late and varying replies.</strong> <code>vss_cli respond</code> sends a reply after <code>delay_ms</code> plus or minus <code>jitter_ms</code>, and a seed makes the jitter repeat. See <a href="../device-emulation/">Device emulation</a>.</li>
</ul>

<h2>Example</h2>
<p>Every byte takes between 180 and 200 ms to reach each other member of the bus. This is a link in a setup file:</p>
<pre class="vss-code"><code>{ "mode": "bus", "members": ["master", "device-1", "device-2"],
  "delay_ms": 190, "jitter_ms": 10 }</code></pre>
<p>Pace a two-way link at 115200 bit/s, 8N1:</p>
<pre class="vss-code"><code>{ "mode": "duplex", "sources": ["master"], "targets": ["device"],
  "wire_timing": { "bit_rate": 115200, "framing": "8N1" } }</code></pre>
<p>Make a bus a half-duplex line that spoils both messages when two stations send at once:</p>
<pre class="vss-code"><code>{ "mode": "bus", "members": ["master", "device-1", "device-2"],
  "half_duplex": { "bit_rate": 9600, "turnaround_ms": 2,
                   "collisions": "garble", "rts_gating": false } }</code></pre>

<h2>What is not covered</h2>
<ul>
<li>A delay is a timer in Studio. It is accurate to a few milliseconds, and a busy computer can make a byte late but never early. Wire timing is kept to about half a millisecond on Windows 10 (version 1803) and later.</li>
<li>Over RFC 2217 the gaps a client sees are not reliable, because TCP joins and splits what it sends.</li>
<li>A fault is injected by a person in the window. No test or <code>vss_cli</code> command injects one. The control gateway has a scope for it, <code>control.faults</code>, and it allows nothing yet.</li>
<li>A fixture stops after a fault, so you add another fixture to inject again.</li>
<li>Seeded random fault injection from a test. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Response timing rules that a test or <code>vss_cli</code> can reach. The library exists and has no way in yet. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/#delay-and-jitter-on-a-link">Delay and jitter on a link</a></li>
<li><a href="../../guide/#wire-timing-on-a-link">Wire timing on a link</a></li>
<li><a href="../../guide/#rs-485-is-half-duplex-and-a-bus-can-be">Half-duplex buses</a></li>
<li><a href="../../guide/#mock-fixture-advanced">Mock fixture</a></li>
</ul>
</div></div></section>
