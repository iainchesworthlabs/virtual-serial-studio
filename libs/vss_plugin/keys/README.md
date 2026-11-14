# The official plugin keys

Public files only. No private key is ever in this folder or anywhere in this repository.

| File | What it is |
|---|---|
| `official-keys.payload.json` | The first official key list, as the text that is signed. It names the key that signs official plugins and the key that signs revocation lists, with the days each counts on |
| `official-keys.json` | The same list, signed with the root key. It is not here until the owner has signed it |

The root public keys are in `../src/official_roots.cpp`, built into Studio. `docs/plugins.md`, section 8.9, says how the keys relate.

## Signing the key list

The owner does this, on the computer that holds the root private key. It needs OpenSSL 3 and Python 3.9 or later, and nothing else. From the top of the repository:

```
python3 tools/plugin_sign.py key-list --key <path of the root private key> --key-id root-2026 --payload libs/vss_plugin/keys/official-keys.payload.json --out libs/vss_plugin/keys/official-keys.json
```

The script checks the list first, passes the path of the key file to OpenSSL, and writes `official-keys.json`. It does not read, copy or print the key.

Then check the result with the root public key, which needs no private key:

```
python3 tools/plugin_sign.py verify --public-key 3a94dda0542ec5631e0ce810151baa831dd7f128ff9c393a68fef153026b0ca1 --purpose key-list libs/vss_plugin/keys/official-keys.json
```

It prints `ok`. Commit `official-keys.json`. The test `plugin signature: the signed key list is accepted by the built-in root` then runs; until the file is there it is skipped.

## Changing the list

A new list has a higher `serial`. To add a key, add its entry. To withdraw one, put its id in `revoked`; it stays there in every later list. Sign the new payload as above. A list is signed exactly as its bytes are on disk, so sign after the last edit.

## The dates

Both keys count from 14 November 2026 to 14 November 2028. An official plugin cannot be valid after the day its signing key stops counting, so two years is also the longest an official plugin signed with `plugins-2026` runs before it needs a newer signature.
