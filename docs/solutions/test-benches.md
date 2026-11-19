---
template: marketing.html
title: Test benches
title_tag: "Test benches: Virtual Serial Studio"
description: "Build a bench of virtual ports, linked pairs, modem control lines and a multi-drop bus, and keep it as a JSON setup file for Studio or a script."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Test benches</p>
<h1>Test benches</h1>
<p class="icl-lead">Build the whole set of ports a test needs, keep it as a file, and bring it back the same way every time.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A bench for serial software is more than one port: a master, several devices, a listener, a cable between two programs, and control lines that must do what a cable does. Wiring it by hand each time is slow, and nobody remembers the settings.</p>

<h2>How Virtual Serial Studio does it</h2>
<ul>
<li><strong>Several ports.</strong> At the Advanced level the toolbox has Port, Fixture, Loopback, Device and Network, and bundles Pair, Split, Join, Bus, Switch, Redirect, Monitor, Share and Bench. <strong>Bench</strong> makes several fixtures, each paired with its own port.</li>
<li><strong>Linked pairs and the null-modem link.</strong> A linked pair is two ports wired like a null-modem cable, with DTR to DSR and RTS to CTS.</li>
<li><strong>Modem control lines.</strong> The Inspector has switches for DTR and RTS, a Break button, and chips for CTS, DSR, DCD and RI. On a fixture, <strong>Open line matrix and faults</strong> sets which of DTR and RTS drives which input on the port at the other end, with the presets Standard null-modem, Loopback and Custom.</li>
<li><strong>A multi-drop bus.</strong> A <strong>Shared bus</strong> link gives each program its own port, and each hears every other and none hears itself. A <strong>Join</strong> to one more port makes a listener that hears every member and sends to none. At the Advanced level the bus can be a half-duplex line with a line rate, which reports two ports sending at once.</li>
<li><strong>Saved setups.</strong> A setup is a JSON file of the ports and the links. <strong>File, Save</strong> writes it, <strong>File, Open</strong> applies it, and <code>vss_cli run</code> runs it with no window. <strong>File, Setups and templates</strong> opens four templates, one of them a test bench with no hardware. <strong>Export as command</strong> shows the <code>vss_cli</code> command for the current setup.</li>
<li><strong>A check of the bench itself.</strong> <strong>Tools, Benchmark</strong> sends 1, 10 or 100 MB from one port and counts what arrives on another.</li>
</ul>

<h2>Example</h2>
<p>A master, two devices and a listener. The devices and the master share a bus, and the listener hears all three.</p>
<pre class="vss-code"><code>{
  "version": 1,
  "name": "polled-bus",
  "ports": [
    { "name": "master",   "kind": "virtual" },
    { "name": "device-1", "kind": "virtual" },
    { "name": "device-2", "kind": "virtual" },
    { "name": "listener", "kind": "virtual" }
  ],
  "links": [
    { "mode": "bus", "members": ["master", "device-1", "device-2"] },
    { "mode": "join", "sources": ["master", "device-1", "device-2"],
      "targets": ["listener"], "emulate_modem_lines": false }
  ]
}</code></pre>
<p>Save it as <code>polled-bus.json</code>, open it with <strong>File, Open</strong>, or run it for an hour:</p>
<pre class="vss-code"><code>vss_cli run polled-bus.json --seconds=3600</code></pre>
<p>A topology file must have <code>"version": 1</code>, and a member Studio does not know is an error, so a misspelt name is found when the file opens. <code>vss_cli bus --virtual=3 --tap=virtual</code> makes the same bus from the command line and prints the port each program should open.</p>

<h2>What is not covered</h2>
<ul>
<li>A port has no electrical mode. A bus models who hears whom, not the wiring. A program that toggles RTS to switch a transceiver still runs, and the line goes nowhere, because a bus carries no modem lines.</li>
<li>A bus does not echo a station's own bytes back to it. Gating a half-duplex bus by RTS applies to loopback ports only.</li>
<li>The wiring set in the line matrix lasts while Studio runs and is not saved in a setup file.</li>
<li>Undo and Redo do not cover adding or removing a port with a dialog or a wizard, or applying JSON.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/#one-master-several-programs-and-a-silent-listener">One master, several programs and a silent listener</a></li>
<li><a href="../../guide/#saving-opening-and-templates">Saving, opening and templates</a></li>
<li><a href="../../guide/#rs-485-is-half-duplex-and-a-bus-can-be">Half-duplex buses</a></li>
<li><a href="../../guide/scenarios/">Scenario recipes</a></li>
</ul>
</div></div></section>
