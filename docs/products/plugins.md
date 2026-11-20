---
template: marketing.html
title: Plugins
title_tag: "Plugins for Virtual Serial Studio: Virtual Serial Studio"
description: "Plugins add decoders, generators and recording to Studio. You install one, decide whether to trust it and switch it on, and each runs apart from Studio."
---
<!-- OWNER DECISION: official plugins. The owner says official plugins exist in the Pro product, but this repository does not hold them, so the section "Official plugins" below is a placeholder. Needed: (1) the name of each official plugin; (2) one plain line on what each one does for a customer; (3) which edition has it (Pro, Enterprise or both); (4) whether any needs a licence beyond the edition; (5) where a customer gets the plugin folder, since Studio never downloads a plugin; (6) whether the plugin is signed by Virtual Serial Studio today. Remove the placeholder paragraph and the comment when this is filled in. Check docs/user-guide/limits.md, docs/editions.md, docs/pricing.md and docs/faq.md, which still say that no official plugin has been released. -->
<!-- OWNER DECISION: how a plugin author gets the plugin API and a manifest guide. Today there is the plugin API reference in the developer pages and the design in docs/plugins.md; there is no SDK package beyond the licence reader in sdk/licence-reader. -->
<section class="vss-head"><div class="icl-wrap">
<p class="icl-eyebrow">Products</p>
<h1>Plugins</h1>
<p class="icl-lead">A plugin adds something to Studio, such as a decoder that reads a protocol in your traffic. You choose which plugins to install, you decide whether to trust each one, and each runs as a program of its own, apart from Studio. <span class="vss-chip vss-chip--ok">Plugin host in the packages</span></p>
<div class="icl-actions"><a class="icl-button" href="../../guide/plugins/">Plugins in the guide</a></div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>What a plugin can add</h2>
<ul class="vss-ticks">
<li><strong>A decoder</strong> reads the traffic of a port and shows the frames and events it finds in a window of its own, <strong>Tools, Decoded traffic...</strong>. Choose <strong>Decode as...</strong> on a port, pick the protocol, and the decoder starts. It only reads, and it never changes or delays your data.</li>
<li><strong>A generator</strong> sends traffic of its own onto a port.</li>
<li><strong>Recording and replay.</strong> A plugin you allow can record the data of the ports and links you name, and play one of its own recordings onto a port. While it does, the status bar says <strong>Reading data</strong> or <strong>Recording</strong>.</li>
<li><strong>Remote control.</strong> A plugin can ask to let another computer use this computer's ports. Studio asks you each time, shows what the session could do and how long it would last, and starts on <strong>Refuse</strong>.</li>
</ul>
<p>The plugin interface also has fixtures, link behaviours and connections to other tools. Studio does not run those yet. <span class="vss-chip vss-chip--plan">Planned</span></p>
</div>
<div>
<h2>Install, trust and switch one on</h2>
<ol>
<li>Get the plugin's folder from its author. Studio never downloads a plugin.</li>
<li>Open <strong>Tools, Plugins...</strong> and choose <strong>Add a plugin...</strong>. Studio looks into the folder and shows who made it and what its signature says. It copies nothing until you choose <strong>Add plugin</strong>.</li>
<li>If the author signed it and you have their public key, choose <strong>More options</strong>, then <strong>Import a key...</strong>, and compare the fingerprint with the one the author published.</li>
<li>Switch the plugin on. Studio asks first. The question lists the plugin's name, version, what it adds and each permission it asks for. It starts on <strong>Cancel</strong>.</li>
<li>Use what it adds, such as <strong>Decode as...</strong> on a port, or <strong>Add...</strong> in the plugin's own page.</li>
</ol>
<p>An administrator can install a plugin for everyone on a computer and can limit a computer to official plugins.</p>
</div>
</div>
<ul class="vss-links"><li><a href="../../guide/plugins/#deciding-about-a-plugin">Deciding about a plugin</a></li><li><a href="../../guide/plugins/#accepting-an-authors-key">Accepting an author's key</a></li><li><a href="../../guide/plugins/#adding-and-removing-a-plugin">Adding and removing a plugin</a></li><li><a href="../../guide/plugins/#decoding-traffic">Decoding traffic</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>You stay in charge</h2>
<p>A plugin runs as a program of its own, apart from Studio, and only after you have decided to trust that version of it. A signed plugin shows who made it, and Studio asks again if its files change.</p>
<p>The list in <strong>Tools, Plugins...</strong> shows, for each plugin, who stands behind it, what it adds, what it may do and how far Studio keeps it apart from your files.</p>
</div>
<div>
<h2>Who made it</h2>
<ul class="vss-ticks">
<li><strong>Official:</strong> signed by Virtual Serial Studio, and the only plugins that show its badge.</li>
<li><strong>Signed by an author:</strong> signed with a key that you or your administrator accepted.</li>
<li><strong>Signed with an unknown key:</strong> the author's key is not on this computer, so the signature could not be checked.</li>
<li><strong>Unsigned:</strong> nobody signed it, so Studio cannot tell who made it or whether it was changed.</li>
</ul>
</div>
</div>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Official plugins</h2>
<p>Official plugins are made and signed by Virtual Serial Studio, and are part of the Pro product. Placeholder: the list of official plugins, what each one does and which edition has it will be added here.</p>
</div>
<ul class="vss-links"><li><a href="../../pricing/">Pricing</a></li><li><a href="../../guide/licences/">Licences in the guide</a></li></ul>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>How plugins are kept safe</h2>
<p>A signature shows who made a plugin and that its files are unchanged. It does not show that the plugin is safe, so a plugin runs only after you decide, and the host also runs it apart from your files where the computer allows.</p>
</div>
<ul class="vss-ticks">
<li><strong>Windows:</strong> a plugin runs in a sandbox of its own. It can read its own folder and write its own data folder and no other file of yours, and it has no network unless you allowed that permission.</li>
<li><strong>Linux and macOS:</strong> Studio does not keep a plugin apart yet, and the Plugins page says so. An official plugin runs without that. Any other plugin runs only if you choose <strong>Run without isolation...</strong> for that version, where your administrator allows it.</li>
</ul>
<p>If you write plugins, the messages between a plugin and Studio are in the plugin API reference.</p>
<ul class="vss-links"><li><a href="../../developers/plugin-api/">The plugin API</a></li><li><a href="../../guide/plugins/">Plugins in the guide</a></li><li><a href="../../guide/limits/#what-does-not-exist-yet">What does not exist yet</a></li></ul>
</div></section>
