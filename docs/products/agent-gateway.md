---
template: marketing.html
title: Agent and gateway
title_tag: "Agent and gateway: drive serial ports from a script: Virtual Serial Studio"
description: "Make serial ports, send bytes, wait for a reply and keep a JUnit report from a Python script or test harness, on one computer. Built from source today."
---
<!-- OWNER DECISION: when the agent and the gateway go into the packages. docs/agent.md says the owner has decided that vss_agent and vss_gate will be in the installers and packages, and gives no release. Until then this page says they are built from source only. -->
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Products</p>
<h1>Agent and gateway: drive ports from a script</h1>
<p class="icl-lead">For a test engineer. Let a Python script or a test harness make serial ports, send bytes, wait for the reply and keep a report, with no window and nobody at the keyboard. <span class="vss-chip vss-chip--src">Built from source only</span></p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>What it lets a test do</h2>
<ul class="vss-ticks">
<li><strong>Create ports and link them.</strong> A test makes the ports it needs, links them one way or both ways, and removes them when it finishes.</li>
<li><strong>Send bytes and wait for a reply.</strong> One call sends a request and waits, up to a time you set, for the bytes you expect. The gateway starts listening before it sends, so a fast reply cannot be missed.</li>
<li><strong>Know when nothing came.</strong> A wait that runs out ends with a <code>timeout</code> error, and the connection stays usable for the next step.</li>
<li><strong>Keep a report.</strong> While a run is open, each wait is noted with how it ended, and you can add checks of your own. The run comes back as JUnit XML for your CI system.</li>
<li><strong>Watch what happens.</strong> Read the stream of events and traffic on the ports, with or without the bytes.</li>
</ul>
<p>It is the same Community edition as Studio, and the gateway, its Python client and the report need no licence.</p>
</div>
<div>
<h2>Three programs</h2>
<ul class="vss-ticks">
<li><strong>The agent</strong>, <code>vss_agent</code>, keeps your virtual ports and links running while your tests come and go. One runs for each user, and it closes every port when it stops.</li>
<li><strong>The gateway</strong>, <code>vss_gate</code>, lets your programs control the agent. By default only your own user account can open it.</li>
<li><strong>The Python client</strong>, <code>vss_control</code>, is what a test script imports. It needs Python 3.9 or later and no other package.</li>
</ul>
<p>The command line has the same steps as commands, for a shell script or a CI job.</p>
<ul class="vss-links"><li><a href="../../guide/control-from-a-script/">Controlling Studio from a script</a></li><li><a href="../../guide/command-line/">Command line equivalents</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>A test in a few lines</h2>
<p>Start the agent and the gateway and leave them running. On Windows, start each in its own terminal.</p>
<pre class="vss-code"><code>vss_agent &amp;
vss_gate --local-role=control &amp;
vss_cli control ping</code></pre>
<p>Then a script can make two ports, send a request into one and wait for the reply on the other. This one also writes the report.</p>
</div>
<div>
<pre class="vss-code"><code>from vss_control import Client

with Client.connect() as c:
    a = c.create_port("a")
    b = c.create_port("b")
    c.connect_ports(a, b, one_way=True)
    run = c.begin_run("smoke test")
    c.expect_bytes(port=b, pattern=b"OK\r\n",
                   timeout_ms=2000, send_port=a,
                   send=b"AT\r\nOK\r\n",
                   label="the device answers")
    c.end_run()
    with open("smoke.xml", "w", encoding="utf-8") as f:
        f.write(c.report_junit(run["run"]))
    c.remove_port(a)
    c.remove_port(b)</code></pre>
</div>
</div>
<p>If the bytes do not arrive in time, <code>expect_bytes</code> raises a <code>ControlError</code> whose code is <code>timeout</code>, and the report notes how the wait ended.</p>
<ul class="vss-links"><li><a href="../../guide/control-from-a-script/">Controlling Studio from a script</a></li><li><a href="../../solutions/test-automation-and-ci/">Test automation and CI</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>What it needs today</h2>
<ul class="vss-ticks">
<li>The agent, the gateway and the Python client are not in the installers yet. You build them from the source, and the tools are built by default. <span class="vss-chip vss-chip--src">Built from source only</span></li>
<li>Python 3.9 or later for the client.</li>
<li>The installed command line has the <code>control</code> commands, but they have nothing to talk to until the agent and the gateway run.</li>
</ul>
<ul class="vss-links"><li><a href="../../developers/building/">Building from source</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div>
<div>
<h2>What to know before you start</h2>
<ul class="vss-ticks">
<li>It controls one computer. Use from another computer needs TLS, which is not built. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>The ports a script makes are loopback ports inside the agent. Another program cannot open them, and the agent cannot attach a real device to one yet. They are for testing a script, or a protocol, against bytes you send yourself.</li>
<li>The ports you make in the Studio window belong to Studio, so the gateway does not see them.</li>
<li>The client is for Python. <span class="vss-chip vss-chip--plan">Other languages: Planned</span></li>
</ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Who may do what</h2>
<p>Your own programs get the role you gave the gateway when you started it. <code>monitor</code> only looks. <code>control</code> can also read the bytes, make, link and remove ports, and send bytes.</p>
<p>To let another program in with less, make it a token for the role you choose. A token is shown once and lasts 30 days unless you set a time from 1 to 365 days, and you revoke it whenever you like.</p>
</div>
<div>
<h2>A record of what happened</h2>
<p>The gateway writes an audit log of every connection, every refusal and every change to a port, with who made it. It never records the bytes or a token.</p>
<p>A command checks that the log is whole, with no record removed or changed.</p>
</div>
</div>
<ul class="vss-links"><li><a href="../../guide/control-from-a-script/#let-another-program-in-with-a-token">Letting another program in with a token</a></li><li><a href="../../guide/control-from-a-script/#see-who-is-connected">See who is connected</a></li></ul>
</div></section>
