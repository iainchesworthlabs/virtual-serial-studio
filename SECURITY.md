# Security policy

## Reporting a vulnerability

Please report security vulnerabilities **privately**, not as a public issue or pull request, through
[GitHub's private vulnerability reporting](https://github.com/iainchesworthlabs/virtual-serial-studio/security/advisories/new):
it opens a private advisory that only you and the maintainer can see.

What helps most:

- what is affected: the version (the About dialog, or `vss_gui --version`), the platform, and whether it is
  `vss_gui`, `vss_cli` or a release file;
- how to reproduce it: the smallest topology file, command line or byte stream that shows it;
- what someone could do with it.

Please do not attach traffic captured from equipment that is not yours.

If you cannot use the form (you have no GitHub account, say), open an issue that says only that you have a security
matter, with no details, and the maintainer will reply there to arrange a private channel.

### What counts

Studio opens serial ports, creates virtual ones, and can bridge a port to TCP, RFC 2217 or UDP. So what counts is:

- a crash, hang, out-of-bounds read or other memory-safety fault that crafted bytes can cause, from a network
  endpoint or from a topology (JSON) file;
- a network endpoint that accepts a peer the user did not ask it to, or that exposes a port the user did not share;
- anything that lets one program read or write another program's virtual port without being given it;
- a flaw in the Windows COM driver, the build and workflow files, or what the release process publishes.

The bridges do not authenticate or encrypt: plain TCP, RFC 2217 and UDP have neither, and a hostile network is outside
what they defend against. A report that depends on that is a limit of the protocol, not a vulnerability in this code.

### What to expect

Studio is maintained by one person, so there is no guaranteed response time and no promised schedule for a fix. A
report is read and acknowledged in the advisory; one that is confirmed is fixed on `develop` and published with the
next tagged release. Please allow time for a fix to ship before you publish details: the advisory is where that is
agreed.

## Supported versions

Studio is pre-1.0. Only the newest release receives fixes; an older release is not patched.

## Signed releases

Every release file is listed in `SHA512SUMS`, carries a detached GPG signature (`.asc`) and a GitHub build-provenance
attestation, and the release holds an SPDX software bill of materials. The signing key is the organisation's release
key:

```text
Iain Chesworth Labs Releases <iain.chesworth@gmail.com>
Ed25519, created 2026-10-22, expires 2028-10-21
Fingerprint: FD13 CB8C AC83 1664 8CD9  74D0 8149 254C 4189 4DBE
             (FD13CB8CAC8316648CD974D08149254C41894DBE)
```

Trust the fingerprint above, which you are reading in the repository, and not a key file that arrives with a download:
a signature checked against a key from the same place as the file proves only that the two agree. The public key is
[`docs/signing-key.asc`](docs/signing-key.asc). It is not on a keyserver. To check a download, with the files in one
folder:

```sh
gpg --import docs/signing-key.asc
gpg --fingerprint FD13CB8CAC8316648CD974D08149254C41894DBE   # compare with the line above
gpg --verify SHA512SUMS.asc SHA512SUMS
sha512sum -c SHA512SUMS
gh attestation verify <file> --repo iainchesworthlabs/virtual-serial-studio
```

A key that does not have this fingerprint did not sign a release of this project; if you are shown one that claims
to, report it as above. If the key is ever revoked or replaced, this section changes in the same commit that changes
`docs/signing-key.asc`.

The Windows programs in a package are signed as the `SIGN_WINDOWS` setting says (see
[Releasing](docs/releasing.md#signing)); the macOS app is not yet signed or notarised.
