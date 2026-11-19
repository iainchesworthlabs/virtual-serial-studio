---
template: marketing.html
title: Linux
title_tag: "Linux: Virtual Serial Studio"
description: "Install Virtual Serial Studio on Linux from a deb, an rpm or an archive. What it needs, how virtual ports appear, and what is known not to work."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Platforms</a> / Linux</p>
<h1>Linux</h1>
<p class="icl-lead">Studio and <code>vss_cli</code> for x86-64 Linux. Virtual ports are pseudo terminals, so no driver or kernel module is installed.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../products/drivers/">Virtual ports</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Install</h2>
<p>There are three files for Linux in each release. Each has a signature, and the Downloads page says how to check it.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">File</th><th scope="col">What it is</th></tr></thead>
<tbody>
<tr><th scope="row"><code>virtual-serial-studio_&lt;version&gt;-1_amd64.deb</code></th><td>For Debian and Ubuntu. The programs and the Qt libraries go in <code>/opt/virtual-serial-studio</code>, with links <code>/usr/bin/vss_gui</code> and <code>/usr/bin/vss_cli</code>, a launcher entry, an icon and the licence texts.</td></tr>
<tr><th scope="row"><code>virtual-serial-studio-&lt;version&gt;-&lt;release&gt;.x86_64.rpm</code></th><td>The same files, for Fedora and Red Hat.</td></tr>
<tr><th scope="row"><code>virtual-serial-studio-&lt;tag&gt;-linux-x86_64.tar.gz</code></th><td>The programs <code>bin/vss_gui</code> and <code>bin/vss_cli</code>, the Qt libraries and plug-ins, and the licence texts, to unpack anywhere.</td></tr>
</tbody></table></div>
<pre class="vss-code"><code>sudo apt install ./virtual-serial-studio_&lt;version&gt;-1_amd64.deb
sudo dnf install ./virtual-serial-studio-&lt;version&gt;-&lt;release&gt;.x86_64.rpm</code></pre>
<p>The packages add no kernel module, udev rule, polkit action, group change or maintainer script. Removing a package removes its files and nothing else. Qt is bundled, so a Qt fix reaches you with a new Studio package and not with your distribution's update.</p>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Requirements</h2>
<ul class="vss-ticks">
<li>x86-64.</li>
<li>glibc 2.42 or newer and the C++ library from GCC 16. The deb and the rpm declare both, so a system below them refuses to install.</li>
<li>The system libraries that Qt's <code>xcb</code> plug-in loads. The deb and rpm depend on them, so installing the package installs them. For the archive on Ubuntu, install <code>libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0</code>.</li>
<li><code>pkexec</code> is recommended. Studio uses it to offer to add you to the serial group.</li>
</ul>
</div>
<div>
<h2>Limits</h2>
<ul class="vss-ticks">
<li>The Linux files are for x86-64 and need glibc 2.42 or newer and the C++ library from GCC 16, which is Ubuntu 26.04 or later, so on Ubuntu 24.04 the packages refuse to install and the archive does not start.</li>
<li>The package carries the <code>xcb</code> Qt platform plug-in only, and there is no Wayland plug-in. Without the system's xcb libraries <code>vss_gui</code> stops at start with "Could not load the Qt platform plugin xcb".</li>
<li>Studio can tell whether a program has a pseudo terminal open on Linux. Programs that list ports do not list the pseudo terminal.</li>
</ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>How virtual ports appear</h2>
<p>A virtual port is a pseudo terminal, <code>/dev/pts/N</code>. Studio holds one side and keeps the other open, so the port exists before a program connects. <code>N</code> changes on every run, so <code>vss_cli vport</code> makes symbolic links with stable names, in <code>$XDG_RUNTIME_DIR/vss</code> in a normal session.</p>
<pre class="vss-code"><code>vss_cli vport --name=ttyVSS0,ttyVSS1 --link=ttyVSS0:ttyVSS1</code></pre>
<ul class="vss-ticks">
<li>A program that takes a typed path works: picocom, minicom, screen, socat, pyserial with a path, and Qt with a name.</li>
<li>A program that offers a list of detected ports does not list the port, because pyserial and Qt read lists that have no entry for a pseudo terminal.</li>
<li>The kernel's pseudo terminal has no modem lines, so DTR, RTS, CTS and DSR do not reach a program.</li>
</ul>
</div>
<div>
<h2>Serial devices and the system</h2>
<ul class="vss-ticks">
<li>Studio opens a device with the group that already owns it: <code>dialout</code> on Debian, Ubuntu and Fedora, <code>uucp</code> on Arch. If you are not in the group, the Setup check shows <code>sudo usermod -aG dialout &lt;user&gt;</code> and, where <code>pkexec</code> and <code>usermod</code> exist, a button that runs it.</li>
<li>If AppArmor or SELinux blocks Studio, the Setup check prints the lines to add. The packages ship no profile or policy.</li>
<li>Firewall rules need root. Without <code>ufw</code> or <code>firewalld</code>, Studio cannot tell whether a listener is blocked, and the Setup check says unknown.</li>
</ul>
</div>
</div>
<ul class="vss-links"><li><a href="../../guide/virtual-ports-on-linux-and-macos/">Virtual ports on Linux and macOS</a></li><li><a href="../../guide/#when-the-operating-system-asks-or-says-no">When the operating system asks</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>What is tested</h2>
<p>The platform matrix builds Studio and runs the tests on Ubuntu 24.04 and 26.04 runners, and on an Ubuntu 26.04 guest on the owner's machine. The release workflow checks the packages: it installs the deb in an Ubuntu 26.04 container and the rpm in a Fedora 44 container, runs <code>vss_cli selftest</code>, and starts <code>vss_gui --version</code> under Xvfb. A Fedora system with SELinux enforcing has not been tried.</p>
</div>
<ul class="vss-links"><li><a href="../">Platforms</a></li><li><a href="../../developers/platforms/">Platforms and the test matrix</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div></section>
