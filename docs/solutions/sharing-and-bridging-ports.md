---
template: marketing.html
title: Sharing and bridging ports
title_tag: "Sharing and bridging ports: Virtual Serial Studio"
description: "Give one serial device to several programs, merge senders into one port, share a bus, or bridge a port to the network with tcp, udp or rfc2217."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Sharing and bridging ports</p>
<h1>Sharing and bridging ports</h1>
<p class="icl-lead">One device, several programs. Several senders, one receiver. A port on another computer.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A serial device can normally be open in one program at a time. You may want a terminal, a test tool and a logger on the same device, a master and several device programs on one line, or a device on one computer used from another.</p>

<h2>How Virtual Serial Studio does it</h2>
<p>Studio opens the device once and makes a virtual port for each program. A link joins the ports, and the kind of link decides who hears whom.</p>
<div class="vss-table"><table>
<thead><tr><th scope="col">In Studio</th><th scope="col">What it does</th></tr></thead>
<tbody>
<tr><th scope="row"><strong>Share my device with several programs</strong>, or a <strong>Split one port to many</strong> link</th><td>Everything the device receives is copied to every program's port, and whatever a program writes goes back to the device. Up to 32 programs.</td></tr>
<tr><th scope="row"><strong>Send several into one</strong>, or a <strong>Join many ports into one</strong> link</th><td>The receiver gets every sender's data. <strong>Choose one owner at a time</strong> lets only the owner's bytes through.</td></tr>
<tr><th scope="row"><strong>Let programs take turns sending to a device</strong></th><td>A split from the device to the programs with an owner, so one program sends at a time and all of them get the device's replies.</td></tr>
<tr><th scope="row"><strong>Put several on one line</strong>, or a <strong>Shared bus</strong> link</th><td>Every port hears the others and none hears itself, like stations on one multi-drop line. A port can be on one bus, and a bus holds up to 64 ports. It carries no modem lines.</td></tr>
<tr><th scope="row"><strong>Monitor</strong> link, or <strong>Watch traffic</strong></th><td>A read-only copy of a port, which never drives modem lines.</td></tr>
<tr><th scope="row"><strong>Let other computers use my device</strong>, or <strong>Ports, Network endpoint</strong></th><td>Bridges the device to a network address, as <code>vss_cli bridge</code> does.</td></tr>
</tbody></table></div>
<p>A network port can be a member of a bus or a join, so a device program on another computer can sit on the same bus. A link that would close a loop is refused with "That link would create a loop".</p>

<h3>Network addresses</h3>
<div class="vss-table"><table>
<thead><tr><th scope="col">Address</th><th scope="col">Meaning</th></tr></thead>
<tbody>
<tr><th scope="row"><code>serial://COM3?baud=115200</code></th><td>A real serial device.</td></tr>
<tr><th scope="row"><code>tcp://host:port</code></th><td>A TCP client: Studio connects to a server.</td></tr>
<tr><th scope="row"><code>udp://host:port</code></th><td>UDP datagrams to and from one address.</td></tr>
<tr><th scope="row"><code>tcp-listen://host:port</code></th><td>A TCP server: Studio waits for other computers. Up to 32 can connect, and what you send goes to all of them.</td></tr>
<tr><th scope="row"><code>udp-listen://host:port</code></th><td>UDP datagrams from any sender. Replies go to the senders heard from lately.</td></tr>
<tr><th scope="row"><code>rfc2217://host:port</code></th><td>An RFC 2217 client: Studio connects to an access server, and sends the speed, framing and modem lines.</td></tr>
<tr><th scope="row"><code>rfc2217-listen://host:port</code></th><td>An RFC 2217 server that serial-over-network clients, such as pySerial's <code>rfc2217://</code>, connect to.</td></tr>
</tbody></table></div>

<h2>Example</h2>
<p>Share a device with three programs for a minute. Each listener gets its own virtual port.</p>
<pre class="vss-code"><code>vss_cli share serial://COM3 --listeners=3 --seconds=60</code></pre>
<p>Put the master and two device programs on one bus, with a listen-only port for an analyser:</p>
<pre class="vss-code"><code>vss_cli bus --virtual=3 --tap=virtual</code></pre>
<p>Bridge a device to a network address, and let only one subnet connect:</p>
<pre class="vss-code"><code>vss_cli bridge serial://COM3 rfc2217-listen://192.168.1.10:7001?allow=192.168.1.0/24</code></pre>
<p>On the other computer, open it as a serial port with an RFC 2217 client such as <code>rfc2217://192.168.1.10:7001</code> in pySerial.</p>

<h2>Keeping it safe</h2>
<ul>
<li>An RFC 2217 server has no password, and nothing a network port carries is encrypted. <code>?allow=</code> after the port takes addresses or ranges, and a computer that is not on the list is disconnected before it can send or receive a byte. An address can be forged, so the list keeps strangers on the same network away and nothing more.</li>
<li>Listen on <code>127.0.0.1</code> when the other program is on this computer. For another computer, use an SSH tunnel or a VPN. Do not listen on <code>0.0.0.0</code> on a network you do not control.</li>
<li>Studio and <code>vss_cli bridge</code> warn when an RFC 2217 server can be reached from another computer and has no <code>allow=</code> list.</li>
<li>A listening port keeps what it has to send to each client separately. While every client is more than 1 MiB behind, the port slows whatever sends to it. When one client catches up to within 1 MiB, any client still further behind is disconnected. So is a client that takes no data for 30 seconds.</li>
</ul>

<h2>What is not covered</h2>
<ul>
<li>No password and no encryption on network ports. TLS for a network port is not built.</li>
<li>A bus does not stop two programs sending at once. Their bytes are interleaved, not corrupted. A half-duplex line setting reports collisions.</li>
<li>A real serial device is opened by one program. Studio cannot tell in advance that another program holds it, so a busy device fails when you attach it.</li>
<li>Settings that an RFC 2217 client sends are accepted and reported, and are not applied to a real device linked to the server. DTR, RTS and break are passed on.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/#connecting-ports">Connecting ports</a></li>
<li><a href="../../guide/#network-addresses">Network addresses</a></li>
<li><a href="../../guide/#keeping-it-safe">Keeping it safe</a></li>
<li><a href="../../guide/scenarios/">Scenario recipes</a></li>
</ul>
</div></div></section>
