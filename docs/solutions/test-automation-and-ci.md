---
template: marketing.html
title: Automate tests in a lab
title_tag: "Automate tests in a lab: Virtual Serial Studio"
description: "Build a set of ports once, save it as a file, and bring it back the same way on every run, from a terminal or a CI job with no window. Record what happens and keep a test report."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Automate tests in a lab</p>
<h1>Automate tests in a lab</h1>
<p class="icl-lead">Build the ports a test needs once, then bring them back the same way every time, with no window.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A test of serial software needs ports, a device that answers, and a way to see what happened. Wiring that by hand for every run is slow, and nobody remembers the settings. On a build machine there is no window and nobody to press a button.</p>

<h2>What you do</h2>
<ol>
<li>Build the setup in Studio: the ports, the links between them, and any delay. Choose <strong>File, Save</strong>. The setup is a file you can keep with your tests. <strong>File, Setups and templates</strong> has a test bench with no hardware to start from.</li>
<li>At the Advanced level, <strong>File, Export as command</strong> shows the <code>vss_cli</code> command that builds the same setup.</li>
<li>Run it with no window. <code>vss_cli run</code> makes the ports, keeps them until you stop it or the time is up, and takes them down when it ends.</li>
<li>Put a device on a port with <code>vss_cli respond</code>, and record the run with <code>vss_cli capture</code>. Compare the recording with one from a good run.</li>
</ol>

<h2>Example</h2>
<p>Run a saved setup for an hour, and answer on the port a device would use:</p>
<pre class="vss-code"><code>vss_cli run bench.json --seconds=3600
vss_cli respond device.json --uri=serial://COM7</code></pre>
<p>A mistake in a command line ends the command with exit code 2, so a CI job fails at once. On Linux and macOS the ports are pseudo terminals with stable names, and need no driver. On Windows they need the Studio driver (see <a href="../../products/drivers/">Drivers</a>).</p>

<h2>Wait for a reply and keep a test report</h2>
<p>From a test script you can make ports, link them, send bytes, wait for a reply and keep the result as a JUnit file for your CI system. This needs the agent, the gateway and the Python client. <span class="vss-chip vss-chip--src">Built from source only</span></p>
<pre class="vss-code"><code>vss_cli control port create a          # prints the id, such as p_1
vss_cli control port create b          # p_2
vss_cli control link p_1 p_2
vss_cli control run begin smoke
vss_cli control expect p_2 --pattern=4f4b0d0a --timeout=2000 \
    --send-to=p_1 --send=41540d0a4f4b0d0a --label="the device answers"
vss_cli control run end
vss_cli control run report --out=smoke.xml</code></pre>
<p>The ports made this way live inside the agent, so another program cannot open them. Use them to test a script or a protocol against bytes you send yourself. The guide has the steps to start the agent and the gateway.</p>

<h2>Things to know</h2>
<ul>
<li>The gateway controls one computer. Controlling another computer is not built. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Client libraries other than Python are not built. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>To check a plugin in CI, <code>vss_cli plugin run</code> exits with 0 when every instance ran, and 1 when the plugin or an instance never ran.</li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../guide/setups/">Setups: saving, opening and templates</a></li>
<li><a href="../../guide/command-line/">Command line equivalents</a></li>
<li><a href="../../guide/control-from-a-script/">Controlling Studio from a script</a></li>
<li><a href="../../products/agent-gateway/">Agent, gateway and Python client</a></li>
</ul>
</div></div></section>
