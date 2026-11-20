---
template: marketing.html
title: Quality and trust
title_tag: "Quality and trust: Virtual Serial Studio"
description: "How Virtual Serial Studio is tested and released: tests on Windows, Linux and macOS, memory and thread checks, fuzzing, signed releases and no telemetry."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Quality and trust</p>
<h1>Quality and trust</h1>
<p class="icl-lead">What is done to keep Studio reliable, and what you can check for yourself. It is a description of how Studio is tested and released. It does not say that Studio suits any particular use.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2 id="tests">How it is tested</h2>
<ul class="vss-ticks">
<li>Studio and the command line are built and tested on Windows, Linux and macOS every night.</li>
<li>The tests move data through real serial pairs, live network connections and separate programs, and not only through the code on its own.</li>
<li>The tests also run under tools that catch memory mistakes and data races: AddressSanitizer, UBSan and ThreadSanitizer.</li>
<li>Any program that reads a file or network data from outside is fed random and broken input on every merge and every night (fuzzing). A crash fails the run.</li>
<li>The build fails if test coverage of the libraries drops below a set level, and if any compiler warns.</li>
<li>A test that cannot run on a computer says so. It never reports a pass.</li>
</ul>
</div>
<div>
<h2 id="platforms-tested">Where it is tested</h2>
<ul class="vss-ticks">
<li>Ubuntu 24.04 and 26.04, Windows Server 2025, and macOS 15 and 26 on Apple silicon and Intel, as hosted runs on GitHub.</li>
<li>The Windows driver is also tested in Windows 10 and Windows 11 virtual machines, and Studio on Ubuntu 26.04 in a virtual machine, on the maintainer's own computer.</li>
<li>A hosted run shows that Studio builds and passes its tests on a real installation of that system. It does not show that anyone has watched it on a real screen.</li>
<li>Every run, and its result, is public on GitHub.</li>
</ul>
<ul class="vss-links"><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/actions">The runs on GitHub</a></li><li><a href="../guide/limits/">What this build cannot do</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2 id="static-analysis">How the code is checked</h2>
<ul class="vss-ticks">
<li>The code is scanned every night by GitHub's CodeQL and by other analysers, including Microsoft's for the Windows build.</li>
<li>Each finding is an alert in GitHub's code scanning, so it is a record that can be read and closed.</li>
<li>None of them blocks a change by itself. A failed nightly run opens an issue.</li>
</ul>
</div>
<div>
<h2 id="signed-releases">Signed releases</h2>
<ul class="vss-ticks">
<li>Every file in a release has a signature, and a checksum file lists every file.</li>
<li>Every release has a software bill of materials that lists Qt, Boost, Boost.VirtualSerial, Monocypher and the bundled fonts.</li>
<li>Every package has a GitHub build-provenance attestation, which ties it to the build that made it.</li>
<li>A release is not created if a file is missing from the checksum list, a checksum is wrong, a file has no signature or there is no bill of materials.</li>
<li>You check against a key whose fingerprint you read in the repository, and not against one that arrives with the download.</li>
</ul>
<ul class="vss-links"><li><a href="../downloads/#verify">How to verify a download</a></li><li><a href="../security/#releases">How releases are signed</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2 id="no-telemetry">No telemetry</h2>
<ul class="vss-ticks">
<li>Studio sends nothing about you or your computer: no telemetry, no usage data, no crash reports and no account.</li>
<li>The one request Studio makes by itself is the update check. It asks for one signed file, with no query, no cookie and no body. It is off until you turn it on, and a build that has no key to check the file does not make it.</li>
<li>A licence is checked on your computer and never sent anywhere. There is no activation server.</li>
<li>A support bundle is a file that you save and send yourself. It never holds the bytes of your traffic.</li>
</ul>
<ul class="vss-links"><li><a href="../privacy/">Privacy</a></li><li><a href="../security/#network">What the program sends over a network</a></li></ul>
</div>
<div>
<h2 id="accessibility">Accessibility</h2>
<ul class="vss-ticks">
<li>Every control has a name and a role for screen readers, can be reached from the keyboard, shows where the focus is, and is at least 24 by 24 pixels.</li>
<li>Every dialog works at 200 percent text, in both the light and the dark theme.</li>
<li>A test fails the build if a control breaks these rules, and the colours are checked for contrast on every pull request.</li>
</ul>
<ul class="vss-links"><li><a href="../accessibility/">Accessibility</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint icl-cta"><div class="icl-wrap">
<h2>Found a problem?</h2>
<p class="icl-lead">Report a fault on GitHub, and a security problem privately.</p>
<div class="icl-actions"><a class="icl-button" href="../support/">Support</a><a href="../security/#report">Report a security problem</a></div>
</div></section>
