---
template: marketing.html
title: Platforms
title_tag: "Platforms: Virtual Serial Studio"
description: Virtual Serial Studio runs on Windows, macOS and Linux. What each system gets, what is tested on it, and how virtual ports appear there.
---
<!-- OWNER DECISION: how this page states the result of the platform matrix. The page names the workflow and does not copy a result. On 20 November 2026 the scheduled runs of the Platform matrix workflow on develop had failed since 16 November, and docs/platforms.md says the last run in which every cell passed was on 5 November. Decide whether to say anything about the current state here. -->
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Platforms</p>
<h1>Windows, macOS and Linux</h1>
<p class="icl-lead">Studio and <code>vss_cli</code> run on all three. Each system has its own packages, its own kind of virtual port and its own limits, which are listed exactly on its page.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<h2 class="icl-sr">The three systems</h2>
<div class="vss-table"><table>
<thead><tr><th scope="col">System</th><th scope="col">Packages</th><th scope="col">Virtual ports</th><th scope="col">Built and run in the platform matrix</th></tr></thead>
<tbody>
<tr><th scope="row"><a href="windows/">Windows</a></th><td>Installer and zip, for x86-64</td><td><code>COM</code> ports from the Studio driver, which is test-signed</td><td>Windows Server 2025 runner. A Windows 10 guest and a Windows 11 guest, with the driver.</td></tr>
<tr><th scope="row"><a href="macos/">macOS</a></th><td>Disk image and archive, for Apple silicon. Not signed or notarised.</td><td>Pseudo terminals, <code>/dev/ttysNNN</code></td><td>macOS 15 and 26 runners, Apple silicon and Intel.</td></tr>
<tr><th scope="row"><a href="linux/">Linux</a></th><td>Archive, deb and rpm, for x86-64. Need glibc 2.42 or newer.</td><td>Pseudo terminals, <code>/dev/pts/N</code>, with stable names from <code>vss_cli vport</code></td><td>Ubuntu 24.04 and 26.04 runners, and an Ubuntu 26.04 guest.</td></tr>
</tbody></table></div>
<p>The release files and their checks are on <a href="../downloads/">Downloads</a>.</p>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>What is tested</h2>
<p>The platform matrix builds the GUI and runs the tests on each system and version in the table, every night, on request, and on a change to the matrix itself. A test that cannot run reports <code>not_run</code>, never a pass. The tests are:</p>
<ul class="vss-ticks">
<li>The unit, system and GUI test suites.</li>
<li>A separate process that opens two virtual ports and exchanges 4 KB each way through the hub.</li>
<li>A split and a join topology, loaded from JSON, that route every byte.</li>
<li><code>vss_cli selftest</code> and <code>vss_gui --selftest</code>.</li>
<li>A release package unpacked with no build environment, with <code>vss_cli selftest</code> and <code>vss_gui --version</code> run from it.</li>
<li>What the system and common tools show for a virtual port, on Linux and macOS.</li>
</ul>
</div>
<div>
<h2>What a hosted cell does not show</h2>
<p>A hosted macOS or Windows cell proves the build and the tests on a real installation of that system. It does not prove that anyone has seen the program on a real display. The Windows driver tests run in guests on the owner's machine, and are not a matrix cell.</p>
<p>The result of each cell is in the run list of the <a href="https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/platform-matrix.yml">Platform matrix workflow</a> on GitHub. <a href="../developers/platforms/">Platforms and the test matrix</a> in the developer pages lists the cells and the tests in full.</p>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Where to go next</h2>
<p>Each page says what the system needs, how to install, and what is known not to work.</p>
</div>
<ul class="vss-links"><li><a href="windows/">Windows</a></li><li><a href="macos/">macOS</a></li><li><a href="linux/">Linux</a></li><li><a href="../products/drivers/">Drivers and virtual ports</a></li><li><a href="../downloads/">Downloads</a></li><li><a href="../guide/limits/#what-this-build-cannot-do">What this build cannot do</a></li></ul>
</div></section>
