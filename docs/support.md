---
template: marketing.html
layout: prose
title: Support
title_tag: "Support: Virtual Serial Studio"
description: "How to get help with Virtual Serial Studio, report a problem or a security issue, ask about Pro and Enterprise, and what support each edition includes."
---
<!-- OWNER DECISION: the support channel, hours and response times for Pro and Enterprise, the supported versions, the update policy, and the sales channel. They are not decided, so the table below says "details on request". -->
<!-- OWNER DECISION: where mail to the support address goes. The zone of virtual-serial-studio.com has MX records to iCloud Mail (read on 20 November), so mail to the address can arrive; the owner confirms that support@ is an address or alias of that mailbox before the page is published. The address is shown as plain text and not as a link, as the plan says. The form below posts to the company site's contact endpoint, which emails the owner of the company site and keeps a copy for 30 days; it works once the company stack has been redeployed with the source virtual-serial-studio, and a pull request preview cannot send it. -->

# Support

## Before you ask

- The [guide](user-guide/index.md) is organised by task. Its [troubleshooting](user-guide/troubleshooting.md#troubleshooting) lists what Studio shows and what to check, starting with the most common.
- The [FAQ](faq.md) answers the questions that come up first.
- The [platform pages](platforms/index.md) say what each system needs and what has not been tried, so you can tell whether a problem is a known gap.

In Studio, **Help, Why is nothing arriving?** walks through a port that is open and silent, and **Tools, Setup check** says what the computer is missing for ports that other programs can open.

## Report a problem

Open an issue on the project's [GitHub issues](https://github.com/iainchesworthlabs/virtual-serial-studio/issues) page. It helps to say:

- the version, which the About window, `vss_gui --version` and `vss_cli --version` show, and the system;
- what happened and what you expected;
- the steps to reproduce it;
- the text of any message.

The smallest setup file or command line that shows the problem helps most. **File, Support bundle** saves a ZIP with the version, the driver state, your setup, the event log and the serial ports on the computer. It never holds the bytes that your ports sent or received, and you can read each file before you save it. Nothing is sent: you attach it yourself if you want to.

**Do not attach a capture or any data taken from a customer's equipment, or from equipment that is not yours.** Make a small one that shows the problem.

Studio is maintained by one person, so there is no promised response time and no promised schedule for a fix.

## Report a security problem

Report it privately, not as a public issue. Use [GitHub's private vulnerability reporting](https://github.com/iainchesworthlabs/virtual-serial-studio/security/advisories/new). If you cannot, open an issue that says only that you have a security matter, with no details, and the maintainer will arrange a private channel. [The security policy](https://github.com/iainchesworthlabs/virtual-serial-studio/blob/develop/SECURITY.md) says what counts, and [Security](security.md) says what the program sends over a network and where its protections stop.

## Talk to us { #talk-to-us }

For Pro, Enterprise, a trial, plugins or anything else that is not a fault in the program, use the form below or write to support@virtual-serial-studio.com. Say what you want to do, which edition you are asking about, how many people and computers will use it, and which systems and versions you run.

The form goes to one person and to no one else. Nothing you write is published, and there is no automatic reply: we answer by email, from our own address. Please put nothing confidential in it, and do not send a capture or any data from a customer's equipment. <a href="/privacy/#contact-form">What the form keeps, and for how long</a>.

<form id="contact-form" class="vss-form" method="post" action="/api/contact">
<input type="hidden" name="source" value="virtual-serial-studio">
<p class="vss-form__help">Every field is needed except the organisation.</p>
<div>
<label for="cf-name">Your name</label>
<input id="cf-name" name="name" type="text" autocomplete="name" maxlength="100" required>
</div>
<div>
<label for="cf-organisation">Organisation <span class="vss-form__opt">(optional)</span></label>
<input id="cf-organisation" name="organisation" type="text" autocomplete="organization" maxlength="150">
</div>
<div>
<label for="cf-email">Your email address</label>
<input id="cf-email" name="email" type="email" autocomplete="email" maxlength="254" required aria-describedby="cf-email-help">
<p id="cf-email-help" class="vss-form__help">This is where we reply. Give one address.</p>
</div>
<div>
<label for="cf-topic">What are you asking about?</label>
<select id="cf-topic" name="topic" required>
<option value="">Choose one</option>
<option value="pro">The Pro edition</option>
<option value="enterprise">The Enterprise edition</option>
<option value="trial">A trial</option>
<option value="plugins">Plugins</option>
<option value="other">Something else</option>
</select>
</div>
<div>
<label for="cf-message">Your message</label>
<textarea id="cf-message" name="message" rows="7" minlength="10" maxlength="4000" required aria-describedby="cf-message-help"></textarea>
<p id="cf-message-help" class="vss-form__help">At least 10 characters and at most 4,000, with no more than two web addresses.</p>
</div>
<div class="vss-form__trap" aria-hidden="true">
<label for="cf-ref">Leave this field empty</label>
<input id="cf-ref" name="ref_code" type="text" tabindex="-1" autocomplete="off">
</div>
<div class="vss-form__consent">
<input id="cf-consent" name="consent" type="checkbox" value="yes" required>
<label for="cf-consent">I agree that Iain Chesworth Labs may use what I send here to answer me. <a href="/privacy/#contact-form">What is kept, and for how long</a>.</label>
</div>
<button class="icl-button" type="submit">Send message</button>
</form>

## What support each edition includes { #support-by-edition }

| Edition | Support |
| --- | --- |
| Community | Help from the community on the GitHub issues page. There is no promised response time and no promised schedule for a fix. Only the newest release receives fixes. |
| Pro | Details on request. |
| Enterprise | Details on request. |

[Pricing](pricing.md) compares the editions.
