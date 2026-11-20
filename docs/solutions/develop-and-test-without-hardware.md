---
template: marketing.html
title: Develop and test without hardware
title_tag: "Develop and test without hardware: Virtual Serial Studio"
description: "Give your program a serial port before the device exists, make the other end answer like the device, and slow the line down or break it to see how your program copes."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Develop and test without hardware</p>
<h1>Develop and test without hardware</h1>
<p class="icl-lead">Write and run serial software when the device is not on your desk.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>Your program opens a serial port. The device it talks to does not exist yet, or it is in another building, or the whole team shares one. With no port your program will not start, and nobody can run it in an automatic test.</p>

<h2>What you do</h2>
<ol>
<li>Open Studio. Under <strong>My program needs a serial port</strong>, choose <strong>Give my program a serial port</strong>. To join two programs to each other, like a null-modem cable, choose <strong>Connect two programs to each other</strong>.</li>
<li>Press <strong>Create</strong>. Studio shows the name to open, such as <code>COM7</code> on Windows or <code>/dev/pts/3</code> on Linux. Copy it.</li>
<li>Open that port in your program. To play the device by hand, choose <strong>I'll type to it from this window</strong> when you create the port, then type in <strong>Type to</strong>. To get your own bytes back, choose <strong>Send everything straight back</strong>.</li>
</ol>

<h2>Make the other end answer like the device</h2>
<p>Link two ports. Open one in your program, and let <code>vss_cli respond</code> take the other. You write the replies in a small file: when these bytes arrive, send those bytes, after a delay you choose. This one answers after 150 ms, give or take 10 ms.</p>
<pre class="vss-code"><code>{ "version": 1,
  "rules": [
    { "name": "status", "match": "02 30", "reply": "02 31 41 03",
      "delay_ms": 150, "jitter_ms": 10 } ] }</code></pre>
<pre class="vss-code"><code>vss_cli respond device.json --uri=serial://COM7</code></pre>
<p>The same file can poll like a master. <code>vss_cli play</code> sends a recording back at the times it was made (see <a href="../capture-and-analysis/">Watch and record traffic</a>).</p>

<h2>Make the line slow, or make it fail</h2>
<p>At the Advanced level, right-click a link and choose <strong>Settings...</strong>.</p>
<ul>
<li><strong>Delay</strong> and <strong>Jitter</strong> hold back what the link carries, so you can try a reply that is early, late or on the edge.</li>
<li><strong>Wire timing</strong> makes a long write take as long as it would on a real wire, for protocols that need gaps of silence, such as Modbus RTU.</li>
<li>Add a <strong>Mock fixture</strong>, then choose <strong>Open line matrix and faults</strong> in the Inspector. <strong>Inject</strong> raises a parity error, framing error, overrun, break, hang-up or disconnect. The fixture then stops, as a real device would.</li>
</ul>
<p>Injecting a fault from a script or a test is not built. <span class="vss-chip vss-chip--plan">Planned</span></p>

<h2>Things to know</h2>
<ul>
<li>A virtual port has no wire. A program that depends on the timing of a real device needs a delay or wire timing on the link.</li>
<li>On Windows, a virtual <code>COM</code> port needs the Studio driver. It is test-signed, so Windows accepts it only where the certificate authority of Iain Chesworth Labs is trusted. See <a href="../../products/drivers/">Drivers</a>.</li>
<li>On Linux and macOS the port is a pseudo terminal. A program that lists the serial ports on the computer does not see it, so give that program the path.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/getting-started/#quick-start">Quick start</a></li>
<li><a href="../../guide/adding-ports/">Adding ports</a></li>
<li><a href="../../guide/timing-and-control-lines/">Timing and control lines</a></li>
<li><a href="../../guide/command-line/#scripted-devices-with-vss_cli-respond">Scripted devices</a></li>
</ul>
</div></div></section>
