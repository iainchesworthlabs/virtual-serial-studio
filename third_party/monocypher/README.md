# Monocypher

[Monocypher](https://monocypher.org) 4.0.2, by Loup Vaillant and contributors,
under BSD-2-Clause or CC0-1.0 (`LICENCE.md`). It provides the Ed25519
signature check behind licence files and signed plugin manifests
(`libs/vss_license`). No cryptography is written in this project.

The four source files are byte for byte those of the release archive. Do not
edit them. To update, replace them with the files of a newer release and
update the table and the version in `THIRD_PARTY_NOTICES.md`.

| File | From the archive | SHA-256 |
|---|---|---|
| `monocypher.c` | `src/monocypher.c` | `afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59` |
| `monocypher.h` | `src/monocypher.h` | `f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374` |
| `monocypher-ed25519.c` | `src/optional/monocypher-ed25519.c` | `7c9b16056cbd27521919e8a6f56a228808b9e718afc42e3d33f28c08e5abdee2` |
| `monocypher-ed25519.h` | `src/optional/monocypher-ed25519.h` | `bd546edcd468d64e28caa3dbf4b1d6bfad7435c0ce994723fd81aae26405121b` |

The archive is `https://monocypher.org/download/monocypher-4.0.2.tar.gz`
(SHA-512 `bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714`).
The files were also compared with tag `4.0.2` of
`github.com/LoupVaillant/Monocypher`; they differ only in the version line that
the release script fills in.

The tests run the RFC 8032 vectors and the Project Wycheproof Ed25519 vectors
against this code (`tests/unit/test_license_signature.cpp`).
