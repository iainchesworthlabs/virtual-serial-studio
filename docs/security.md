---
template: marketing.html
title: Security
title_tag: "Security: Virtual Serial Studio"
description: "How to report a vulnerability in Virtual Serial Studio privately, how releases are signed, what the program sends over a network, and where its limits are."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Security</p>
<h1>Security policy and releases</h1>
<p class="icl-lead">How to report a vulnerability, how to check that a download is ours, what the program sends over a network, and where its protections stop.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2 id="report">Report a vulnerability</h2>
<p>Report it privately, not as a public issue or pull request. Use <a href="https://github.com/iainchesworthlabs/virtual-serial-studio/security/advisories/new">GitHub's private vulnerability reporting</a>. It opens an advisory that only you and the maintainer can see.</p>
<p>If you cannot use the form, for example because you have no GitHub account, open an issue that says only that you have a security matter, with no details. The maintainer will reply there to arrange a private channel.</p>
<p>What helps most is what is affected (the version from the About window, <code>vss_gui --version</code> or <code>vss_cli --version</code>, the platform, and whether it is <code>vss_gui</code>, <code>vss_cli</code> or a release file), the smallest setup file, command line or byte stream that shows it, and what someone could do with it. Please do not attach traffic captured from equipment that is not yours.</p>
<p>Studio is maintained by one person. There is no promised response time and no promised schedule for a fix. A report is read and acknowledged in the advisory. One that is confirmed is fixed on <code>develop</code> and published with the next tagged release. Please allow time for a fix to ship before you publish details, and agree it in the advisory.</p>
<p>Only the newest release receives fixes, because Studio is before version 1.0.</p>
<ul class="vss-links"><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/SECURITY.md">SECURITY.md on GitHub</a></li></ul>
</div>
<div>
<h2>What counts</h2>
<ul class="vss-ticks">
<li>A crash, hang, out-of-bounds read or other memory-safety fault that crafted bytes can cause, from a network endpoint or from a setup (JSON) file.</li>
<li>A network endpoint that accepts a peer you did not ask it to, or that exposes a port you did not share.</li>
<li>Anything that lets one program read or write another program's virtual port without being given it.</li>
<li>A flaw in the Windows COM driver, in the build and workflow files, or in what the release process publishes.</li>
</ul>
<p>The network bridges do not authenticate or encrypt, because plain TCP, RFC 2217 and UDP do neither. A hostile network is outside what they defend against. A report that depends on that is a limit of the protocol and not a vulnerability in the code.</p>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2 id="releases">How releases are signed</h2>
<ul class="vss-ticks">
<li>Every release file has a detached GPG signature, the file's name with <code>.asc</code> added.</li>
<li><code>SHA512SUMS</code> lists a checksum for every file but the signatures, and is signed as well.</li>
<li>The release holds an SPDX bill of materials, and each package has a GitHub build-provenance attestation.</li>
<li>The key is <code>Iain Chesworth Labs Releases</code>, Ed25519, created on 22 October 2026 and expiring on 21 October 2028. Its fingerprint is <code>FD13 CB8C AC83 1664 8CD9 74D0 8149 254C 4189 4DBE</code>.</li>
<li>Trust the fingerprint you read in the repository, and not a key file that arrives with a download. A signature checked against a key from the same place as the file shows only that the two agree. The key is not on a keyserver.</li>
</ul>
<p><a href="../downloads/#verify">How to verify a download</a> has the commands.</p>
</div>
<div>
<h2>What is and is not signed</h2>
<ul class="vss-ticks">
<li>The Windows driver is signed with a certificate from the certificate authority of Iain Chesworth Labs, so Windows loads it only on a computer where that authority is installed. The installer never adds a certificate and never turns on test signing. A driver signed by Microsoft is <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>The macOS app is not signed or notarised, so macOS asks you to confirm before it opens it. Check the download against the signature and the checksums first.</li>
<li>A Linux package is signed by the GPG signature on the file and nothing more.</li>
</ul>
<ul class="vss-links"><li><a href="../guide/when-the-system-asks/">When the system asks</a></li><li><a href="../guide/windows/">Windows</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="network">What the program sends over a network</h2>
<p>Nothing, except the connections you make and one optional check.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">What</th><th scope="col">When</th><th scope="col">What it carries</th></tr></thead>
<tbody>
<tr><th scope="row">A <code>tcp</code>, <code>udp</code> or <code>rfc2217</code> endpoint</th><td>Only when you make one, as a client that connects out or a listener that waits.</td><td>The bytes of that port, to the address you gave. A listener accepts only the addresses in its <code>?allow=</code> list, when it has one.</td></tr>
<tr><th scope="row">The update check</th><td>The automatic check is off until you turn it on, when Studio asks at the first start or in Settings. When it is on, Studio asks once a day. You can also ask at any time with <strong>Help, Check for updates</strong>. A build that has no key to check the file makes no request. An administrator can turn the check off with <code>"update_check": false</code> in the policy file.</td><td>One HTTPS request for one signed file, with no query, no cookie and no body. The <code>User-Agent</code> is the fixed text <code>VirtualSerialStudio</code>. Studio chooses the version for itself, so the site is never told which one you have.</td></tr>
<tr><th scope="row">An update download</th><td>Only after you say yes.</td><td>A request for the package to the project's releases on GitHub, which sees it as it sees any download.</td></tr>
<tr><th scope="row"><code>vss_cli</code></th><td>Never by itself.</td><td>It makes no update check. It opens only the endpoints you give it.</td></tr>
</tbody></table></div>
<p>There is no telemetry, no usage data, no crash report and no account. A licence is checked on the computer and never sent anywhere, because there is no activation server. A support bundle is a file you save and send yourself, and it never holds the bytes of your traffic.</p>
<ul class="vss-links"><li><a href="../privacy/">Privacy</a></li><li><a href="../quality/#no-telemetry">No telemetry</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2 id="gateway">What the gateway protects, and what it does not</h2>
<p>The control gateway, <code>vss_gate</code>, lets a test script on the same computer make ports, send bytes and wait for replies. It is built from source and is not in a package yet.</p>
<ul class="vss-ticks">
<li>It serves a named pipe on Windows or a Unix socket on Linux and macOS that only the user who started it can open, and before it reads a byte it asks the operating system who made the connection. Nothing listens on the network.</li>
<li>Programs of that user may only look unless the gateway is started with <code>--local-role=control</code>.</li>
<li>A token lets a program in with the role you choose. It is shown once, the gateway keeps only a hash of it, and it ends after 30 days unless you say otherwise. A wrong token is slowed down, and each refusal is written to an audit log, never the token.</li>
<li>The audit log can be checked with <code>vss_cli control audit verify</code>. A broken chain shows that a line was removed or changed. An intact one does not prove that nobody rewrote the whole log.</li>
</ul>
</div>
<div>
<h2>Where its protection stops</h2>
<ul class="vss-ticks">
<li>There is no TLS, so one computer cannot control another. Controlling one computer from another with TLS is <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>The optional TCP listener, off unless you give <code>--loopback-port</code>, listens at <code>127.0.0.1</code> and <code>::1</code> and nowhere else. The token crosses that connection in clear, which is acceptable on the loopback address of one computer and is the reason it cannot be put on a network.</li>
<li>Any other user of the computer can connect to that port. Without a token they get nothing.</li>
<li>The agent makes loopback ports that only its own clients can open. A port made in the Studio window is not the agent's.</li>
</ul>
<ul class="vss-links"><li><a href="../products/agent-gateway/">Agent and gateway</a></li><li><a href="../guide/control-from-a-script/">Controlling Studio from a script</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="bridges">Ports you put on a network</h2>
<p>An RFC 2217 server has no password, and nothing it carries is encrypted. The <code>?allow=</code> list keeps strangers on the same network away, but an address can be forged, and the data crosses the network as it is.</p>
</div>
<ol class="vss-steps">
<li><h3>The same computer</h3><p>Listen on <code>127.0.0.1</code>. Nothing else can reach it.</p></li>
<li><h3>An SSH tunnel</h3><p>Listen on <code>127.0.0.1</code> and forward a port from the other computer. SSH encrypts and authenticates the traffic.</p></li>
<li><h3>A VPN</h3><p>Listen on the address the VPN gives the computer, and add <code>?allow=</code> with the VPN's range.</p></li>
<li><h3>A network you control</h3><p>Listen on the interface they can reach, add <code>?allow=</code>, and allow only that port through the firewall.</p></li>
</ol>
<p>Do not listen on <code>0.0.0.0</code> on a network you do not control, and do not make the port reachable from the Internet. The network dialog and <code>vss_cli bridge</code> warn when a server can be reached from another computer and has no <code>allow=</code> list.</p>
<ul class="vss-links"><li><a href="../guide/network/#keeping-it-safe">Keeping it safe, in the guide</a></li><li><a href="../solutions/sharing-and-bridging-ports/">Sharing and bridging ports</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="supply-chain">The supply chain</h2>
<p>What the repository shows about what goes into a release.</p>
</div>
<div class="vss-two">
<ul class="vss-ticks">
<li>Each release has an SPDX bill of materials that lists Qt, Boost, Boost.VirtualSerial, Monocypher and the bundled fonts. The release script stops if a component is added to the list of third-party code and has no place where its version is pinned.</li>
<li>Every <code>uses:</code> line in the workflows names a full commit, with the version in a comment. The shared policy checks look at this on every pull request, and a workflow analyser, zizmor, reads the workflow files.</li>
<li>Dependabot proposes updates to the actions weekly, and to the vcpkg baseline and the documentation tools monthly, and waits seven days after a release before it does.</li>
</ul>
<ul class="vss-ticks">
<li>A pull request is checked against the dependency review for new or changed dependencies, and that check is one of the two the branch rules require.</li>
<li>The tools that build this site are installed from a file of hashes, so a changed package is refused.</li>
<li>The code that checks a signature is Monocypher 4.0.2, kept unchanged in the repository and tested against the published Ed25519 test vectors.</li>
<li>Each file carries a licence line, and a check reads them. <a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/THIRD_PARTY_NOTICES.md">Third-party notices</a> lists every component and its licence.</li>
</ul>
</div>
</div></section>
