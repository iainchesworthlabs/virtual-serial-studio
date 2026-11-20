---
template: marketing.html
layout: prose
title: Privacy
title_tag: "Privacy: Virtual Serial Studio"
description: "What Virtual Serial Studio sends and what it does not: no telemetry, an optional update check, what this site collects, and what GitHub sees when you download."
---
<!-- OWNER DECISION: the legal text of this page. It says what the program, the site and the releases do, as far as the repository shows, and is for a solicitor to review. Decide the legal wording, and what happens to mail sent to the support address. -->

# Privacy

<div class="icl-callout"><strong>DRAFT</strong><p>DRAFT, not legal advice, to be reviewed before it is relied on. It says what the program, this site and the release files do, as far as the repository shows.</p></div>

## The program

Virtual Serial Studio sends nothing about you or your computer. It has no telemetry, no analytics, no usage data, no crash reports and no account.

It makes a network connection in two cases only:

- **A port you make.** A `tcp`, `udp` or `rfc2217` endpoint connects to the address you gave it, or listens for the computers you allow. It carries the bytes of that port and nothing else.
- **The update check.** It is off until you turn it on: Studio asks once, at the first start, whether to check automatically, and **Help, Check for updates** asks at any time. A check is one HTTPS request for one signed file, with no query, no cookie and no body, and a fixed `User-Agent` that holds no version and no system. Studio reads the whole file and chooses for itself, so the site is never told which version you have. The server sees what any web server sees: the address the request came from and the time. A build that has no key to check the file makes no request. An administrator can turn the check off for everyone on a computer.

If you say yes to an update, the package comes from the project's releases on GitHub, which sees the request as it sees any download.

`vss_cli` makes no update check.

A licence file is checked on the computer, and nothing about it is sent. The file holds the name of the person or organisation it was issued to, and may hold a contact and an email address. They stay on the computer, are shown to the person who uses it, and are left out of a support bundle unless that person turns the row on.

A support bundle is a file that you save and send yourself, if you choose to. It never holds the bytes your ports sent or received.

<a href="../security/#network">Security</a> lists these connections in a table.

## This site

This site is a section of the company site of Iain Chesworth Labs, and its privacy text applies: <a href="/privacy/">the company site's privacy page</a>. It is a page of the company site and not of this one, and it says what the company site's servers keep.

The front pages of this site run no script, have no analytics and load nothing from another site. The checks in this repository fail the build if a page does otherwise.

## Release files and GitHub

The release files, the signatures, the bill of materials and the issues are on GitHub. When you download a file, open an issue or read the source there, GitHub sees your request, and its own privacy statement applies. We do not control what GitHub collects. If you open an issue, what you write is public.

## Writing to us

If you write to the support address, we have your message and your address, and we use them to answer you. Please do not send confidential material or data taken from equipment that is not yours.
