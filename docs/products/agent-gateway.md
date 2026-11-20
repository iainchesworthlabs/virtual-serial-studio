---
template: marketing.html
title: Agent, gateway and Python client
title_tag: "Agent, gateway and Python client: Virtual Serial Studio"
description: "vss_agent keeps ports running for one user, vss_gate lets a test script control them on one computer, and vss_control is the Python client. Built from source."
---
<!-- OWNER DECISION: when the agent and the gateway go into the packages. docs/agent.md says the owner has decided that vss_agent and vss_gate will be in the installers and packages, and gives no release. Until then this page says they are built from source only. -->
<section class="vss-head"><div class="icl-wrap">
<h1>Agent, gateway and Python client</h1>
<p class="icl-lead">Let a test script make ports, send bytes, wait for a reply and keep a JUnit report, on one computer. <span class="vss-chip vss-chip--src">Built from source only</span></p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Three programs</h2>
<p>None of the three is in a package yet. Build them from the source with the steps in <a href="../../developers/building/">Building</a>; the tools are built by default.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">Program</th><th scope="col">What it is for</th></tr></thead>
<tbody>
<tr><th scope="row"><code>vss_agent</code></th><td>Keeps this user's virtual ports and links running, and serves them over a pipe or socket that only this user can open. Nothing listens on the network. One agent runs for each user, and it closes every port when it stops.</td></tr>
<tr><th scope="row"><code>vss_gate</code></th><td>The control gateway. It serves the control API to this user's programs over a pipe or socket that only this user can open, and it needs a running agent. Programs may only look unless you start it with <code>--local-role=control</code>.</td></tr>
<tr><th scope="row"><code>vss_control</code></th><td>The Python client for the gateway. It needs Python 3.9 or later and nothing else, and it makes and links ports, sends bytes, waits for bytes and reads the event and traffic streams.</td></tr>
</tbody></table></div>
<p><code>vss_cli agent</code> and <code>vss_cli control</code>, which are in the packages, are the command line for the agent and the gateway. They have nothing to talk to until the two programs run. See <a href="../../guide/command-line/">Command line</a>.</p>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>How a test script drives ports</h2>
<p>A test begins a run, makes two ports and links them, sends a request into one and waits for the reply on the other. The gateway starts listening before it sends, so a fast reply cannot be missed. A wait that runs out ends with a <code>timeout</code> error, and the connection stays usable.</p>
<p>While a run is open, each wait is noted with how it ended, and <code>report_junit</code> hands back the run as JUnit XML for a CI system. A run belongs to the user or token that began it.</p>
<pre class="vss-code"><code>vss_agent &amp;
vss_gate --local-role=control &amp;
vss_cli control ping</code></pre>
<p>On Windows start each program in its own terminal.</p>
</div>
<div>
<pre class="vss-code"><code>from vss_control import Client, ControlError

with Client.connect() as c:
    a = c.create_port("a")
    b = c.create_port("b")
    c.connect_ports(a, b, one_way=True)
    run = c.begin_run("smoke test")
    c.expect_bytes(port=b, pattern=b"OK\r\n",
                   timeout_ms=2000, send_port=a,
                   send=b"AT\r\nOK\r\n",
                   label="the device answers")
    try:
        c.expect_bytes(port=b, pattern=b"never",
                       timeout_ms=200,
                       label="nothing else comes")
    except ControlError as error:
        print(error.code)   # timeout
    c.end_run()
    with open("smoke.xml", "w", encoding="utf-8") as f:
        f.write(c.report_junit(run["run"]))
    c.remove_port(a)
    c.remove_port(b)</code></pre>
</div>
</div>
<p>The same steps from a terminal are <code>vss_cli control port create</code>, <code>link</code>, <code>send</code>, <code>expect</code>, <code>run begin</code>, <code>run end</code> and <code>run report --out=smoke.xml</code>.</p>
<ul class="vss-links"><li><a href="../../guide/control-from-a-script/">Controlling Studio from a script</a></li><li><a href="../../solutions/test-automation-and-ci/">Test automation and CI</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Who may do what</h2>
<p>Every operation needs a scope, and a role is a set of scopes. The <code>monitor</code> role reads ports, links and counters and keeps a test run. The <code>control</code> role can also read the bytes, make, remove and link ports, and send bytes. Your own programs get the role you gave <code>vss_gate</code>.</p>
<p>A token lets another program connect with only the scopes you choose. It is shown once, it lives 30 days unless you give <code>--days</code> (1 to 365), and the gateway keeps only a hash of it. A token is never given on a command line. Commands take <code>--token-file</code>.</p>
<p>The gateway writes an audit log of every connection, refusal and change to a port, with who made it. It never records the bytes or a token. <code>vss_cli control audit verify</code> checks that the log is a chain of records with none removed or changed.</p>
</div>
<div>
<h2>What it does not do yet</h2>
<ul class="vss-ticks">
<li>It controls one computer. The gateway listens on a pipe or socket for this user, and, if you ask, on TCP at <code>127.0.0.1</code> and <code>::1</code>. It cannot listen on another address. <span class="vss-chip vss-chip--plan">Remote use with TLS: Planned</span></li>
<li>A port made through the gateway is a loopback port in the agent. Another program cannot open it, and the agent cannot attach a serial device to it yet.</li>
<li>The ports you make in the Studio window are Studio's own, so the gateway does not see them.</li>
<li>A token sent over the loopback TCP port crosses it in clear. That is why this listener stays on the loopback address.</li>
<li>The client is for Python. <span class="vss-chip vss-chip--plan">Other languages: Planned</span></li>
</ul>
</div>
</div>
<ul class="vss-links"><li><a href="../../developers/architecture/">Architecture</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div></section>
