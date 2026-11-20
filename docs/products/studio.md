---
template: marketing.html
title: Studio
title_tag: "Studio, the application: Virtual Serial Studio"
description: "Studio makes virtual serial ports, shares one real device between programs, records traffic, bridges a port to the network and answers like a device."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Products</p>
<h1>Studio, the application</h1>
<p class="icl-lead">Give a program a serial port, share one real device between several programs, watch and record what goes through, and put a port on the network. All of it is in one window, and none of it needs a second serial cable.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../guide/">Read the guide</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-row">
<div>
<h2>Start with what you need to do</h2>
<p>Studio opens on the question "What do you want to do?". Choose a job, fill in one short form and press <strong>Create</strong>. Studio builds the ports and then tells you which port to open in which program, for example "In PuTTY, open COM7 at 115200 baud".</p>
<p>When you want more control, switch to <strong>Advanced</strong> with <strong>View, Advanced mode</strong>. It shows the same ports as a diagram you can build on, with a Ports list, an Inspector, a Send panel and a Monitor. Nothing you made changes when you switch.</p>
<ul class="vss-links"><li><a href="../../guide/getting-started/#simple-and-advanced">Simple and Advanced</a></li><li><a href="../../guide/getting-started/#quick-start">Make your first port</a></li></ul>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/simple-jobs-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/simple-jobs-light.png" width="1280" height="800" alt="The Studio window at the Simple level. The list of setups on the left is empty. On the right, under the question What do you want to do, are job cards in groups: Give my program a serial port, Connect two programs to each other, Use my device through a new port, Share my device with several programs, Try my device from this window, and Let other computers use my device."></picture></figure>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>What you can do with it</h2>
<p>Each task below is one job in the window, with the name you see there.</p>
</div>
<ul class="vss-ticks">
<li><strong>Give my program a serial port.</strong> Studio makes a virtual port that the program opens like any other, a <code>COM</code> port on Windows and a device path on Linux and macOS. On Windows you can ask for the COM number the program expects.</li>
<li><strong>Connect two programs to each other.</strong> Studio makes two ports wired together like a null-modem cable, so what one program writes the other reads.</li>
<li><strong>Test without the hardware.</strong> In <strong>Give my program a serial port</strong>, choose <strong>Send everything straight back</strong>, so the program talks to a port that echoes. Or type to the port yourself from the window.</li>
<li><strong>Use my device through a new port.</strong> Studio opens the device once and keeps it open, and your program opens the new port instead.</li>
<li><strong>Share my device with several programs.</strong> Each program gets its own port, and everything the device sends reaches all of them. <strong>Let programs take turns sending to a device</strong> lets one program send at a time while all of them hear the replies.</li>
<li><strong>Put several on one line.</strong> Studio makes a shared bus, like an RS-485 line, where every port hears the others.</li>
</ul>
<ul class="vss-links"><li><a href="../../guide/getting-started/#jobs-simple">All the jobs</a></li><li><a href="../../guide/adding-ports/#virtual-port">Adding a port</a></li><li><a href="../../guide/connecting-ports/#connecting-ports">Connecting ports</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-row vss-row--flip">
<div>
<h2>Share a device</h2>
<p>A serial device can usually be opened by one program at a time. In <strong>Share my device with several programs</strong> you choose the device and how many programs, from 1 to 32. Studio opens the device and makes a port for each program.</p>
<p>The last step lists what to open in each program, with a <strong>Copy</strong> button for each port. <strong>Add another program</strong> adds a port later without interrupting the others.</p>
<ul class="vss-links"><li><a href="../../guide/sharing-a-device/#sharing-one-device-with-several-programs">Sharing one device with several programs</a></li><li><a href="../../guide/sharing-a-device/#one-master-several-programs-and-a-silent-listener">One master and a silent listener</a></li></ul>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/share-device-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/share-device-light.png" width="760" height="640" alt="The last step of sharing a device. COM3 is shared with 3 programs. The terminal program opens COM7 and the test-tool program opens COM8. The logger has a port only Studio can open. A message says everything COM3 receives is now copied to all 3 ports."></picture></figure>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-row">
<div>
<h2>Watch and record traffic</h2>
<p>The <strong>Monitor</strong> shows a row for every transfer, with its time, port, direction (RX or TX), size and the bytes as text or hex. You can filter by port and direction, search the bytes, and pause to read.</p>
<p>At the Simple level, <strong>Show me the data going through</strong> on a setup's page does the same for that setup. <strong>Export capture</strong> saves every byte with its time, as <code>.cap</code>, <code>.csv</code> or <code>.jsonl</code>, so another tool can read it. The Monitor keeps the latest 10,000 rows, so for a longer record use the command line.</p>
<ul class="vss-links"><li><a href="../../guide/monitor-and-capture/#watching-traffic">Watching traffic</a></li><li><a href="../../guide/monitor-and-capture/#capture-files">Capture files</a></li><li><a href="../../guide/command-line/">Command line equivalents</a></li></ul>
</div>
<figure class="vss-shot"><picture><source srcset="../../images/site/monitor-hex-dark.png" media="(prefers-color-scheme: dark)"><img src="../../images/site/monitor-hex-light.png" width="1280" height="420" alt="The Monitor showing hex. Rows alternate between a terminal port sending and a modem port receiving, each with a time, a direction, a byte count and the bytes in hex, for an AT command session. Pause, Clear, Follow, Export and Export capture are above the rows."></picture></figure>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Put a port on the network</h2>
<p>Choose <strong>Let other computers use my device</strong>. Studio bridges the device to a network port, 4001 by default, so a program on another computer can reach it. At the Advanced level, <strong>Ports, Network endpoint</strong> offers TCP, UDP and RFC 2217, and lets Studio connect out to a server instead of waiting for one.</p>
<p>The data crosses the network as it is, with no password and no encryption. Keep a listener on the computer itself, use an SSH tunnel or a VPN, or limit it to the addresses that may connect. The guide shows each way.</p>
<ul class="vss-links"><li><a href="../../guide/network/#network-addresses">Network addresses</a></li><li><a href="../../guide/network/#keeping-it-safe">Keeping it safe</a></li></ul>
</div>
<div>
<h2>Answer like a device</h2>
<p>At the Advanced level, <strong>Ports, Mock fixture</strong> adds a pretend device. It can echo what it receives, or send a line of text, a fixed pattern, a counter or random bytes at a rate you choose.</p>
<p>To see how a program copes when something goes wrong, open <strong>Open line matrix and faults</strong> on the fixture and use the Inject buttons: a parity error, a framing error, an overrun, a break, a hang-up or a disconnect.</p>
<ul class="vss-links"><li><a href="../../guide/adding-ports/#mock-fixture-advanced">Mock fixture</a></li><li><a href="../../solutions/">Solutions</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Run a saved setup</h2>
<p>A setup is the list of your ports and links. <strong>File, Save</strong> writes it to a file and <strong>File, Open</strong> builds it again.</p>
<p><strong>File, Setups and templates</strong> has four to start from: a test bench with no hardware, a device shared with three programs, a device on the network, and two programs sharing one device one at a time. Closing the window keeps the ports running, and Studio stays in the notification area until you quit it.</p>
<ul class="vss-links"><li><a href="../../guide/setups/#saving-opening-and-templates">Saving, opening and templates</a></li><li><a href="../../guide/setups/#when-the-window-is-closed">When the window is closed</a></li></ul>
</div>
<div>
<h2>What your system needs</h2>
<ul class="vss-ticks">
<li><strong>Windows:</strong> a virtual port is a <code>COM</code> port made by the Studio driver, which the installer offers to install. The driver is test-signed, so Windows accepts it only on a computer where the certificate authority of Iain Chesworth Labs has been installed. The guide says how. <span class="vss-chip vss-chip--plan">Microsoft-signed driver: Planned</span></li>
<li><strong>Linux:</strong> a virtual port is a pseudo terminal, so Studio needs no driver.</li>
<li><strong>macOS:</strong> a virtual port is a pseudo terminal, so Studio needs no driver. The release is not signed, so macOS asks you to confirm before it opens Studio.</li>
</ul>
<ul class="vss-links"><li><a href="../../guide/windows/">Windows in the guide</a></li><li><a href="../../platforms/">Platforms</a></li><li><a href="../../guide/limits/#what-this-build-cannot-do">What this build cannot do</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Good to know</h2>
<ul class="vss-ticks">
<li>Every control has an accessible name and can be reached from the keyboard, and the window follows the system's light or dark setting, or you can choose one.</li>
<li>Ctrl+K opens a search over every action.</li>
<li>The window can be shown in French, Spanish, German, Hebrew, Yiddish, Japanese, Simplified Chinese and Arabic. They are machine translations that no speaker has reviewed.</li>
<li>Studio sends no usage data, crash reports or account details, and there is no activation server.</li>
</ul>
</div>
<div>
<h2>Next</h2>
<p>The Community edition is free under the GPL and is the whole application. Plugins add more to it, and a test script can drive ports on the same computer.</p>
<ul class="vss-links"><li><a href="../plugins/">Plugins</a></li><li><a href="../agent-gateway/">Agent and gateway</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div>
</div>
</div></section>
