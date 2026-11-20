---
template: marketing.html
title: What Studio can do
title_tag: "What Studio can do: Virtual Serial Studio"
description: "Every feature of Virtual Serial Studio in one list: virtual ports, sharing, timing, fault injection, recording and playback, scripted devices and automated tests, and what is not built yet."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / What Studio can do</p>
<h1>What Studio can do</h1>
<p class="icl-lead">Everything that is built today, in one list. Each group links to the guide for the steps.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>Make ports</h2>
<ul>
<li>A virtual serial port for your program, or two ports joined like a null-modem cable. <a href="../develop-and-test-without-hardware/">Develop and test without hardware</a></li>
<li>On Windows, a <code>COM</code> port with the number you choose, made by the Studio driver. On Linux and macOS, a pseudo terminal with a stable name.</li>
<li>A real serial device, opened once by Studio, so your programs open Studio's ports and never the device.</li>
</ul>

<h2>Share and connect</h2>
<ul>
<li>One device shared by up to 32 programs, with every program hearing the device. <a href="../sharing-and-bridging-ports/">Share a device and put it on the network</a></li>
<li>Programs that take turns sending to a device, or several senders joined into one receiver.</li>
<li>Several programs on one shared line, like stations on an RS-485 bus, with a listener that only hears.</li>
<li>A device put on the network as <code>tcp</code>, <code>udp</code> or RFC 2217, with a list of the computers that may connect. Nothing a network port carries is encrypted.</li>
</ul>

<h2>Make the line slow</h2>
<ul>
<li><strong>Delay</strong> and <strong>Jitter</strong> on any link, up to 60 seconds, to try a reply that is early, late or on the edge.</li>
<li><strong>Wire timing</strong>, so a long write takes as long as it would on a real wire, for protocols that depend on gaps such as Modbus RTU.</li>
<li>A shared line with a line rate that reports when two stations talk at once, and can spoil both messages so a checksum fails.</li>
</ul>

<h2>Make it fail</h2>
<ul>
<li>A <strong>Mock fixture</strong> that raises a parity error, framing error, overrun, break, hang-up or disconnect from <strong>Open line matrix and faults</strong>. On Windows the same buttons raise the fault at the program that has a driver port open.</li>
<li>DTR and RTS switches, a <strong>Break</strong> button, and chips for CTS, DSR, DCD and RI, with a line matrix that says which output drives which input.</li>
<li>Injecting a fault from a script or a test. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>Answer like a device</h2>
<ul>
<li>A mock fixture that echoes, or sends a text line, a pattern, a counter or random bytes at a rate you choose.</li>
<li><code>vss_cli respond</code>: a script of rules that say what to send when certain bytes arrive, after a delay and jitter, and a poll list that makes a port act as a master.</li>
<li>Response timing rules that a test can reach. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>Watch, record and play back</h2>
<ul>
<li>The <strong>Monitor</strong>: every transfer with its time, port, direction and bytes, as ASCII or hex, with filters, search and pause. <a href="../capture-and-analysis/">Watch and record traffic</a></li>
<li><strong>Watch traffic</strong>, a read-only copy of a port that never drives its control lines.</li>
<li><strong>Export capture</strong> and <code>vss_cli capture</code>: every byte with its time, as <code>.cap</code>, <code>.csv</code> or <code>.jsonl</code>.</li>
<li><code>vss_cli play</code>: a recording sent back into a port at the times it was made, at a speed you set, once or in a loop.</li>
<li>A decoder for a protocol, from an official plugin. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>Automate</h2>
<ul>
<li>Setups saved as files, and run with no window by <code>vss_cli run</code>. <a href="../test-automation-and-ci/">Automate tests in a lab</a></li>
<li><strong>Export as command</strong>, which shows the command that builds the setup you have open.</li>
<li>A test script that makes ports, sends bytes, waits for a reply and keeps a JUnit report, in Python or from the command line. <span class="vss-chip vss-chip--src">Built from source only</span></li>
<li>Controlling another computer, TLS for network ports, and client libraries other than Python. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/">The guide</a></li>
<li><a href="../../guide/limits/">What this build cannot do</a></li>
<li><a href="../../products/">Products</a></li>
</ul>
</div></div></section>
