<!-- SPDX-License-Identifier: CC0-1.0 -->

# Licence test vectors

Licence files for testing a reader of Virtual Serial Studio licences, with the
answer a reader must give for each. Everything in this folder is under CC0 1.0:
copy it, change it and use it for anything, with no conditions.

## The key

Every file is signed with one key, `test-rfc8032-1`. It is the key of RFC 8032,
section 7.1, test 1, and its private half is printed in the RFC. Its public
half, in hexadecimal, is:

```
d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a
```

No file here is signed with a key that issues real licences. A program that
ships must never trust `test-rfc8032-1`. Studio does not: it drops every key
whose id starts with `test-`.

## The files

`index.json` lists the vectors. Each `.vsslic` file is one licence file,
compared byte for byte, so do not let a tool change its line endings.

Members of `index.json`:

| Member | Meaning |
|---|---|
| `test_public_key` | The public half of the test key, 64 hexadecimal digits |
| `key_lists` | Named key lists. Each key has an `id` and the window of signing times it is good for (`not_before`, `not_after`, or `null` for no bound). The public key is always the test key |
| `vectors` | The vectors, each as below |

Members of a vector:

| Member | Meaning |
|---|---|
| `file` | The licence file |
| `about` | What the vector shows |
| `keys` | The name of the key list to read it with |
| `now` | The time now |
| `build_time`, `latest_seen` | Lower bounds for the clock check, or `null` |
| `features` | The feature names asked about. The licence grants the answer when it names any one of them |
| `released` | When the thing asked about was released, for the version window, or `null` |
| `computer_name` | For a licence that lists computers: a name on its list, for a reader that checks the list. `null` otherwise |
| `expect.reason` | The reason name: `ok`, `expired`, `not_yet_valid`, `clock_suspect`, `malformed`, `newer_version`, `bad_signature`, `revoked` or `not_in_licence` |
| `expect.granted` | Whether the feature may be used |
| `expect.licence_id` | The licence id, or `null` when the reader must not have read one |
| `expect.state` | `valid`, `expiring`, `grace`, `expired`, `not_yet_valid`, `clock_suspect` or `invalid` |

Times are in the form `2027-01-01T00:00:00Z`.

## How they were made

The vectors are made by a test tool of Virtual Serial Studio, from
`tests/support/licence_vectors.cpp`. The tool signs with the test key only.
From a build of Studio's tests:

```
vss_licence_vectors sdk/licence-reader/vectors
```

Ed25519 signatures do not depend on chance, so the tool writes the same bytes
every time. A unit test of Studio fails when the files here differ from what
the tool makes.

Many of the files cannot be made by signing a licence in the usual way: a
signature with a bit flipped, a payload changed after signing, an envelope
with a member twice. The tool builds those by hand. A plain licence signed
with the test key can also be made with Studio's issuing tool:

```
vss_license issue --request request.json --out test.vsslic --test
```
