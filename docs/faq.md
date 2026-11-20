---
template: marketing.html
title: FAQ
title_tag: "Questions and answers: Virtual Serial Studio"
description: "Answers to what people ask first about Virtual Serial Studio: what it replaces, which ports it shares, why Windows needs a driver, scripting and what is planned."
---
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">FAQ</p>
<h1>Questions and answers</h1>
<p class="icl-lead">The questions that come up first. Each answer is short and points to the page that has more.</p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<h2 class="icl-sr">The questions</h2>
<div class="icl-faq">
<details><summary>What does it do?</summary><p>It makes virtual serial ports, so a program has a port to open before the device exists or while the device is somewhere else. It shares one real device between several programs, joins ports to each other, and shows every byte that goes through. <a href="../solutions/develop-and-test-without-hardware/">Develop and test without hardware</a>.</p></details>
<details><summary>What does it replace?</summary><p>On Windows it makes <code>COM</code> ports with its own driver, so you do not need com0com. On Linux and macOS it makes ports that look like device files, and it can join two of them like a null-modem cable. <a href="../guide/windows/">Windows in the guide</a>.</p></details>
<details><summary>Which ports can it share?</summary><p>Studio opens a real serial device once and gives each program its own virtual port, up to 32 at a time. A port on the network (<code>tcp</code>, <code>udp</code> or <code>rfc2217</code>) can be shared the same way. If another program already holds a device, Studio cannot attach it, and it tells you when attaching fails. <a href="../solutions/sharing-and-bridging-ports/">Share a device and put it on the network</a>.</p></details>
<details><summary>Will it work with my existing program?</summary><p>A program that opens a <code>COM</code> port, or a device path such as <code>/dev/pts/3</code>, can open a virtual port. Some programs need care. A very old Windows program may only accept <code>COM1</code> to <code>COM9</code>, and Studio lets you choose the number. On Linux and macOS a program that lists the ports it finds will not list a virtual port, so give it the path. <a href="../guide/windows/#old-programs-on-windows">Old programs on Windows</a>.</p></details>
<details><summary>Why does Windows need a driver?</summary><p>On Windows a <code>COM</code> port is part of the system and not just a name, and an ordinary program cannot make one. Studio comes with a driver that does. Linux and macOS can make virtual ports without one. <a href="../guide/windows/">Windows in the guide</a>.</p></details>
<details><summary>Why is the driver test-signed?</summary><p>It is signed with a certificate from Iain Chesworth Labs' own certificate authority, and Windows accepts it only on a computer where that authority is installed. On any other computer the installer installs Studio, leaves the driver out and tells you why. A driver signed by Microsoft would install with no extra step. It does not exist yet. <span class="vss-chip vss-chip--plan">Planned</span></p></details>
<details><summary>Can it run with no window?</summary><p>Yes. The command line reads the same setup file as Studio and keeps the ports open until you stop it or a time you set runs out. A setup you built in the window can run in a CI job. <a href="../guide/command-line/">The command line in the guide</a>.</p></details>
<details><summary>How do I script it?</summary><p>From a shell, use the command line. From a test, use the Python client to make and link ports, send bytes, wait for a reply and get a JUnit report. The Python client is built from source today and is not in a package yet. <span class="vss-chip vss-chip--src">Built from source only</span> <a href="../guide/control-from-a-script/">Controlling Studio from a script</a>.</p></details>
<details><summary>Can it record what goes over a port?</summary><p>Yes. The Monitor shows every byte with its time and direction, as text or hex. You can export a capture, record from a terminal, and play a recording back at the times it was made. <a href="../solutions/capture-and-analysis/">Watch and record traffic</a>.</p></details>
<details><summary>Can it pretend to be a device?</summary><p>Yes. A port can echo what it receives, send text or a pattern at a set rate, or answer requests from a script with a delay you choose. <a href="../solutions/develop-and-test-without-hardware/">Develop and test without hardware</a>.</p></details>
<details><summary>Can I put a serial port on the network?</summary><p>Yes, as <code>tcp</code>, <code>udp</code> or <code>rfc2217</code>, either connecting out or waiting for a connection. None of them is encrypted and <code>rfc2217</code> has no password, so keep it on the same computer, use an SSH tunnel or a VPN, or list the computers that may connect. <a href="../security/#bridges">Ports you put on a network</a>.</p></details>
<details><summary>Does it send anything anywhere?</summary><p>No telemetry, usage data, crash reports or account. The only request Studio makes by itself is the optional update check. It is off until you turn it on and it sends nothing about you or your computer. A licence is checked on your computer and never sent. <a href="../privacy/">Privacy</a>, and <a href="../security/#network">what the program sends over a network</a>.</p></details>
<details><summary>What does the Community edition lack?</summary><p>Nothing that is built. It is the whole application, free under the GPL. Pro and Enterprise add a licence file, support and terms agreed with you. <a href="../pricing/">Pricing</a>.</p></details>
<details><summary>What is planned and not available yet?</summary><p>Official plugins, a Windows driver signed by Microsoft, controlling Studio from another computer with TLS, and client libraries for languages other than Python. <a href="../pricing/">Pricing</a> marks each one Planned.</p></details>
<details><summary>Can I use it at work, for free?</summary><p>Yes. Studio is under the GNU General Public License, version 3 or later, which allows use at work. If you distribute a program that includes Studio's code, the licence's terms apply to that program too. Take your own advice for your case. <a href="../licensing/">Licensing</a>.</p></details>
<details><summary>Which systems does it run on?</summary><p>Windows on x86-64, macOS on Apple silicon, and Linux on x86-64. The macOS files are not signed or notarised, so macOS asks you to confirm before it opens them. The Linux files need a recent system (glibc 2.42 or newer, which is Ubuntu 26.04 or later). <a href="../downloads/">Downloads</a> lists what each system needs.</p></details>
<details><summary>How do I report a problem?</summary><p>Open an issue on GitHub with the version, your system, what happened and what you expected, and the smallest setup that shows it. Studio can save a support bundle that never holds your traffic. Do not attach captures from equipment that is not yours. Report a security problem privately through GitHub. <a href="../support/">Support</a>.</p></details>
</div>
</div></section>

<section class="icl-section icl-section--tint icl-cta"><div class="icl-wrap">
<h2>Not answered here?</h2>
<p class="icl-lead">The guide covers each job in full, and Support says where to ask.</p>
<div class="icl-actions"><a class="icl-button" href="../guide/">Read the guide</a><a href="../support/">Support</a></div>
</div></section>
