---
template: marketing.html
layout: prose
title: Support
title_tag: "Support: Virtual Serial Studio"
description: "How to get help with Virtual Serial Studio, report a problem or a security issue, ask about Pro and Enterprise, and what support each edition includes."
---
<!-- OWNER DECISION: the support channel, hours and response times for Pro and Enterprise, the supported versions, the update policy, and the sales channel. They are not decided, so the table below says "details on request". -->
<!-- OWNER DECISION: where mail to the support address goes. virtual-serial-studio.com had no MX record when this page was written (docs/plan/site.md, section 1), so mail to it does not arrive until the owner decides. The address is shown as plain text and not as a link, as the plan says. A contact form on the company site is added in a later change and is not mentioned here. -->

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

For Pro, Enterprise, a trial, plugins or anything else that is not a fault in the program, write to support@virtual-serial-studio.com. Say what you want to do, which edition you are asking about, how many people and computers will use it, and which systems and versions you run.

Please put nothing confidential in the message, and do not send a capture or any data from a customer's equipment.

## What support each edition includes { #support-by-edition }

| Edition | Support |
| --- | --- |
| Community | Help from the community on the GitHub issues page. There is no promised response time and no promised schedule for a fix. Only the newest release receives fixes. |
| Pro | Details on request. |
| Enterprise | Details on request. |

[Pricing](pricing.md) compares the editions.
