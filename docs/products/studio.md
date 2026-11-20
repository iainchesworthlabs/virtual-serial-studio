---
template: marketing.html
title: Studio
title_tag: "Studio, the application: Virtual Serial Studio"
description: "Studio makes virtual serial ports, shares a real device with several programs, links ports in a diagram and records traffic, at a Simple or an Advanced level."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Products</a> / Studio</p>
<h1>Studio, the application</h1>
<p class="icl-lead">Make a serial port for a program, share a real device with several programs, link ports to each other, and watch the traffic on every port, from one window.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../guide/">Read the guide</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-row">
<div>
<h2>Simple and Advanced</h2>
<p>Studio opens at the Simple level, on the question "What do you want to do?". Each job is one form, and Studio tells you which port to open in which program. A setup of your own is listed on the left with its own page, which shows whether a program has opened the port and lets you type to it and watch the data.</p>
<p>The Advanced level is a diagram you build on, with a Ports list, an Inspector, a Send panel and a Monitor. Both levels show the same ports. A setup made at one level opens at the other. Switch with <strong>View, Advanced mode</strong> or the Simple and Advanced switch in the menu bar.</p>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/simple-jobs-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/simple-jobs-light.png" width="1280" height="800" alt="The Studio window at the Simple level. The list of setups on the left is empty. On the right, under the question What do you want to do, are job cards in groups: Give my program a serial port, Connect two programs to each other, Use my device through a new port, Share my device with several programs, Try my device from this window, and Let other computers use my device."></picture></figure>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>The jobs at the Simple level</h2>
<p>The jobs are in four groups. A sentence under the form says what will be built, and <strong>Create</strong> builds it. If a step fails, Studio removes what it made.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">Job</th><th scope="col">What Studio builds</th></tr></thead>
<tbody>
<tr><th scope="row">Give my program a serial port</th><td>One virtual port.</td></tr>
<tr><th scope="row">Connect two programs to each other</th><td>Two virtual ports linked both ways, like a null-modem cable.</td></tr>
<tr><th scope="row">Use my device through a new port</th><td>The device, and one virtual port for your program.</td></tr>
<tr><th scope="row">Share my device with several programs</th><td>The device, and a virtual port for each program.</td></tr>
<tr><th scope="row">Try my device from this window</th><td>The device alone, to type to and read from in the window.</td></tr>
<tr><th scope="row">Let other computers use my device</th><td>The device bridged to a network port, 4001 by default.</td></tr>
<tr><th scope="row">Put several on one line</th><td>A shared bus, where every port hears the others.</td></tr>
<tr><th scope="row">Send several into one</th><td>A join: everything the senders write reaches one receiver.</td></tr>
<tr><th scope="row">Let programs take turns sending to a device</th><td>A split from the device to the programs with an owner, so one program sends at a time and all of them get the device's replies.</td></tr>
<tr><th scope="row">See the data on a port</th><td>Nothing new. It shows the data going through a setup you already have.</td></tr>
<tr><th scope="row">Check this PC is ready</th><td>Opens the setup check, which says what is missing for ports other programs can open.</td></tr>
</tbody></table></div>
<ul class="vss-links"><li><a href="../../guide/getting-started/#jobs-simple">Jobs in the guide</a></li><li><a href="../../solutions/">Solutions</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-row vss-row--flip">
<div>
<h2>The diagram</h2>
<p>At the Advanced level the diagram has a toolbox on its left. Under Ports it has Port, Fixture, Loopback, Device and Network. Under Bundles it has Pair, Split, Join, Bus, Switch, Redirect, Monitor, Share and Bench. Drag a tile onto the diagram, or select it and press Enter.</p>
<p>Drag from the handle on a port to another port to link them two-way, one way, or as a monitor. A link that would close a loop is refused. Right-click a port for its serial settings, and right-click a link for its delay, jitter and wire timing. Undo and Redo cover connecting and removing links, changing an owner and renaming a port, and, on the diagram, adding from the toolbox, duplicating, deleting and changing settings.</p>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/advanced-diagram-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/advanced-diagram-light.png" width="1280" height="800" alt="The Studio window at the Advanced level. A Ports list on the left shows console, logger and modem. The diagram shows console linked two-way to logger, with a modem fixture not yet linked. An Inspector and a Send panel are on the right, and the Monitor below lists RX and TX rows of an AT command."></picture></figure>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Ports</h2>
<p>There are five kinds of port to add, from <strong>Ports, Add port</strong> or from the diagram.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">Kind</th><th scope="col">What it is</th></tr></thead>
<tbody>
<tr><th scope="row">Virtual port</th><td>A port other programs can open: a <code>COM</code> port on Windows with the driver, a pseudo terminal on Linux and macOS, or an in-process loopback that only Studio can open. You can make up to 32 at once, and on Windows you can choose the COM number.</td></tr>
<tr><th scope="row">Linked pair</th><td>Two ports wired together like a null-modem cable, with DTR to DSR and RTS to CTS.</td></tr>
<tr><th scope="row">Real device</th><td>A serial device plugged into this computer. Studio opens it once and keeps it open, and your programs open the port Studio makes. Baud rate, data bits, parity, stop bits and flow control are set when you attach it.</td></tr>
<tr><th scope="row">Mock fixture</th><td>A scripted pretend device for tests without hardware. It can echo, or send a text line, a fixed pattern, a counter or random bytes at a set rate.</td></tr>
<tr><th scope="row">Network endpoint</th><td>A <code>tcp</code>, <code>udp</code> or <code>rfc2217</code> port, as a client that connects out or as a listener that waits for other computers. A listener can take a list of the addresses allowed to connect.</td></tr>
</tbody></table></div>
<p>Links between ports are two-way, one-way, monitor, split, join and shared bus. A bus holds up to 64 ports and carries no modem lines. A link can hold what it carries for a set delay with jitter, and can pace its bytes at the time a wire would take.</p>
<ul class="vss-links"><li><a href="../../guide/adding-ports/#adding-a-port">Adding a port</a></li><li><a href="../../guide/connecting-ports/#connecting-ports">Connecting ports</a></li><li><a href="../../guide/network/#network-addresses">Network addresses</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-row">
<div>
<h2>The Monitor and capture export</h2>
<p>The Monitor shows a row for each transfer with its time, port, direction (RX, TX or DROP), size and a preview as ASCII or hex. You can filter by port and direction, search the payload, pause, and export the rows as text.</p>
<p><strong>Export capture</strong> saves every byte with the time the engine saw it, as one of three formats. The Monitor keeps the latest 10,000 rows and no more than 32 MiB of bytes. For a record that does not stop there, use <code>vss_cli capture</code>.</p>
<div class="vss-table"><table>
<thead><tr><th scope="col">Format</th><th scope="col">What it holds</th></tr></thead>
<tbody>
<tr><th scope="row"><code>.cap</code></th><td>Text, one line for each transfer, with no port names.</td></tr>
<tr><th scope="row"><code>.csv</code></th><td>A header row, then the time, offset, port, direction, length and bytes of each transfer.</td></tr>
<tr><th scope="row"><code>.jsonl</code></th><td>One JSON object for each transfer, with the port and direction.</td></tr>
</tbody></table></div>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/monitor-hex-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/monitor-hex-light.png" width="1280" height="420" alt="The Monitor showing hex. Rows alternate between a terminal port sending and a modem port receiving, each with a time, a direction, a byte count and the bytes in hex, for an AT command session. Pause, Clear, Follow, Export and Export capture are above the rows."></picture></figure>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-row vss-row--flip">
<div>
<h2>Mock fixtures and faults</h2>
<p>A mock fixture stands in for a device you do not have. In Advanced you can give it a fault: a parity error, a framing error, an overrun, a break, a hang-up or a disconnect. Each is reported as an error event on the port, and the fixture then shows Failed, as a real device would after the fault.</p>
<p>The same dialog has a line matrix. It sets which of DTR and RTS the fixture drives into DSR, CTS, DCD and RI on the port it is linked to. On Windows the faults also work on a port of the Studio driver, where they are raised at the program that has the port open.</p>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/fixture-faults-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/fixture-faults-light.png" width="1280" height="760" alt="The line wiring and faults dialog for a mock fixture called modem. On the left, a table sets which of DTR and RTS drive DSR, CTS, DCD and RI. On the right are six Inject buttons: Parity error, Framing error, Overrun, Break, Hang-up and Disconnect."></picture></figure>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Setups as JSON, and templates</h2>
<p>A setup is a JSON document that lists the ports and the links. <strong>File, Save</strong> writes it and <strong>File, Open</strong> applies it. <code>vss_cli run</code> runs the same file without the window.</p>
<p><strong>File, Setups and templates</strong> opens four templates: a test bench with no hardware, a device shared with three programs, a device on the network, and two programs sharing one device one at a time. At the Advanced level, <strong>Export as command</strong> shows the <code>vss_cli</code> command that builds the current setup, and the Topology JSON tab edits the setup as text and names the line of a mistake.</p>
<ul class="vss-links"><li><a href="../../guide/setups/#saving-opening-and-templates">Saving and templates</a></li><li><a href="../command-line/">Command line</a></li></ul>
</div>
<div>
<h2>Plugins</h2>
<p><strong>Tools, Plugins...</strong> lists the plugins on this computer, who stands behind each one, what it adds and what it may do. A plugin runs as a program of its own, apart from Studio, only after you have decided to trust that version. Studio never downloads a plugin.</p>
<p>No official plugin exists yet. <span class="vss-chip vss-chip--plan">Planned</span></p>
<ul class="vss-links"><li><a href="../plugins/">Plugins</a></li><li><a href="../../guide/plugins/#plugins">Plugins in the guide</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Accessibility</h2>
<p>These are properties the code gives, and a self-test of the window checks them in the light and dark themes and again at 200 percent scale.</p>
<ul class="vss-ticks">
<li>Every control has an accessible name and a role.</li>
<li>Every control can be reached from the keyboard and shows a focus ring.</li>
<li>Every target is at least 24 by 24 pixels, and every dialog works at 200 percent text.</li>
<li>The theme follows the system's light or dark setting, or you can choose one. The colour pairs are held to WCAG 2.2 contrast ratios by a check that runs in the project's continuous integration.</li>
<li>Ctrl+K opens a search over every action, and Keyboard shortcuts lists them.</li>
<li>The diagram's links are also available as a list that a screen reader can walk, from the View menu.</li>
</ul>
</div>
<div>
<h2>Languages</h2>
<p>The window can be shown in French, Spanish, German, Hebrew, Yiddish, Japanese, Simplified Chinese and Arabic. Hebrew, Yiddish and Arabic switch the layout to right to left. They are machine translations that no speaker has reviewed.</p>
<h2>Other things Studio does</h2>
<ul class="vss-ticks">
<li>Closing the window keeps the ports running. Studio stays in the notification area until you quit it.</li>
<li>Tools, Setup check says what the operating system may stop Studio doing and the smallest step to fix it.</li>
<li>Tools, Benchmark sends 1, 10 or 100 MB from one port and counts what arrives on another.</li>
<li>File, Support bundle writes a zip you choose the parts of. It never holds the bytes your ports sent and received.</li>
<li>Studio sends no usage data, crash reports or account details, and there is no activation server.</li>
</ul>
</div>
</div>
<ul class="vss-links"><li><a href="../../guide/limits/#what-this-build-cannot-do">What this build cannot do</a></li><li><a href="../../platforms/">Platforms</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div></section>
