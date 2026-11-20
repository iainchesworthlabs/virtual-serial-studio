---
template: marketing.html
title: Test automation and CI
title_tag: "Test automation and CI: Virtual Serial Studio"
description: "Run serial test setups with no window, drive ports from a shell or Python test script, wait for a reply, and keep a JUnit report for your CI system."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Solutions</a> / Test automation and CI</p>
<h1>Test automation and CI</h1>
<p class="icl-lead">Run the ports a test needs with no window, send bytes, wait for the answer, and hand the result to your CI system.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap"><div class="vss-text">
<h2>The problem</h2>
<p>A test of serial software needs ports, a device that answers, and a way to say whether the answer came in time. On a build machine there is no window, no hardware, and nobody to press a button.</p>

<h2>How Virtual Serial Studio does it</h2>
<ul>
<li><code>vss_cli run topology.json</code> applies a setup saved from Studio and keeps it until Ctrl-C, SIGTERM, or <code>--seconds=N</code>. It needs no window, and it takes its ports down before it ends. A mistake in the command line ends it with exit code 2.</li>
<li><code>vss_cli vport</code> makes pseudo terminals with stable names on Linux and macOS. The platform matrix does this on the hosted runners. <code>vss_cli respond</code> makes a port answer like a device. See <a href="../device-emulation/">Device emulation</a>.</li>
<li>The control gateway lets a script make ports, link them, send bytes and wait for a pattern with <code>expect</code>. The gateway starts listening before it sends, so a fast reply is not missed, and a wait that runs out ends with exit code 1 or a <code>timeout</code> error.</li>
<li>A run collects the checks of a test and gives them back as JUnit XML. While a run is open, each <code>expect</code> is noted with how it ended, and you can add checks of your own with an outcome of <code>passed</code>, <code>failed</code>, <code>error</code> or <code>skipped</code>.</li>
<li>The Python client, <code>vss_control</code>, needs Python 3.9 or later and nothing else. It does the same from a test script.</li>
</ul>
<p>The agent, the gateway and the Python client are built from source, and are not in the packages. <span class="vss-chip vss-chip--src">Built from source only</span> The gateway controls one computer, and its ports are loopback ports in the agent, so use it to test a script or a protocol against bytes you send yourself.</p>

<h2>Example</h2>
<p>Start the agent and the gateway, then run one test step and keep a report. This is the same flow as <a href="../../guide/control-from-a-script/">Controlling Studio from a script</a>.</p>
<pre class="vss-code"><code>vss_agent &amp;
vss_gate --local-role=control &amp;
vss_cli control port create a          # prints the id, such as p_1
vss_cli control port create b          # p_2
vss_cli control link p_1 p_2
vss_cli control run begin smoke
vss_cli control expect p_2 --pattern=4f4b0d0a --timeout=2000 \
    --send-to=p_1 --send=41540d0a4f4b0d0a --label="the device answers"
vss_cli control run end
vss_cli control run report --out=smoke.xml</code></pre>
<p>From Python, <code>Client.connect()</code> gives the same calls: <code>create_port</code>, <code>connect_ports</code>, <code>begin_run</code>, <code>expect_bytes</code>, <code>end_run</code> and <code>report_junit</code>. The gateway, its tokens, the Python client and the JUnit report need no licence.</p>
<p>To test a plugin in CI, <code>vss_cli plugin run &lt;id&gt; --seconds 30 --instance fixture:&lt;type&gt;</code> exits with 0 when every instance ran, and 1 when the plugin or an instance never ran.</p>

<h2>What is not covered</h2>
<ul>
<li>One computer only. Controlling another computer needs TLS, which is not built. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Windows virtual <code>COM</code> ports need the driver, which needs administrator rights and a signature the computer trusts. The project runs its driver tests in guests on the owner's machine and not on hosted runners.</li>
<li>Seeded random fault injection from a test, and response timing rules, have no way in yet. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Client libraries other than Python. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>

<h2>More</h2>
<ul class="vss-links">
<li><a href="../../products/agent-gateway/">Agent, gateway and Python client</a></li>
<li><a href="../../guide/command-line/">Command line</a></li>
<li><a href="../../guide/control-from-a-script/">Controlling Studio from a script</a></li>
<li><a href="../../developers/testing/">Testing</a></li>
</ul>
</div></div></section>
