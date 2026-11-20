---
template: marketing.html
title: Windows
title_tag: "Windows: Virtual Serial Studio"
description: "Install Virtual Serial Studio on Windows with the installer or the zip. What it needs, how COM ports appear, the test-signed driver, and the limits."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Platforms</a> / Windows</p>
<h1>Windows</h1>
<p class="icl-lead">Studio and <code>vss_cli</code> for x86-64 Windows, with a driver that makes <code>COM</code> ports other programs can open.</p>
<div class="icl-actions"><a class="icl-button" href="../../downloads/">Download</a><a class="icl-button icl-button--ghost" href="../../products/drivers/">Drivers</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Install</h2>
<p>There are two files for Windows in each release. Each has a signature, and the Downloads page says how to check it.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">File</th><th scope="col">What it is</th></tr></thead>
<tbody>
<tr><th scope="row"><code>virtual-serial-studio-&lt;tag&gt;-windows-x86_64.exe</code></th><td>The installer. It installs for all users into <code>%ProgramFiles%\Virtual Serial Studio</code> and asks for administrator rights once, when it starts. It holds Studio, <code>vss_cli</code>, the Qt libraries, the licence texts, and the driver with its helper service. There is no per-user mode.</td></tr>
<tr><th scope="row"><code>virtual-serial-studio-&lt;tag&gt;-windows-x86_64.zip</code></th><td>The same files, to run without administrator rights. The driver, its helper service and the set-up scripts are in <code>bin/driver</code>.</td></tr>
</tbody></table></div>
<ul class="vss-ticks">
<li>The installer has a page for the driver. Nothing installed offers to install it. An older driver offers to update it, and says that open ports are closed. The same version leaves it as it is. A newer one is left alone.</li>
<li>A page with a box that is not ticked puts the program folder on the system path, so that <code>vss_cli</code> runs from any terminal. The folder also holds the Qt libraries, which is why the box is not ticked.</li>
<li>A silent install, <code>/S</code>, installs the driver when there is none and adds nothing to the path unless it is given <code>/ADDTOPATH=1</code>. <code>/DRIVER=0</code> leaves the driver and the helper service alone, <code>/REPAIRDRIVER=1</code> installs the driver again, and <code>/D=&lt;folder&gt;</code> installs elsewhere.</li>
<li>Before an upgrade or an uninstall the installer closes <code>vss_gui.exe</code> and <code>vss_cli.exe</code> if they run from the install folder. Studio keeps the running setup, so its ports start again the next time you open it.</li>
<li>An uninstall removes the driver, the helper service, the path entry and each signed-in user's start-at-sign-in entry. It keeps your settings and saved setups.</li>
</ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>Requirements</h2>
<ul class="vss-ticks">
<li>Windows on x86-64. There is no Arm build.</li>
<li>Administrator rights to run the installer. Studio itself starts without them. Every program has a manifest that asks for the rights of the person who starts it.</li>
<li>To use the driver, a computer that trusts the certificate authority of Iain Chesworth Labs, as below.</li>
<li>Studio is built against Qt 6.12, and the Qt libraries are in the package.</li>
</ul>
</div>
<div>
<h2>Limits</h2>
<ul class="vss-ticks">
<li>The Windows files are for x86-64 only; there is no Arm build.</li>
<li>The Windows driver is test-signed with a certificate from the certificate authority of Iain Chesworth Labs, so Windows accepts it only on a computer where that authority has been installed; on any other computer the installer installs Studio and leaves the driver out.</li>
<li>Windows names a port above <code>COM9</code> as <code>\\.\COM12</code>. A program that builds the name as <code>"COM%d"</code> cannot open it, so give it a number from 1 to 9.</li>
<li>One user can hold 16 driver ports, and 32 can exist in all.</li>
</ul>
<p>The driver is described on <a href="../../products/drivers/">Drivers and virtual ports</a>. A Microsoft-signed driver is <span class="vss-chip vss-chip--plan">Planned</span></p>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>How virtual ports appear</h2>
<p>A virtual port is a <code>COM</code> port with a number, such as <code>COM7</code>. Other programs open it as they open any serial port, and Device Manager lists it. You can ask for a number when you make the port, or let Studio choose a free one.</p>
<p>With <em>Best available</em>, Studio uses the Studio driver if it is installed, then com0com if it is there, and otherwise an in-process loopback whose path is <code>loopback:&lt;name&gt;</code> that only Studio can open. Tools, Setup check says which piece is missing, and <strong>Set up Virtual Serial Studio</strong> installs or repairs the driver and the helper service.</p>
<p>Studio's driver ports belong to Studio. They are removed when it quits, or if it stops without warning, and a program that has one open loses it at that moment. Closing the window does not quit.</p>
</div>
<div>
<h2>What Windows asks</h2>
<ul class="vss-ticks">
<li>Windows Firewall asks the first time a program starts listening on every network address. Pressing Allow access needs administrator rights. Studio makes no firewall rule by itself. The Setup check reads the rules and shows what is missing, and <code>vss_cli permissions firewall-allow</code> adds one rule after you agree in Studio.</li>
<li>The installer and the programs are signed with a certificate from the same authority when the release is built that way, so Windows may show Unknown publisher on a computer where the authority is not installed.</li>
<li>A 32-bit program on 64-bit Windows reaches the driver like any <code>COM</code> port.</li>
</ul>
</div>
</div>
<ul class="vss-links"><li><a href="../../guide/windows/#old-programs-on-windows">Old programs on Windows</a></li><li><a href="../../guide/windows/#choosing-a-com-number-windows">Choosing a COM number</a></li><li><a href="../../guide/when-the-system-asks/#when-the-operating-system-asks-or-says-no">When the operating system asks</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>What is tested</h2>
<p>The platform matrix builds Studio and runs the tests on a Windows Server 2025 runner. The driver tests and the Studio checks run in a Windows 10 22H2 guest and a Windows 11 guest on the owner's machine. Tests that need pseudo terminals are skipped on Windows. The release workflow also runs the installer silently twice, the second time as an upgrade, and then uninstalls it, and it requires a clean machine afterwards.</p>
</div>
<ul class="vss-links"><li><a href="../">Platforms</a></li><li><a href="../../developers/platforms/">Platforms and the test matrix</a></li><li><a href="../../downloads/">Downloads</a></li></ul>
</div></section>
