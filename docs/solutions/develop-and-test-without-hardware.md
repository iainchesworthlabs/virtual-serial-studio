---
template: marketing.html
title: Develop and test without hardware
title_tag: "Develop and test without hardware: Virtual Serial Studio"
description: "Give a program a virtual serial port, or join two programs with a null-modem link, to write and test serial software before the device exists."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Develop and test without hardware</p>
<h1>Develop and test without hardware</h1>
<p class="icl-lead">Write and run serial software when the device is not on your desk.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A program that talks to a serial device needs a port to open. The device may not exist yet, may be in another building, or may be one unit shared by a whole team. Without a port the program cannot start, and nobody can run it in an automatic test.</p>

<h2>How Virtual Serial Studio does it</h2>
<ul>
<li><strong>Give my program a serial port</strong> makes one virtual port and tells you the name to open: <code>COM7</code> on Windows, <code>/dev/pts/3</code> on Linux. Choose <strong>I'll type to it from this window</strong> to play the device by hand, or <strong>Send everything straight back</strong> to make a mock fixture that echoes.</li>
<li><strong>Connect two programs to each other</strong> makes two ports linked both ways, like a null-modem cable, with DTR wired to DSR and RTS to CTS. What one program sends, the other receives.</li>
<li>A <strong>mock fixture</strong> stands in for a device. It can echo, or send a text line, a fixed pattern, a counter or random bytes at a set rate. <a href="../device-emulation/">Device emulation</a> goes further.</li>
<li>The same ports can be made from a terminal with <code>vss_cli</code>, so a script or a build can make them.</li>
</ul>

<h2>Example</h2>
<p>On Linux or macOS, make two ports with stable names and join them. Run it in one terminal and leave it running.</p>
<pre class="vss-code"><code>vss_cli vport --name=ttyVSS0,ttyVSS1 --link=ttyVSS0:ttyVSS1</code></pre>
<p>The links are in <code>$XDG_RUNTIME_DIR/vss</code> on Linux and in <code>/tmp/vss</code> on macOS. Open one in your program and the other in a terminal program such as picocom, or in a second copy of your program.</p>
<p>On Windows, with the driver installed, ask for the number your program expects:</p>
<pre class="vss-code"><code>vss_cli virtual --kind=windows-driver --com=3</code></pre>
<p>The command prints the port and runs until you press Ctrl-C. <code>vss_cli loopback --seconds=30</code> shows the wiring with no hardware, and <code>vss_cli selftest</code> checks the program itself.</p>

<h2>What is not covered</h2>
<ul>
<li>A virtual port has no wire. It does not behave like a particular adapter, and a program that depends on the timing of a real device needs a delay or wire timing on the link. See <a href="../fault-injection-and-timing/">Fault injection and timing</a>.</li>
<li>On Windows, a virtual <code>COM</code> port needs the driver. Without it, Studio falls back to com0com if it is there, and otherwise to a loopback port that only Studio can open.</li>
<li>The kernel's pseudo terminal on Linux has no modem lines, so DTR and RTS do not reach a program. A program that lists ports does not list a pseudo terminal. It needs the path.</li>
<li>On macOS a program that lists ports through IOKit does not see the port.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/getting-started/#quick-start">Quick start</a></li>
<li><a href="../../guide/adding-ports/#adding-a-port">Adding a port</a></li>
<li><a href="../../products/drivers/">Drivers and virtual ports</a></li>
<li><a href="../../products/studio/">Studio</a></li>
</ul>
</div></div></section>
