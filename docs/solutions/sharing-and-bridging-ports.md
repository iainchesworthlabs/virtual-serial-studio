---
template: marketing.html
title: Share a device and put it on the network
title_tag: "Share a device and put it on the network: Virtual Serial Studio"
description: "Let several programs use one serial device at the same time, take turns sending to it, or use it from another computer over the network."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Share a device and put it on the network</p>
<h1>Share a device and put it on the network</h1>
<p class="icl-lead">One device, several programs. Or a device on one computer, used from another.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A serial device can normally be open in one program at a time. You may want a terminal, a test tool and a logger on the same device. Or the device is plugged into one computer and the program that needs it runs on another.</p>

<h2>What you do</h2>
<p>Studio opens the device once and gives each program its own port. Your programs open those ports and never touch the device.</p>
<ol>
<li>Choose <strong>Share my device with several programs</strong>. At the Advanced level use <strong>Topology, Share a device</strong>.</li>
<li>Choose the device, then add a row for each program, up to 32. A program name such as PuTTY is only a label.</li>
<li>Press <strong>Create</strong>. Studio lists the port each program should open, such as <code>COM7</code> and <code>COM8</code>. Open one in each program.</li>
</ol>
<p>Everything the device sends reaches every program, and whatever a program writes goes to the device. To let one program send at a time, choose <strong>Let programs take turns sending to a device</strong>. Each program gets a <strong>Let this one send</strong> button, and the others only listen.</p>

<h2>Use the device from another computer</h2>
<p>Choose <strong>Let other computers use my device</strong>. Studio makes a network port, 4001 unless you change it, and the other computer connects to it. At the Advanced level, <strong>Ports, Network endpoint</strong> lets you choose TCP, UDP or RFC 2217, and lets Studio connect out to a server instead of waiting. RFC 2217 makes the device look like a serial port to a client such as pySerial's <code>rfc2217://</code>.</p>

<h2>Example</h2>
<p>Share a device with three programs for a minute:</p>
<pre class="vss-code"><code>vss_cli share serial://COM3 --listeners=3 --seconds=60</code></pre>
<p>Let the computers on one subnet use the device over RFC 2217. A computer outside the list is disconnected:</p>
<pre class="vss-code"><code>vss_cli bridge serial://COM3 rfc2217-listen://192.168.1.10:7001?allow=192.168.1.0/24</code></pre>

<h2>Keep it safe</h2>
<ul>
<li>Nothing a network port carries is encrypted, and an RFC 2217 port has no password. <code>?allow=</code> lists the computers that may connect. It keeps strangers on your network away and nothing more.</li>
<li>If the other program is on the same computer, listen on <code>127.0.0.1</code>. For another computer, use an SSH tunnel or a VPN. Do not listen on <code>0.0.0.0</code> on a network you do not control.</li>
<li>TLS for a network port is not built. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>Things to know</h2>
<ul>
<li>A real device can be open in only one program. If another program already has it, attaching it fails.</li>
<li><strong>Put several on one line</strong> makes a shared bus, where every program hears the others, like stations on one RS-485 line. A program on another computer can be one of them. The guide has the steps.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/sharing-a-device/">Sharing a device</a></li>
<li><a href="../../guide/network/">Network addresses and keeping it safe</a></li>
<li><a href="../../guide/connecting-ports/">Links between ports</a></li>
<li><a href="../../guide/scenarios/">Scenario recipes</a></li>
</ul>
</div></div></section>
