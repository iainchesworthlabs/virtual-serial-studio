---
template: marketing.html
title: Quality and trust
title_tag: "Quality and trust: Virtual Serial Studio"
description: "How Virtual Serial Studio is tested, fuzzed, analysed and released: sanitizer builds, fuzz harnesses, coverage gates, signed releases and a bill of materials."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Quality and trust</p>
<h1>How it is tested and released</h1>
<p class="icl-lead">Everything on this page is in the repository, where you can read the workflow or the file that does it. It says what is run. It does not say that the program is fit for any particular use.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<ul class="vss-stats">
<li><strong>3</strong><span>sanitizers, AddressSanitizer, UBSan and ThreadSanitizer, with the tests run under them at <code>-O1</code></span></li>
<li><strong>10</strong><span>libFuzzer harnesses on the code that reads bytes from outside</span></li>
<li><strong>90% / 80%</strong><span>statement and branch coverage the build must hold for the libraries</span></li>
<li><strong>7</strong><span>hosted runners in the platform matrix: two Ubuntu, one Windows, four macOS</span></li>
</ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="tests">Tests and sanitizers</h2>
<p>The tests are driven by CTest, and there is one set of presets for each system.</p>
</div>
<div class="vss-two">
<div>
<ul class="vss-ticks">
<li>The suites are unit, system, GUI, performance and fuzz replays. The system tests carry data through real serial pairs, live TCP, UDP and RFC 2217 peers, and separate <code>vss_cli</code> processes.</li>
<li>The GUI has one test program for each area, an offscreen self-test that walks the main window and every dialog in both themes, and the same self-test again at 200 percent scale.</li>
<li>Performance is gated against the floors in <code>perf/floors.json</code>.</li>
<li>A test that cannot run on a machine reports that it did not run. It never reports a pass.</li>
<li>Warnings are errors in every build, on GCC, Clang, AppleClang and MSVC.</li>
</ul>
</div>
<div>
<ul class="vss-ticks">
<li>The pull request gate builds with GCC and runs the tests, and runs the tests again under AddressSanitizer and UBSan. When threading code changes it runs ThreadSanitizer too. The merge queue adds Windows with MSVC and the GUI build.</li>
<li>Every night it also builds with the newest GCC and Clang, runs GCC with AddressSanitizer and UBSan, runs ThreadSanitizer with Clang again on the day's merged code, and runs MSVC with AddressSanitizer.</li>
<li>Every sanitizer build is optimised at <code>-O1</code>, so stack traces stay readable and the run time stays practical.</li>
</ul>
<ul class="vss-links"><li><a href="../developers/testing/">Testing</a></li><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/ci.md">Continuous integration</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="fuzzing">Fuzzing</h2>
<p>Each reader of bytes that come from outside has a libFuzzer harness, a seed corpus and the inputs of past crashes.</p>
</div>
<div class="vss-two">
<div>
<p>The ten harnesses read a saved setup, an endpoint address, the bytes of a connection to the agent, a plugin manifest, a plugin message, a plugin's settings, a response timing rule set, the bytes of a connection to the gateway, a licence file and an update feed. Each must refuse the input with a reason or accept it without a crash, and what it accepts must write back and read again as the same value.</p>
</div>
<div>
<ul class="vss-ticks">
<li>Every build replays the seed corpus and the regression inputs as tests, and the sanitizer build replays them under AddressSanitizer and UBSan.</li>
<li>Every ready pull request replays them under libFuzzer.</li>
<li>Every push to <code>develop</code> fuzzes each target for 60 seconds, and every night for 10 minutes.</li>
<li>A crash fails the run, keeps the input, and at night opens an issue. The input goes into the regression folder with the fix.</li>
</ul>
<ul class="vss-links"><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/docs/fuzzing.md">Fuzzing</a></li><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/.github/workflows/fuzz.yml">The Fuzz workflow</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="coverage">Coverage</h2>
<p>Coverage is measured with LLVM profile instrumentation on every merge to <code>develop</code>.</p>
</div>
<ul class="vss-ticks">
<li>The libraries have a gate of 90 percent of statements and 80 percent of branches. The build fails below either.</li>
<li>The GUI has its own gate, set a little below what it measured when the gate was added, so a change cannot lose ground. It is raised as tests are added and is not lowered.</li>
<li>The coverage report lists every file and marks the ones outside the gated set.</li>
</ul>
<ul class="vss-links"><li><a href="../developers/testing/">Coverage in Testing</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="static-analysis">Static analysis</h2>
<p>Findings go to code scanning on GitHub as alerts, so each one is a record that can be read and closed.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">Analysis</th><th scope="col">What it looks at</th><th scope="col">When</th></tr></thead>
<tbody>
<tr><th scope="row">CodeQL</th><td>The C++ code, the Python tools and the workflow files</td><td>Every night on <code>develop</code></td></tr>
<tr><th scope="row">MSVC code analysis</th><td>The Windows build, with the GUI and the tests</td><td>Every night on <code>develop</code></td></tr>
<tr><th scope="row">clang-tidy and cppcheck</th><td>The libraries, the command line, the GUI and the tests. A pull request is checked on the files it changes.</td><td>Every night on <code>develop</code>, and on a pull request that changes code</td></tr>
<tr><th scope="row">zizmor</th><td>The workflow files</td><td>Every pull request, and every night</td></tr>
</tbody></table></div>
<p>None of these gates a merge by itself. The alerts are the record, and a failed nightly run opens an issue.</p>
<ul class="vss-links"><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/.github/workflows/codeql.yml">CodeQL workflow</a></li><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/.github/workflows/static-analysis.yml">Static analysis workflow</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="signed-releases">Signed releases</h2>
<p>Each release can be checked against a key that you read in the repository, and not against one that arrives with the download.</p>
</div>
<div class="vss-two">
<ul class="vss-ticks">
<li>Every file has a detached GPG signature, a <code>.asc</code> beside it.</li>
<li><code>SHA512SUMS</code> lists every file but the signatures, and is signed too.</li>
<li>Each release holds an SPDX bill of materials, <code>sbom.spdx.json</code>, that lists Qt, Boost, Boost.VirtualSerial, Monocypher and the bundled fonts.</li>
<li>Each package has a GitHub build-provenance attestation.</li>
</ul>
<div>
<ul class="vss-ticks">
<li>Before a release is created the workflow stops if a file is missing from <code>SHA512SUMS</code>, a sum is wrong, a file has no signature, or there is no bill of materials.</li>
<li>The fingerprint of the release key is printed in <code>SECURITY.md</code> in the repository and on the Downloads page.</li>
</ul>
<ul class="vss-links"><li><a href="../downloads/#verify">How to verify a download</a></li><li><a href="../security/">Security</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="platforms-tested">Platforms tested</h2>
<p>The platform matrix builds Studio and runs the tests on seven hosted runners every night, on request, and on a change to the matrix itself.</p>
</div>
<ul class="vss-ticks">
<li>The runners are Ubuntu 24.04 and 26.04, Windows Server 2025, and macOS 15 and 26 on Apple silicon and Intel.</li>
<li>The Windows driver is also tested in Windows 10 and Windows 11 guests, and Ubuntu 26.04 in a guest, on the owner's computer. Those are not hosted runs.</li>
<li>A hosted runner shows that the program builds and the tests pass on a real installation of that system. It does not show that anyone has seen the program on a real display.</li>
<li>The result of each run is on GitHub, in the run list of the Platform matrix workflow.</li>
</ul>
<ul class="vss-links"><li><a href="../downloads/">Downloads</a></li><li><a href="https://github.com/iainchesworthlabs/virtual-serial-studio/actions/workflows/platform-matrix.yml">The Platform matrix workflow</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="no-telemetry">No telemetry</h2>
<p>This is a rule of the project, and the code follows it.</p>
</div>
<ul class="vss-ticks">
<li>The rule for contributors is no network access from the window, no telemetry and no analytics.</li>
<li>The one request Studio makes by itself is the update check: one request for a signed file, with no query, no cookie and no body. It is off until you turn it on, and a build that has no key to check the file does not make it.</li>
<li>No licence is sent anywhere. There is no activation server, no usage data, no crash report and no account.</li>
<li>A support bundle is a file that you save and send yourself. It never holds the bytes of your traffic.</li>
</ul>
<ul class="vss-links"><li><a href="../privacy/">Privacy</a></li><li><a href="../security/">Security</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2 id="accessibility">Accessibility</h2>
<p>The rules are checked by a test, and the colours by a script.</p>
</div>
<ul class="vss-ticks">
<li>Every control has an accessible name and role, can be reached from the keyboard, shows a visible focus ring, and is at least 24 by 24 pixels. Every dialog works at 200 percent text.</li>
<li>The self-test fails on a focusable control with no name, a target under 24 by 24, a control the keyboard cannot reach, or text that is clipped.</li>
<li>Every pair of colours that carries meaning is checked against WCAG 2.2 contrast on every pull request: 4.5 to 1 for text, and 3 to 1 for borders and the focus ring.</li>
</ul>
<ul class="vss-links"><li><a href="../accessibility/">Accessibility</a></li></ul>
</div></section>
