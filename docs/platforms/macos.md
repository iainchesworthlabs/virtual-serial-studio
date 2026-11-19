---
template: marketing.html
title: macOS
title_tag: "macOS: Virtual Serial Studio"
description: "Install Virtual Serial Studio on macOS from the disk image. What it needs, why macOS asks you to confirm, how virtual ports appear, and what is untried."
---
<!-- OWNER DECISION: when the macOS packages are signed and notarised. They are not today (SIGN_MACOS is false), so the page says macOS asks you to confirm. Signing needs the owner's Apple Developer ID certificate and notary credentials (docs/releasing.md). -->
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Platforms</a> / macOS</p>
<h1>macOS</h1>
<p class="icl-lead">Studio and <code>vss_cli</code> for Apple silicon Macs. Virtual ports are pseudo terminals, so nothing is installed beyond the app.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../products/drivers/">Virtual ports</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Install</h2>
<p>There are two files for macOS in each release. Each has a signature, and the Downloads page says how to check it.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">File</th><th scope="col">What it is</th></tr></thead>
<tbody>
<tr><th scope="row"><code>virtual-serial-studio-&lt;tag&gt;-macos-arm64.dmg</code></th><td>A disk image with <code>vss_gui.app</code>, an Applications link to drag it onto, <code>bin/vss_cli</code> and the licence texts.</td></tr>
<tr><th scope="row"><code>virtual-serial-studio-&lt;tag&gt;-macos-arm64.tar.gz</code></th><td>The same files, for a person who prefers an archive.</td></tr>
</tbody></table></div>
<ol>
<li>Open the disk image and drag <code>vss_gui.app</code> onto the Applications link. Eject the image.</li>
<li>The command line tool is not inside the app, because a file added to the app afterwards breaks its seal. To put it on your path, copy it out of the mounted image:</li>
</ol>
<pre class="vss-code"><code>sudo install -m 755 "/Volumes/Virtual Serial Studio/bin/vss_cli" /usr/local/bin/vss_cli</code></pre>
<p>To remove Studio, drag <code>vss_gui.app</code>, and <code>vss_cli</code> if you copied it, to the Bin. The image installs no driver, kernel extension, system extension, launch daemon or privileged helper.</p>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Requirements</h2>
<ul class="vss-ticks">
<li>An Apple silicon Mac. There is no Intel build in the release.</li>
<li>The release is built on macOS 26. The platform matrix also builds and runs the tests on macOS 15.</li>
</ul>
<h2>Limits</h2>
<ul class="vss-ticks">
<li>The macOS files are for Apple silicon only, and they are not signed or notarised, so macOS asks you to confirm before it opens Studio.</li>
<li>Studio's "Start when I sign in" is not available on macOS yet.</li>
<li>Studio can see that a program has opened a port on Linux and for the Windows driver, and not on macOS. A port there reads Open and no more.</li>
</ul>
</div>
<div>
<h2>Opening an app that is not signed</h2>
<p>The first time, macOS refuses to open <code>vss_gui</code> because the developer cannot be verified. Try to open it once. Then open System Settings, Privacy &amp; Security, and choose Open Anyway beside the message about <code>vss_gui</code>. On macOS 14 and earlier, Control-click the app, choose Open, then Open again.</p>
<p>Removing the quarantine flag also works:</p>
<pre class="vss-code"><code>xattr -dr com.apple.quarantine /Applications/vss_gui.app</code></pre>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>How virtual ports appear</h2>
<p>A virtual port is a pseudo terminal, <code>/dev/ttysNNN</code>. Studio holds one side and keeps the other open, so the port exists before a program connects. <code>vss_cli vport</code> makes stable names in <code>/tmp/vss</code>, with mode 0700. The links go when the process ends, and macOS empties <code>/tmp</code> at restart.</p>
<ul class="vss-ticks">
<li>A program that takes a typed path works, with the path of the port or of a stable link.</li>
<li>A program that lists ports through IOKit does not show the port. <code>ls /dev/cu.* /dev/tty.*</code> does not list it either, and pyserial does not list it.</li>
<li>macOS refuses 921600 baud on a pseudo terminal. Studio takes line rates up to 12000000 baud, but the system and the device decide what is possible, and a refused rate is reported when it is applied.</li>
</ul>
</div>
<div>
<h2>What macOS asks</h2>
<ul class="vss-ticks">
<li>From macOS 15, macOS asks once for Local Network access the first time Studio uses a network endpoint.</li>
<li>The Application Firewall asks once about incoming connections when Studio opens a listener. If you choose Don't Allow, the fix is two <code>sudo socketfilterfw</code> commands that you run yourself. Studio does not run them.</li>
</ul>
<h2>What has not been tried</h2>
<ul class="vss-ticks">
<li>The macOS build is compiled and tested only on GitHub-hosted macOS runners. It has not been run on a Mac that anyone looks at.</li>
<li>The graphical interface has not been seen on a real display.</li>
<li>Gatekeeper behaviour has not been checked.</li>
<li>No third-party serial program has been tried against a macOS virtual port.</li>
</ul>
</div>
</div>
<ul class="vss-links"><li><a href="../">Platforms</a></li><li><a href="../../guide/virtual-ports-on-linux-and-macos/">Virtual ports on Linux and macOS</a></li><li><a href="../../guide/#when-the-operating-system-asks-or-says-no">When the operating system asks</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div></section>
