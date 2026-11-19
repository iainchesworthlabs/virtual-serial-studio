---
template: marketing.html
title: Plugins
title_tag: "Plugins and the plugin host: Virtual Serial Studio"
description: "Studio and vss_cli run plugins as programs of their own after you trust them. How trust, signing, the sandbox and the licence check work, and how to build one."
---
<!-- OWNER DECISION: official plugins. None exist in the repository, so the page says Planned. Which plugins come first, who signs them, and whether any needs a licence are not decided. -->
<!-- OWNER DECISION: how a plugin author gets the plugin API and a manifest guide. Today there is the plugin API reference in the developer pages and the design in docs/plugins.md; there is no SDK package beyond the licence reader in sdk/licence-reader. -->
<section class="vss-head"><div class="icl-wrap">
<p class="icl-crumb"><a href="../">Products</a> / Plugins</p>
<h1>Plugins and the plugin host</h1>
<p class="icl-lead">A plugin adds something to Studio and runs as a program of its own. The host starts it, keeps it running, stops it, and keeps it apart from your files where the computer can. <span class="vss-chip vss-chip--ok">Plugin host in the packages</span> <span class="vss-chip vss-chip--plan">Official plugins: Planned</span></p>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>What a plugin is</h2>
<p>A plugin is a folder that holds a manifest, <code>plugin.json</code>, the program it runs, and usually a signature, <code>plugin.sig</code>. Studio never downloads a plugin. You or your administrator put the folder in a plugin folder.</p>
<p>The manifest names the extension points the plugin offers. They are generator, fixture, modifier, adapter and decoder. The Plugins page says what a plugin adds, such as fixtures, generators, link behaviours or connections to other tools.</p>
<p>The host runs the plugin as its own process and talks to it over a private channel that only the host holds. The messages are in <a href="../../developers/plugin-api/">the plugin API, message by message</a>. If a plugin fails, the host starts it again after 1, 2, 4 and then 8 seconds, and stops trying after the fifth failure in ten minutes.</p>
</div>
<div>
<h2>Where plugins are managed</h2>
<ul class="vss-ticks">
<li><strong>Tools, Plugins...</strong> in Studio lists the plugins with their state, who stands behind each one, what it adds, how far it is kept apart from your files, and what it may do. <strong>Add a plugin...</strong> looks into a folder and shows what it found before anything is copied.</li>
<li>In the Advanced toolbox a plugin that is switched on has a tile for each fixture type. A port a plugin provides says so in the Inspector.</li>
<li><code>vss_cli plugin list</code>, <code>verify</code>, <code>install</code>, <code>remove</code> and <code>run</code> do the same from a terminal. They do not raise administrator rights for you. Installing a plugin for everyone needs a prompt that you opened as an administrator.</li>
</ul>
</div>
</div>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-intro">
<h2>Trust and signing</h2>
<p>A signature shows who made a plugin and that its files are unchanged. It does not show that the plugin is safe, so a plugin runs only after a decision.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">Line on the Plugins page</th><th scope="col">What it means</th></tr></thead>
<tbody>
<tr><th scope="row">Official</th><td>Signed by Virtual Serial Studio. Runs on its signature. None exist yet.</td></tr>
<tr><th scope="row">Signed by an author</th><td>Signed with a key that you or your administrator chose to trust. You decide about each plugin, for that exact version. If its files change, Studio asks again.</td></tr>
<tr><th scope="row">Signed with an unknown key</th><td>The key is not on this computer, so the signature could not be checked.</td></tr>
<tr><th scope="row">Unsigned</th><td>Nobody signed it. Studio cannot tell who made it or whether it was changed. It installs only with <code>--allow-unsigned</code>.</td></tr>
</tbody></table></div>
<ul class="vss-ticks">
<li>The question you answer lists the plugin's name, version and fingerprint, what it adds and each permission it asks for. A permission the plugin marks as optional has a box you can clear. The dialog starts on Cancel.</li>
<li>An author publishes a public key file ending in <code>.vsskey.json</code>. <strong>Import a key...</strong> shows its fingerprint, which you compare with the one the author published. Choosing to trust a key never switches a plugin on.</li>
<li>Keys are Ed25519. <code>tools/plugin_sign.py</code> in the repository makes a key (<code>keygen</code>), signs a plugin folder (<code>plugin</code>) and checks a signature (<code>verify</code>). A key can be kept encrypted with a passphrase.</li>
<li>An administrator can set a policy file that, for example, turns off the plugin folder of each user.</li>
</ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>The sandbox</h2>
<p>Where the computer can do it, the host runs a plugin apart from your files and your other programs. The Plugins page shows, for each plugin, how far that went and which parts were not applied.</p>
</div>
<div class="vss-table"><table>
<thead><tr><th scope="col">System</th><th scope="col">What the host does today</th></tr></thead>
<tbody>
<tr><th scope="row">Windows</th><td>Runs the plugin in a job object and an AppContainer of its own. It can read its own folder and write its data folder and no other file of yours. It has no network unless the permission was granted, cannot open other programs, has no window or clipboard, and is held to limits on memory, processes and processor time.</td></tr>
<tr><th scope="row">Linux and macOS</th><td>Does not confine a plugin yet, and says so. An official plugin runs without confinement. Any other plugin runs only if you choose <strong>Run without isolation...</strong> for that version, where your administrator allows it.</td></tr>
</tbody></table></div>
<p>The sandbox is a second line of defence. The first is the decision about trust.</p>
</div></section>

<section class="icl-section icl-section--tint"><div class="icl-wrap">
<div class="vss-two">
<div>
<h2>The licence check</h2>
<p>An official plugin that needs a licence is not started without one, and the Plugins page says so. The host checks that the licence on this computer is valid and names that plugin, or all official plugins.</p>
<p>A plugin can check the licence itself. The host hands it the licence file when it asks: always to an official plugin, and to any other only if the manifest asks for the permission <code>licence.read</code> and you allow it. <code>sdk/licence-reader</code> in the repository is a reader for licence files that a plugin may copy. It is under the BSD 2-Clause licence, and the rest of Studio is GPL.</p>
<p>Switching other features on or off by licence is not built. <span class="vss-chip vss-chip--plan">Planned</span></p>
</div>
<div>
<h2>How a third-party plugin is built and run today</h2>
<ol>
<li>Write a program that reads its channel number from <code>VSS_PLUGIN_CHANNEL</code>, says hello, and answers the host's messages as <a href="../../developers/plugin-api/">the API reference</a> lists them. <code>VSS_PLUGIN_ID</code> and <code>VSS_PLUGIN_DATA</code> name the plugin and its data folder.</li>
<li>Write <code>plugin.json</code>: the id, name and version, the program to run for each platform, the permissions it asks for with a reason for each, and its extension points.</li>
<li>Sign the folder with <code>tools/plugin_sign.py</code>, or leave it unsigned.</li>
<li>Run <code>vss_cli plugin verify &lt;folder&gt;</code>, then <code>vss_cli plugin install &lt;folder&gt;</code>.</li>
<li>Decide about the plugin in Studio, on the Plugins page. <code>vss_cli plugin run</code> never makes that decision for you.</li>
<li>Run it with <code>vss_cli plugin run &lt;id&gt; --seconds 30</code>, adding <code>--instance fixture:&lt;type&gt;</code> to ask for an instance of one of its extension points.</li>
</ol>
</div>
</div>
<pre class="vss-code"><code>vss_cli plugin verify ./modbus-fixture
vss_cli plugin install ./modbus-fixture
vss_cli plugin run com.example.modbus-fixture --seconds 30 --instance fixture:meter</code></pre>
<p><code>plugin run</code> prints one line each time the plugin's state changes, including the process id, how far it is confined and what was not applied. <code>--json</code> prints one JSON object on each line instead. It exits with 0 when the plugin ran and was stopped, and 1 when it never ran or failed too often.</p>
<ul class="vss-links"><li><a href="../../developers/plugin-api/">The plugin API</a></li><li><a href="../../guide/#plugins">Plugins in the guide</a></li><li><a href="../command-line/">Command line</a></li></ul>
</div></section>

<section class="icl-section"><div class="icl-wrap">
<div class="vss-intro">
<h2>Planned</h2>
<p>These are not available. Confinement of a plugin on Linux and macOS is not built either, and the sandbox table above says so.</p>
</div>
<ul class="vss-ticks">
<li>Official plugins, signed by Virtual Serial Studio. None exist. <span class="vss-chip vss-chip--plan">Planned</span></li>
<li>Response timing rules and seeded fault injection for a plugin or a test. The rule library exists and has no way in yet. <span class="vss-chip vss-chip--plan">Planned</span></li>
</ul>
</div></section>
