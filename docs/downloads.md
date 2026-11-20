---
# The Downloads page. The table, the version and the date come from the newest release (tools/docs/release/, written by
# tools/docs/refresh_release.py), through the releases: markers of tools/docs/site_release_hook.py. Nothing here is typed by hand
# that the release changes. How the release data works is in docs/ci.md, "Release data and the update feed".
template: marketing.html
layout: prose
title: Downloads
description: Download Virtual Serial Studio for Windows, macOS and Linux. Each file has a signature, and a checksum file lists every file.
---

# Downloads

Every file below is on GitHub Releases, and each link goes straight to the file. Each file has a signature, and `SHA512SUMS` lists a
checksum for every file. [How to verify a download](#verify) says how to check them.

The newest release is version <!-- releases:version -->, published on <!-- releases:date -->. It is a <!-- releases:kind -->.
A release candidate or other pre-release is for trying out and for finding faults. Do not rely on one for work that matters.
[Every file of <!-- releases:tag -->, and earlier releases, on GitHub](<!-- releases:url -->).

<!-- releases:downloads -->

## Before you install

- The Windows files are for x86-64 only; there is no Arm build.
- The Windows driver is test-signed with a certificate from the certificate authority of Iain Chesworth Labs, so Windows accepts it
  only on a computer where that authority has been installed; on any other computer the installer installs Studio and leaves the
  driver out.
- The macOS files are for Apple silicon only, and they are not signed or notarised, so macOS asks you to confirm before it opens
  Studio.
- The Linux files are for x86-64 and need glibc 2.42 or newer and the C++ library from GCC 16, which is Ubuntu 26.04 or later, so on
  Ubuntu 24.04 the packages refuse to install and the archive does not start.

## How to verify a download { #verify }

Each file has a detached OpenPGP signature beside it, with the same name and `.asc` added. `SHA512SUMS` lists a checksum for every file
and is signed as well. Each package also has a GitHub build-provenance attestation.

1. **Get the release key.** Download [the public key](signing-key.asc) from this site and import it. Then compare its fingerprint with
   the one below and with the one in `SECURITY.md` in the repository. The key is not on a keyserver, and a key that arrives with a
   download proves nothing by itself.
2. **Check the signature.** Put the file and its `.asc` in one folder and ask GnuPG to check them.
3. **Or check the checksums.** Check `SHA512SUMS` with its signature, then check your file against it.

```sh
gpg --import signing-key.asc
gpg --fingerprint FD13CB8CAC8316648CD974D08149254C41894DBE
gpg --verify FILE.asc FILE

gpg --verify SHA512SUMS.asc SHA512SUMS
sha512sum --check --ignore-missing SHA512SUMS
gh attestation verify FILE --repo iainchesworthlabs/virtual-serial-studio
```

Put the name of the file you downloaded in place of `FILE`. The release key is `Iain Chesworth Labs Releases`, Ed25519, and it
expires on 21 October 2028. Its fingerprint is `FD13 CB8C AC83 1664 8CD9 74D0 8149 254C 4189 4DBE`. If the fingerprint is different, the
key did not sign a release of this project.
