---
template: marketing.html
title: Drivers and virtual ports
title_tag: "Drivers and virtual ports: Virtual Serial Studio"
description: How Studio makes virtual serial ports. A COM port driver on Windows, which is test-signed today, and pseudo terminals with stable names on Linux and macOS.
---
<!-- OWNER DECISION: how a person gets and installs the public certificate of the certificate authority that signs the driver (Iain Chesworth Labs). The installer's message points to bin\driver\trust\install-trust.ps1 and says to use the public certificates of the authority that signed the build. Where a person downloads those certificates is not stated in the repository. -->
<!-- OWNER DECISION: when the Windows driver is signed by Microsoft. Nothing signs the kernel-mode driver yet, and who holds the certificate for attestation signing is not decided (docs/windows-com-driver.md, decision point 3). -->
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Products</a> / Drivers</p>
<h1>Drivers and virtual ports</h1>
<p class="icl-lead">A virtual port has to look like a real one to the program that opens it. On Windows that takes a driver. On Linux and macOS it takes none.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../platforms/">Platforms</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Windows: a COM port driver</h2>
<p>On Windows a <code>COM</code> port is a kernel device, not a name, and nothing in user space can make one. Studio ships a driver that makes <code>COM</code> ports for it, so you do not need com0com.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col"></th><th scope="col"><code>vsscom_um</code>, user mode</th><th scope="col"><code>vsscom</code>, kernel mode</th></tr></thead>
<tbody>
<tr><th scope="row">In the Windows packages</th><td>Yes, with its helper service <code>vsscom_helper</code>, in the installer and the zip. <span class="vss-chip vss-chip--ok">In the packages</span></td><td>No. It is built in continuous integration for test machines.</td></tr>
<tr><th scope="row">Signing</th><td>The catalog is signed with a certificate from the certificate authority of Iain Chesworth Labs. Windows loads it only where that authority is trusted.</td><td>Not signed. Windows loads kernel code only if Microsoft signed it or test signing is on.</td></tr>
<tr><th scope="row">How ports are made</th><td>The helper service, which runs as SYSTEM, makes a software device for each port, for ordinary users.</td><td>A control device makes them on demand.</td></tr>
</tbody></table></div>
<ul class="vss-ticks">
<li>Studio uses <em>Best available</em> by default: the Studio driver if it is installed, then com0com, then an in-process loopback that only Studio can open.</li>
<li>You can choose a COM number from 1 to 255, change it later, and a number another device holds is refused. Programs that build the name as <code>"COM%d"</code> need a number from 1 to 9.</li>
<li>One user can hold 16 ports, and 32 can exist in all.</li>
<li>A port tells Studio when a program opens it, and what baud rate and framing the program set.</li>
<li>Faults from a mock fixture's dialog reach the program that has a driver port open: a parity error, a framing error, an overrun, a break, a hang-up, or taking the port back.</li>
<li>The driver is 64-bit. The release is for x86-64 Windows only. An Arm driver is built and has not been tried in a guest. A 32-bit program reaches the driver like any <code>COM</code> port.</li>
</ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>What Windows needs from you</h2>
<p>The driver is test-signed. Windows accepts it only on a computer where the certificate authority of Iain Chesworth Labs has been installed. On any other computer the installer installs Studio and leaves the driver out, and says why.</p>
<p>The installer never adds a certificate and never turns on test signing. To trust the authority, the installer's message points to <code>bin\driver\trust\install-trust.ps1</code>, which you run with the public certificates of the authority that signed the build. On a test machine you can turn on test signing instead. Then choose <strong>Set up Virtual Serial Studio</strong> in Studio, which installs or repairs the driver and the helper service.</p>
<p>The installer records the result as <code>installed</code>, <code>skipped</code>, <code>not-trusted</code> or a failure with its code, in <code>driver-setup.log</code> and in its Apps &amp; features entry, so a deployment tool can read it.</p>
</div>
<div>
<h2>Signed by Microsoft</h2>
<p>A driver that Microsoft has signed installs with no step by hand. That driver does not exist yet. <span class="vss-chip vss-chip--plan">Planned</span></p>
<h2>Where it was tried</h2>
<p>The driver tests run in a Windows 10 22H2 guest and a Windows 11 guest. Driver Verifier has been run on the kernel-mode driver in the Windows 11 guest. The state of each test is in <code>drivers/windows/README.md</code> in the repository.</p>
<ul class="vss-links"><li><a href="../../platforms/windows/">Windows</a></li><li><a href="../../guide/#old-programs-on-windows">Old programs on Windows</a></li><li><a href="../../guide/#choosing-a-com-number-windows">Choosing a COM number</a></li></ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Linux and macOS: pseudo terminals</h2>
<p>A virtual port is a pseudo terminal. Studio holds one side and keeps the other open, so the port exists before any program connects. No driver, kernel module or udev rule is installed.</p>
</div>
<div class="vss-two">
<div>
<p>The name of a pseudo terminal changes on every run: <code>/dev/pts/N</code> on Linux and <code>/dev/ttysNNN</code> on macOS. A stable link is a symbolic link in a private directory that points at the current one.</p>
<pre class="vss-code"><code>vss_cli vport --name=ttyVSS0,ttyVSS1 --link=ttyVSS0:ttyVSS1</code></pre>
<p>That makes two ports with stable names and joins them like a null-modem cable. The command runs until you stop it and removes its links when it ends. A name is letters, digits, <code>.</code>, <code>_</code> and <code>-</code>, up to 64 characters.</p>
</div>
<div>
<ul class="vss-ticks">
<li>The link directory is <code>$VSS_VPORT_DIR</code>, then <code>$XDG_RUNTIME_DIR/vss</code>, then <code>/run/vss</code> and <code>/tmp/vss</code> on Linux, or <code>/tmp/vss</code> on macOS. It is made with mode 0700.</li>
<li>A program that takes a path works: picocom, minicom, screen, socat, Boost.Asio, pyserial with a path, and Qt's <code>QSerialPort</code> with a name.</li>
<li>A program that offers a list of detected ports does not list a pseudo terminal. pyserial and Qt read lists that have no entry for one.</li>
<li>On Linux a pseudo terminal has no modem lines, so DTR, RTS, CTS and DSR do not reach a program. Studio shows its own state for them.</li>
<li>On macOS a program that lists ports through IOKit does not see the port, and there is no <code>/dev/cu.*</code> name.</li>
</ul>
</div>
</div>
<p>A driver that makes a port every program lists is not planned for Linux or macOS. It would need a kernel module on Linux, or an Apple entitlement and a driver on macOS.</p>
<ul class="vss-links"><li><a href="../../guide/virtual-ports-on-linux-and-macos/">Virtual ports on Linux and macOS</a></li><li><a href="../../platforms/linux/">Linux</a></li><li><a href="../../platforms/macos/">macOS</a></li></ul>
</div></section>
