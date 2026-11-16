<!-- SPDX-License-Identifier: BSD-2-Clause -->

# Licence reader

A reader for Virtual Serial Studio licence files (`.vsslic`) that a plugin may
copy into its own source. It is two files, `licence_reader.hpp` and
`licence_reader.cpp`.

## This folder is not GPL

Virtual Serial Studio is under GPL-3.0-or-later. This folder alone is not:

* `licence_reader.hpp`, `licence_reader.cpp`, `CMakeLists.txt` and this file are
  under the BSD 2-Clause licence. The text is in [LICENSE](LICENSE).
* Everything in `vectors/` is under CC0 1.0. See
  [vectors/README.md](vectors/README.md).

The reason is that a plugin checks the licence itself and does not take
Studio's word for it. A plugin that is not GPL may not copy or link Studio's
own licence library, so it needs a reader it may copy. The reader was written
from the description of the format in `docs/licensing.md` and shares no code
with Studio's library. It includes nothing from the rest of this repository.

## What you need

* A C++17 compiler and the standard library.
* Monocypher's Ed25519, from your own copy of Monocypher (BSD 2-Clause or
  CC0): `monocypher.c`, `monocypher-ed25519.c` and their headers. The reader
  includes `<monocypher-ed25519.h>` and calls `crypto_ed25519_check`. It does
  not bring a copy.

There is no JSON library to add. The reader has a small strict JSON reader of
its own.

## How to use it

```cpp
#include "licence_reader.hpp"

vss_licence_reader::question asked;
asked.keys = my_trusted_keys();            // the keys you built in
asked.now = seconds_since_1970_now();
asked.build_time = when_this_plugin_was_built();
asked.latest_seen = my_stored_clock.latest_seen;
asked.features = {"plugins.official", "plugins.com.example.decoder"};
asked.has_released = true;
asked.released = when_this_version_was_released();

const vss_licence_reader::answer answer =
    vss_licence_reader::read_licence(file_text, asked);
if (!answer.granted)
{
    // answer.reason says why, and goes in licence.report unchanged.
}
```

You pass in everything the reader uses: the bytes of the file, the time, and
the keys you trust. It reads no file and no clock.

`answer.reason` is one of `ok`, `expired`, `not_yet_valid`, `clock_suspect`,
`malformed`, `newer_version`, `bad_signature`, `revoked` and `not_in_licence`.
These are the reason names Studio uses. `answer.licence_id` is set when the
signature verified and the payload was read.

### Keys

`asked.keys` is your own list: the id, the 32 bytes of the public key and the
signing times the key is good for. Build the real public keys into your plugin.
A file signed by a key that is not on your list is refused with
`newer_version`.

Never put the test key `test-rfc8032-1` on the list of a plugin you ship. Its
private half is published in RFC 8032, so anyone can sign with it.

### The clock

The reader refuses a licence with `clock_suspect` when the time now is more
than 48 hours before a time that has certainly passed. It takes the latest of
three such times: `build_time`, `latest_seen` and the licence's `issued_at`.

A confined plugin cannot read Studio's folder, so it keeps its own latest time
seen in its own data directory:

```cpp
stored = vss_licence_reader::observe(stored, now);         // at each start
stored = vss_licence_reader::apply_reset(stored, answer);  // after a read
```

`apply_reset` handles a licence that carries a `clock_reset` token. The owner
sends one to a customer whose clock once ran far ahead. A token that has not
been applied sets the stored time back to the licence's `issued_at`, once.
Store `latest_seen` and `applied_resets` between runs.

### What it does not check

* **Revocation lists.** A plugin has none. Studio checks them.
* **The `machines` list.** The reader checks that the list is well formed and
  does not compare it with the computer's name. Studio checks it.

So a licence Studio refuses with `revoked` (by a revocation list) or
`wrong_computer` may still read as `ok` here. Studio does not start an official
plugin under such a licence.

## What it checks

In this order:

1. The file is at most 64 KiB.
2. The envelope is one JSON object with exactly five members. `format` is 1.
3. `purpose` is `licence`.
4. `payload` and `signature` are base64url without padding.
5. `key_id` names a key on your list.
6. The Ed25519 signature holds over `"VSS-SIGNED-FILE-1"`, a zero byte, the
   purpose, a zero byte and the payload. A key or a signature that is not
   written in its one canonical form is refused.
7. The payload is well formed, and `critical` names no member the reader does
   not know.
8. `issued_at` is inside the key's window.
9. The clock, then the term (`starts_at`, `expires_at`, `grace_days`), then the
   features, then the version window (`updates_until`).

## Test vectors

`vectors/` holds licence files with the answer a reader must give for each.
Studio's own tests run every one through this reader and through Studio's
library, and fail when either gives another answer or when the two disagree.
If you write your own reader, or change this one, run it over the vectors.

## Building it

Add `licence_reader.cpp` to your plugin and put Monocypher's headers on the
include path. Nothing else is needed. The `CMakeLists.txt` here is for Studio's
own tests.
