# Secure remote control and monitoring

Status: **accepted**. The owner accepted the recommendations of section 14 on 13 November 2026. Section 13 lists the
slices that build it; each slice says in its own pull request what it built and tested.

It builds on [the per-user agent](agent.md) (the local protocol and its trust model) and on the access levels decided
in [Editions, licensing and plugins](editions.md). Where this page says "the agent" it means `vss_agent` as agent.md
describes it.

## The problem

Pro adds a control endpoint. A test harness (pytest, the C ABI and its wrappers for C#/.NET, Java and Node.js,
LabVIEW, TestStand, Robot Framework, the JUnit report writer) drives Studio through it. The same endpoint lets one
other computer watch a running Studio. That turns a program that today has no listener into one that accepts
commands, and the commands can create ports, write bytes to real devices and change what a test rig does.

Three access levels were decided, each needing more set-up than the last:

1. **Local only (default).** Reachable only from the same computer and the same account.
2. **Token.** Generated on the receiving side, given to the sending side, revocable, limited to chosen actions.
3. **Administrator control.** Plugins add OAuth 2.0 / OpenID Connect, an external identity provider and the client
   credentials grant for unattended rigs.

Constraints that are not open: no telemetry; no network access from the GUI process; the GUI thread never blocks on
hub threads; the agent stays per user and stays free of network code; `vss_core` behaviour and the JSON topology
schema do not change.

## 1. Goals and non-goals

Goals:

1. With nothing enabled, no new way in exists. Enabling a thing is explicit, visible and reversible.
2. Each level adds access only by the user (or an administrator) doing more. No level is reachable by accident,
   and no level weakens a lower one.
3. A stolen, leaked or replayed credential does as little as possible, for as short a time as possible, and can be
   cut off at once.
4. A buggy or hostile client can slow only itself.
5. The decisions about who may do what are made in **one place** in the code, tested on their own.
6. The API is versioned and stable, because scripts outlive releases.

Non-goals:

- Protection from a malicious program of the **same** user running on the same computer. agent.md already says the
  agent is not a boundary inside one account, and the local level inherits that (section 2, T2).
- Protection from an attacker who is administrator or root on the receiving computer, or who owns its kernel.
- Multi-tenant hosting: one Studio serves one user's rig.
- A licence server. The licence is not an access control (section 11).

## 2. Threat model

### Assets

| # | Asset | Why it matters |
|---|---|---|
| A1 | Control of ports and links | Wrong data to a real device, a test that passes or fails falsely |
| A2 | Bytes on the wire | Can carry passwords, keys, medical or industrial data |
| A3 | Topology and the machine's facts | Names of devices, COM numbers, host names, installed software |
| A4 | Tokens and keys (token store, TLS key, IdP secrets) | Whoever holds one is the principal |
| A5 | Availability of the rig | A lab run that hangs wastes hours |
| A6 | The audit log | Evidence of who did what |
| A7 | The user's account itself | The endpoint must not be a way to run code as the user |

Studio's operations are all inside A7's blast radius if one is careless: `release_port` ends a process, `apply`
reads a topology that names files and devices, `capture` writes a file. The control surface is therefore smaller
than `StudioBackend` on purpose (section 9), and no operation takes a path to read or write from the remote side.

### Attackers and what each can do

| # | Attacker | Position | Main risks | Controls (section) |
|---|---|---|---|---|
| T1 | Another user on the same computer | Local account, no rights | Connect to the pipe or socket; connect to a loopback port; read the token store or the log; squat the name | OS access lists and modes, peer credentials, token required on loopback, private directory with checks (3, 4) |
| T2 | Another process of the same user | Same account | Everything the user can do | **Accepted** for level 1: the agent is no boundary inside an account. Mitigated for level 2 and 3 only in what it can steal (token file is 0600, which is not a boundary here either). A browser or document viewer is a same-user process: it cannot speak a pipe, and the TCP form refuses anything that looks like HTTP (3) |
| T3 | A web page in the user's browser | Sends requests to `localhost` | DNS rebinding, cross-site WebSocket or form post to a loopback port | Binary preface the browser cannot send, no HTTP, token required, no CORS-reachable surface (3) |
| T4 | Network attacker, passive | On the path once remote is on | Read tokens and traffic bytes | TLS 1.3 from the first byte, token never sent in clear, refusal to listen without TLS on a non-loopback address (3, 4) |
| T5 | Network attacker, active | On the path, or a rogue host at the same address | Man in the middle; downgrade; replay a captured session; impersonate Studio to collect a token | Certificate pinned by the sender, pairing out of band, TLS 1.3 only, no fallback (3, 12) |
| T6 | Network attacker, unauthenticated | Can reach the port | Guess tokens, flood the listener, exhaust handshakes, probe for the version | Rate limit with back-off, uniform failures, small pre-authentication budget, no information before authentication (4, 10) |
| T7 | Token thief | Obtained a token from a script, a CI log, a backup, a chat | Use the token from elsewhere | Scope and expiry limit it, revocation cuts it, optional source address list, pin binding, last-use display, tokens never appear in logs or URLs (4, 11) |
| T8 | Malicious or compromised plugin | Same user's process, loaded as a plugin | Use its granted scope for more than the manifest says; mint or forward credentials; feed forged identities (the level 3 plugin) | One enforcement point that plugins cannot skip when going through the gateway; the identity plugin's output is clamped by a policy ceiling; manifest permissions are shown; trust levels and hash pinning from editions.md (6, 7) |
| T9 | Confused deputy | A low-scope client induces a higher-scope component to act | A monitor session causes a write; a plugin relays a request with its own scope instead of the caller's | Every request carries the principal and is checked against that principal, never against the connection that carried it. Read-only sessions get a read-only agent connection (6, 7) |
| T10 | Malicious or buggy client | Authenticated, even legitimate | Oversized or malformed frames, infinite request loops, never reading events, asking for huge results, leaking resources | Frame and field limits, quotas, back-pressure, per-session budgets, fuzzing (3, 10, 15) |
| T11 | Denial of service | Anyone who can connect | Exhaust connections, memory, CPU in the handshake, the hub, the disk with the log | Connection and handshake limits, the gateway is a separate process so the agent stays up, log size cap, bounded queues (10) |
| T12 | Downgrade | Network or client | Force plain text, an older protocol, token over plain TCP, a weaker auth level | Minimum versions, no plain listeners off-host, auth level fixed per listener, no negotiation downward (3, 4, 12) |
| T13 | Log and diagnostics leak | Anyone who reads a log, a support bundle, a crash dump, a CI transcript | Tokens, key material, traffic bytes in the log | Secrets have a type that cannot be printed, logs never take bytes, the support bundle excludes the token store, canary tests (11, 15) |
| T14 | Insider with admin on the receiving computer | Local administrator | Anything | **Out of scope.** The audit log is tamper-evident, not tamper-proof |
| T15 | Lost or stolen sender machine | The harness computer is taken | Its stored token and pinned certificate | Revocation on the receiving side; short expiry for tokens on shared rigs; level 3 rigs use short-lived tokens that a stolen machine cannot renew without the client secret (4, 8) |

### Abuse cases that the design must handle

| Case | Outcome |
|---|---|
| Wrong, missing or revoked token | `unauthenticated`, identical in wording and timing for all three; counted against the source |
| A valid token asks for an action outside its scope | `forbidden`, audited, the session stays open |
| A token is presented over a listener that does not allow tokens | `unauthenticated`; the listener's allowed methods are part of its configuration, and a downgrade to tokens never happens by itself |
| A client replays a captured token from another host | Works if the thief has the token and nothing else restricts it. That is why scope, expiry, revocation and the source list exist. The token is not secret from the server's point of view, so replay by someone who read it is treated as use of a stolen credential (T7), not as a protocol break |
| A client replays a captured **session** | Not possible on TLS 1.3 (fresh keys per connection); there are no long-lived session ids to steal |
| A plugin sends a request on behalf of a remote user | The request is checked against the remote user's scopes, then the plugin's own, whichever is smaller |
| A viewer asks for the traffic of a port it should not see | `monitor` sessions have no bytes unless the token has the data scope; the port filter is part of the scope (5) |

## 3. Process layout and transports

### The gateway

The agent stays what agent.md says: per user, no network, one local pipe or socket. The control endpoint is a
**separate per-user program, `vss_gate`** (the gateway), which is an ordinary client of the agent. It owns the
listeners, authentication, authorisation, rate limits and the audit log, and translates the public API
(section 9) into agent requests.

| Option | For | Against | Verdict |
|---|---|---|---|
| Listeners inside `vss_agent` | One process fewer | A network bug is an engine bug; every user of the agent carries the attack surface even when remote control is off; the agent would link a TLS library and an HTTP-free parser it does not otherwise need | Rejected |
| Listeners inside `vss_gui` | | Forbidden: no network access from the GUI process | Rejected |
| A separate `vss_gate` | The agent, the GUI and `vss_cli` link nothing new and open no listener; Community builds do not contain it; it can crash and restart without stopping a port; the audit log and the secrets live in one small process; it can drop privileges and be sandboxed independently | One more process to start and stop | **Recommended** |

`vss_gate` is started by the agent's owner (the GUI's enable switch, or `vss_cli control enable`), not by an
incoming connection. When no listener is enabled it is not running. It is part of the Pro plugin set, so Community
builds contain none of this code.

The gateway connects to the agent as a normal local client, one connection **per remote session**, so that a
`monitor` session can be given a connection that the agent itself makes read-only (agent.md, decision 10, the
`read_only` flag of `hello`). That is defence in depth: a bug in the gateway's own check still cannot write through a
read-only connection. This slice therefore depends on decision 10 of agent.md being taken as recommended and on its
PR 5.

The agent protocol is private and carries no compatibility promise (agent.md, section 2). The public API of
section 9 is stable. The gateway is where the two meet, so the agent protocol can change without breaking scripts.

### Transports

| # | Transport | Level | Where it listens | Authentication of the other end |
|---|---|---|---|---|
| L1 | Named pipe (Windows) or Unix domain socket (Linux, macOS) | 1, 2, 3 | `\\.\pipe\vss-control-<user SID>`; `<agent directory>/control.sock` | The kernel (access list, modes) and **peer credentials**. A token may still be required (below) |
| L2 | TCP on loopback (`127.0.0.1` and `::1`) | 1 (opt-in), 2, 3 | A port the user chooses; never `0.0.0.0` | A token is always required: loopback cannot say who is calling |
| L3 | TLS over TCP on a chosen address | 2, 3 | The address and port the user chooses | Token or level 3 credential, over TLS with the certificate pinned by the sender |

L1 is the default and the one to prefer. L2 exists because some tools (LabVIEW, some TestStand set-ups, anything in
a container that shares the host's loopback) cannot open a pipe or a Unix socket. It is **off** unless chosen. L3 is
the remote case and exists only for a computer that watches or drives another.

**Common rules, all transports:**

- **One framing.** The control protocol uses the same framing as the agent (agent.md, section 4: length-prefixed,
  JSON header, blob), preceded on L2 and L3 by an 8-byte preface (`VSSCTL` and the major version). A connection that
  does not begin with the preface is closed with no reply, so an HTTP client, a browser WebSocket and a port scanner
  learn nothing. No HTTP or WebSocket is spoken at all; T3's attack needs a request a browser can send.
- **Small before authentication.** Before authentication a frame may be at most 4 KiB and the only operation is
  `hello`. The full 1 MiB applies after. The header depth limit is 8 and text limits are the agent's.
- **Authentication first.** A connection has a deadline of 5 s to authenticate. Nothing reveals Studio's version,
  port names or capabilities until it does. The failure reply is uniform.
- **No plain off-host listener.** The gateway refuses to start an L3 listener with TLS off, and refuses a bind
  address that is not loopback for L2. This is a rule in code and is tested, not a setting.
- **No negotiation downward.** The preface carries one major version; another one is refused. TLS is 1.3 only (below).
- **Host-only exposure of the control surface.** The gateway never forwards the agent's pipe, and no operation
  reaches the agent that section 9 does not list.

### L1: local socket and pipe

This follows agent.md section 3 and adds a **second, separate endpoint** so that the agent's own pipe never gains a
new kind of client.

| | Windows | Linux, macOS |
|---|---|---|
| Access list or mode | `D:P(A;;GA;;;<user SID>)`, nothing inherited, as the agent. `PIPE_REJECT_REMOTE_CLIENTS`. First instance flag so that a squatter makes the gateway fail to start rather than serve | Directory 0700 and socket 0600 in the agent's private directory, with the agent's checks (a real directory, owned by the user, no group or other bits) |
| Peer credentials, checked by the gateway on every connection | `GetNamedPipeClientProcessId`, then the token of the client by `ImpersonateNamedPipeClient` at identification level only, and the user SID compared with the gateway's. The process image path is recorded for the audit log | `SO_PEERCRED` or `getpeereid`/`LOCAL_PEERCRED`: the user id must equal the gateway's; the process id and its executable path are recorded |
| Check by the sender | The sender asks which process serves the pipe and connects only if it runs as the same user | The same, and the directory checks |

The peer check is a second line behind the access list, and is the line that matters when someone has weakened the
list or the directory (a mistaken `chmod`, a share of the profile).

**The administration endpoint.** Administration (section 5, `admin`) has an endpoint of its own beside the public
one: `\\.\pipe\vss-admin-<user SID>` and `<agent directory>/admin.sock`, with the same access
list, modes and peer check. Studio and `vss_cli control` use it, and nothing else should. A connection to the public
endpoint can never hold `admin`, whatever its settings or its token say, so a harness adapter or a plugin that is
tricked into sending an administration request is refused by construction and not by a setting. Under T2 this is not a
boundary against a hostile program of the same user, which can open either endpoint; it separates the scriptable
surface from the one that changes who may connect.

**What a local connection is.** A connection that passes the peer check is the principal `local:<user>`. Its scope is
a setting of the endpoint (section 5), and its **default is `monitor`**, not `control`, so that enabling the endpoint
for a harness requires one more deliberate step to allow driving. A harness that needs control on L1 either runs
under a setting that grants `control` to `local:<user>`, or presents a token with `control` (level 2 on L1 is
allowed and is the more precise choice).

Because T2 is accepted, granting `control` to every process of the user is what the user is choosing, and the
enable dialog says so in plain words (section 12).

### L2: loopback TCP

- Binds `127.0.0.1` and `::1` only, never a name that could resolve elsewhere. The port is set by the user, and a
  port in use is an error, not a fallback to another.
- A token is **always** required. The peer is not identified: another local user can connect, and on Windows the
  process behind a TCP connection cannot be established without a race.
- DNS rebinding and cross-site requests (T3) do not apply: there is no HTTP, a browser cannot send the preface, and
  there is no `Origin` to honour. If the project later adds a WebSocket form it must check `Origin` and `Host`
  against an allow-list first; this page does not propose one.
- Other users on the computer can reach the port (T1). They need the token. A token is therefore never sent over
  L2 to a host other than loopback, and the sender refuses a non-loopback address for an L2 token.
- The token crosses loopback in clear. That is acceptable only because capturing loopback needs administrator
  rights (T14). A user who wants better chooses L1, or TLS on loopback (an option, the same certificate as L3).

### L3: TLS for the remote case

**Protocol.** TLS 1.3 only, from the first byte (no STARTTLS, so nothing to strip). No renegotiation, no 0-RTT, no
compression. Cipher suites: `TLS_AES_256_GCM_SHA384`, `TLS_CHACHA20_POLY1305_SHA256` and
`TLS_AES_128_GCM_SHA256`; key exchange `X25519` and `secp256r1`. ALPN `vss-control/1`. The server certificate is
ECDSA P-256 (fast, small, supported everywhere), or Ed25519 where every sender supports it.

**Certificates are created on the receiving side, by the user, and pinned by the sender.** There is no certificate
authority involved by default, so there is no public CA that can mis-issue for Studio, and no name to forge.

1. When the user enables L3, the gateway creates a key pair and a **self-signed** certificate: subject
   `CN=Virtual Serial Studio <host name>`, no extensions beyond `basicConstraints CA:FALSE`, `keyUsage
   digitalSignature`, `extendedKeyUsage serverAuth`, and a `subjectAltName` of the addresses the user chose. Validity
   is **13 months**. The private key is stored in the per-user directory with the access list of the token store
   and is never exported.
2. The user creates a **pairing bundle** on the receiving side: a single text string containing the host and port,
   the SHA-256 of the certificate's SubjectPublicKeyInfo (the **pin**), and, optionally, a token. It is shown once
   and copied or scanned by the user to the sending side. It is the out-of-band channel; no pin is ever trusted
   because it arrived over the connection it is meant to protect.
3. The sender stores the pin with the host. On connect it accepts a certificate **only if its public key hashes to
   the pin**. The name, the dates and any chain are ignored for the decision (a pin of the key is stronger than any of
   them), but a certificate whose validity has passed by more than the rotation overlap is refused and reported, so a
   forgotten rotation is visible.
4. If the pin does not match, the sender stops, does not send the token, and says that the certificate has changed.
   It does not offer to trust the new one with a click; replacing the pin is done by pairing again or by a
   rotation (below).

**Rotation.** The key pair is rotated yearly, by the user's action or on a reminder, and on demand after any
suspected leak. Rotation avoids a flag day by publishing the **next** pin early:

- The gateway generates the next key pair 60 days before expiry. After authentication, its `hello` reply carries
  `next_pin`, which the sender stores (the reply travelled over a channel the current pin already authenticated, so
  this extends trust by one hop, no further).
- On the day of rotation the gateway starts using the next key. A sender with `next_pin` stored accepts it silently.
  One without (it was off for the whole window) must pair again.
- After a **suspected leak** nothing is rotated quietly: the key is replaced, tokens issued against the old key are
  revoked, and every sender pairs again. The pairing dialog says so.
- A sender keeps a list of at most two pins per host (current and next); a pin is removed when it is replaced.

**Alternatives.** An organisation with its own CA can provide a certificate and key (level 3): the gateway reads
them from files, and the sender pins either the key (as above) or the CA, by configuration. The default needs no CA.
A **mutual TLS** option, where the sender also presents a client certificate that the receiving side pins, is the
better form for unattended rigs that can store a key; the token stays as the authorisation layer. It is listed as
decision 8 and is not in the first slice.

**TLS library.** The project uses Boost and Qt only, and Boost.Asio's TLS needs OpenSSL. Which library to add, if any,
is decision 1. The GUI and the agent do not link it either way (the gateway is a separate program, section 3).

## 4. Authentication

The principal of every connection is one of: `local:<user>` (L1 peer credentials), `token:<id>`, or
`oidc:<issuer>#<subject>` (level 3). Authentication happens once per connection, in the first request after the
preface, and the result is fixed for the life of the connection.

### Token format

```
vsst1_<id>_<secret>
```

| Part | Content |
|---|---|
| `vsst1` | Format version and a fixed prefix, so that a scanner for leaked secrets (a repository secret scanner, a log filter) can recognise it. A change of format changes the prefix |
| `id` | 8 random bytes, base32 (13 characters, no padding). Public. Looks up the record, is printed in logs and shown in the list |
| `secret` | 32 random bytes from the operating system's CSPRNG (`BCryptGenRandom`, `getrandom`, `getentropy`), base32 (52 characters, no padding). Never logged, never stored |

A token is therefore about 70 characters and has 256 bits of secret. It has no structure to parse and carries no
claims: everything about it (scopes, expiry) is held by the receiving side and can be changed without reissuing it.
Base32 avoids characters that are mangled by shells, URLs and spreadsheets.

### Generation, on the receiving side only

- Tokens are created in **Studio on the receiving computer** (a dialog) or by `vss_cli control token create` there,
  both of which talk to the gateway over the agent's pipe (the local, admin-capable path). The sender cannot ask for
  one.
- **No remote request can create, list, edit or revoke a token.** Token management is only reachable through a
  local connection with the `admin` scope, never through L2 or L3. This one rule removes the largest class of
  escalation: a stolen `admin` token over the network cannot mint more tokens.
- The dialog sets: a label ("Lab rig 3"), the scopes (section 5), an expiry (default **30 days**, maximum 1 year,
  "never" is not offered; a rig that must not expire uses level 3), an optional list of source addresses or
  ranges, and the transports it may use.
- The token is shown **once**, with a copy button and a warning, and is not recoverable. If lost, it is revoked and
  replaced.

### Storage on the receiving side

The record holds the id, the label, the scopes, the dates, the source list, the transports, and
`hash = SHA-256("vsst1-token" ‖ secret)`. SHA-256 is sufficient because the secret has 256 bits of entropy, so a slow
hash (Argon2, bcrypt) adds nothing against guessing and would make the check a denial-of-service lever. The store is a
file in the per-user directory with the owner-only access list or mode of the agent's directory, written to a
temporary file and renamed, with the directory checks of agent.md. The file is not in the support bundle, the topology
or the running setup. A rewrite is done only by the gateway.

At rest, a copy of the store is exactly as exposed as the user's profile: someone who reads it gets hashes, which
cannot be used as tokens. That is the reason for hashing.

### Verification

1. Look the record up by `id`. If there is none, or it is revoked or expired, **still** compute the hash of the
   presented secret and compare it with a fixed dummy value, so that the work done does not tell the caller which of
   the cases occurred.
2. Compare hashes with a constant-time comparison (`CRYPTO_memcmp` or a hand-written loop with no early exit,
   tested for no early exit).
3. Check expiry against the clock, with the **monotonic floor** from editions.md (the latest time seen is stored and
   a clock that goes back does not extend a token).
4. Check that the transport and the source address are allowed for this token.
5. On success the session starts with the token's scopes. The `last used` time of the record is updated at most once
   a minute and is the only field the gateway writes to the store while serving.

All failures return the same reply, `unauthenticated`, with a delay that does not depend on the cause.

### Presentation

The token is sent in the first `hello` request, in a field of the header, **never in a URL, command line argument
or environment variable** of the gateway or of any log. Senders should read it from a file or a secret store; the
C ABI takes it as a byte buffer that it zeroises after use. The pairing bundle may carry it for convenience, and then
the bundle is as sensitive as the token.

Tokens are sent in clear on L1 (the kernel authenticates the pipe, and the token is a second factor) and on L2
(accepted, section 3), and only inside TLS on L3.

### Replay

On TLS 1.3 a captured session cannot be replayed, and a captured token is a stolen credential (section 2). Where a
site wants more, level 3 supplies sender-constrained tokens (DPoP, RFC 9449, or mutual TLS bound tokens, RFC 8705);
the gateway's authenticator interface (section 8) has the data it needs (the certificate hash of the connection). A
challenge-response in place of sending the token would keep the token off the wire entirely but needs the server to
hold the secret instead of its hash; the design chooses hashing at rest and TLS in transit, and records that as
decision 4.

### Rate limiting

| Scope | Limit |
|---|---|
| Per source address | 5 failures, then a delay doubling from 1 s to 5 min; reset after 15 minutes of quiet. A source that is in back-off is dropped before the handshake cost where the transport allows |
| Per token id | The same, so a distributed guess at one id slows down |
| Global | At most 20 failed authentications a second across the gateway; beyond that new connections are refused until it falls |
| Unknown ids | Count against the source only. Guessing a valid id (40 bits) and then the secret (256) is not feasible; the limits exist to keep the cost bounded |

A burst of failures sends one line to the audit log per source per minute with a count, so an attack cannot fill the
log (section 11).

### Revocation

Revoking a token in the dialog or `vss_cli control token revoke <id>`:

- takes effect on the **next request** (the gateway re-checks the record on every request from a session with that
  token, a map lookup), and any open session of that token is closed and audited;
- is persistent and immediate in the store; a revoked id stays in a deny list until its expiry so a restored backup
  cannot bring it back unnoticed (the restored file is older than the deny list's own file only if someone replaced
  both);
- needs no contact with the sender.

"Disable the endpoint" and "revoke all" are separate, one-click actions in the dialog.

## 5. Authorisation

### Scopes

A scope names a class of actions. A token carries a set of scopes; a role is a named bundle shown in the dialog.

| Scope | Allows | Does not allow |
|---|---|---|
| `monitor` | Read the list of ports and links, their state, counters, line states and the event stream **without** bytes; the licence edition and the endpoint's own status; the version | The bytes of any traffic |
| `monitor.data` | Subscribe to traffic bytes and read the history. Optionally limited to named ports (a filter in the token record) | Any change |
| `control.ports` | Create and remove ports, link and unlink, rename, change settings and lines, start and stop | Writing data, faults |
| `control.data` | `send` bytes, play a file the **sender uploads** (never one named by path), set up scripted replies | Anything outside the ports the principal made, if `own ports only` is set |
| `control.faults` | Fault injection, response timing, modem line drops | |
| `control.apply` | Apply a whole topology document (subject to the limits of section 9) | |
| `record` | Start, stop and fetch a recording of a port (section 6) | |
| `admin` | Everything local-only: tokens, listeners and certificates, policy, audit settings, stop the gateway, set the scopes of other principals. **Held only by a connection to the administration endpoint (section 3)**, never by a token, an OIDC mapping or a connection to the public endpoint | |

Roles:

| Role | Scopes | Intended for |
|---|---|---|
| **Monitor** | `monitor` (and `monitor.data` if ticked) | The computer that watches |
| **Control** | `monitor`, `monitor.data`, `control.ports`, `control.data`, `control.faults`, `record` | A test harness |
| **Control plus apply** | Control and `control.apply` | A rig that deploys a topology |
| **Admin** | all | Only the administration endpoint: Studio's window and `vss_cli control` |

Rules:

- **No implied scope.** `control.data` does not include `monitor.data`. A write-only sender is possible.
- **Deny by default.** An operation not in the table of section 9 is refused, and so is any operation whose scope
  the principal lacks. A new operation added in the future starts with a scope that nobody has until a token is
  given it.
- **Ceilings.** The endpoint has a ceiling per transport, set by the user or administrator: for example L3 is
  limited to `Monitor`. A principal's scopes are the intersection of its own and the ceiling. `admin` has a ceiling
  of "L1 only" that cannot be changed from the remote side.
- **One enforcement point.** A single function `authorise(principal, operation, arguments) -> allow | deny(reason)`
  decides every request; handlers do no checking of their own, and the table that feeds it is the one in section 9,
  generated into the handler registration so that an operation cannot be registered without a scope. A test walks the
  registry and fails for an operation without one.
- **Own ports.** `control.*` can be restricted to ports the same principal created (default for tokens given to
  scripts), so a test cannot disturb the user's ports.
- **Defence in depth.** A session that has no `control.*` or `record` scope gets an agent connection with `read_only`
  set; a session without `monitor.data` has its event subscription forced to no data in the gateway. A bug in the
  gateway's check is therefore not enough to write.

## 6. Plugins, identities and the confused deputy

Plugins run out of process (editions.md). Two different kinds of plugin meet the endpoint:

| Kind | Example | How it relates to the gateway |
|---|---|---|
| **Client adapters** | The Python plugin, the Java binding, the LabVIEW wrapper | Clients of the gateway. They hold a credential and send requests. They are not trusted by the gateway; they are in the position of any client |
| **Gateway extensions** | The OAuth 2.0 / OIDC authenticator; later a custom audit sink | Run by the gateway and consulted for a decision. They are trusted to say who a caller is, never what the caller may do |

**Rules that stop a confused deputy (T8, T9):**

1. **The gateway decides authorisation, always.** An authenticator extension returns an identity and *claims*
   (section 8). The gateway maps them to scopes by a table the administrator wrote, then intersects with the
   transport ceiling. The extension cannot name scopes directly.
2. **The caller's scope is the limit, not the carrier's.** A request that a component relays carries the original
   principal. Where a client adapter runs on the receiving computer and forwards a remote user's request, the
   request is checked as the remote user, and again as the adapter's own principal; the smaller set applies.
3. **No credential forwarding.** A plugin never receives a token or a bearer credential that belongs to another
   principal. The gateway terminates authentication.
4. **Manifest permissions are real.** A plugin's manifest names the scopes it may be given. The enable dialog shows
   them; a plugin cannot ask at run time for more; a changed manifest asks again (editions.md ties trust to the file
   hash).
5. **Honest limit.** All plugins and `vss_gate` run as the same user. A malicious same-user plugin can open the
   agent's pipe and bypass the gateway entirely (T2). The rules above stop mistakes and confused deputies, and bound
   what an honest plugin can be tricked into; they do not make a plugin a security boundary. The boundary is the
   trust decision for the plugin (official, signed by a trusted author, or confirmed unsigned), which editions.md
   already requires, and an operating system sandbox or a separate account where a site needs one.

## 7. Authorisation design in code (outline)

```
principal   { kind, id, label, transport, source, scopes }
operation   { name, required_scope, own_ports_only, args_schema }
authorise(principal, operation, args) -> allow | deny(reason)
```

- `principal` is created once after authentication and is immutable; changing a token's scopes closes its sessions.
- The result of `authorise` is audited with the principal id, the operation name and the outcome (never the
  arguments' bytes).
- The table is data (a list in one header), the check is a pure function, and both are covered by exhaustive tests:
  every operation against every role and ceiling, expected as a matrix in the test.

## 8. Level 3: the OAuth 2.0 / OpenID Connect plugin interface

Level 3 is for sites that manage access centrally. It adds nothing to the core's decision rules; it adds a source of
principals.

### The interface

A gateway extension of kind `authenticator` implements, over the same local protocol the plugins already use:

```
authenticate(request) -> result
  request:  { method: "bearer",
              credential: <opaque string, as sent by the client>,
              transport: "l1" | "l2" | "l3",
              source: <address or null>,
              channel: { tls_cert_sha256: <of the client cert, if mutual TLS>, ... } }
  result:   { ok: true,
              subject: "<stable id>", issuer: "<id>",
              claims: { "scope": [...], "groups": [...], "roles": [...], "custom": ... },
              expires_at: <time>,
              session_binding: <optional hash for DPoP or mTLS> }
          | { ok: false, reason: <code> }          // reason is audited; the client sees "unauthenticated"
```

The gateway:

- calls it only after the preface and the frame limits, with a timeout of 3 s and a bound on concurrent calls (the
  extension is a DoS target and a failure is a refusal, never a pass);
- maps `claims` to scopes through the **policy table** (below), intersects with the transport ceiling, and never
  grants `admin` to a network principal;
- ends the session at `expires_at` (it does not extend past the token's own expiry) and re-authenticates on a new
  connection;
- audits subject and issuer, never the credential.

If the extension is missing, crashes or times out, bearer authentication is refused. It never falls back to the
token method for that listener (T12): each listener's `allowed_methods` is configuration (`token`, `bearer`, or
both), and an administrator can set `bearer` only.

### The policy table

An administrator writes, in a file that only `admin` can change (and that can be deployed by their tooling):

```
issuers:
  - issuer:   "https://idp.example.com/"
    audience: "virtual-serial-studio"
    algorithms: ["RS256", "ES256"]
    jwks:     { uri: "…/jwks.json" | file: "C:\…\keys.json", refresh: "1h", stale_if_error: "7d" }
    clock_skew: "60s"
    scope_map:
      - when: { claim: "scp", contains: "vss.monitor" }   -> grant: ["monitor"]
      - when: { claim: "scp", contains: "vss.control" }   -> grant: ["control.ports","control.data","control.faults"]
      - when: { claim: "groups", contains: "lab-rigs" }   -> grant: ["control.ports","control.data","control.faults","record"]
    ceiling: { l3: ["monitor","control.ports","control.data","control.faults","record"] }
```

A token whose claims map to nothing gets no scopes and is refused. A scope that names `admin` in the map is ignored.

### Validating a JWT (offline-capable)

Validation is done in the extension with these rules, and the extension's tests (section 15) target each:

1. **The issuer is configured, not discovered from the token.** `iss` must equal a configured issuer exactly; the
   key set comes from the configuration. The header fields `jku`, `x5u`, `jwk` and `x5c` are **ignored**, never
   fetched or trusted.
2. **Algorithm allow-list.** Only the algorithms in the configuration (default `RS256`, `ES256`, `EdDSA`). `none` is
   never accepted. A symmetric algorithm (`HS*`) is never accepted for a key that is published as a public key
   (the classic confusion); an issuer configured with a shared secret says so explicitly and is separate.
3. **Key selection.** By `kid` from the cached key set; a key whose `alg`/`use`/`key_ops` do not allow signature
   verification is refused.
4. **Claims.** `exp` required and in the future; `nbf` and `iat` honoured with the configured skew (default 60 s);
   `aud` must contain the configured audience; `typ` is checked (`at+jwt` for RFC 9068 tokens) where the issuer sends
   it; `sub` required; `azp`/`client_id` checked if configured; maximum lifetime (`exp - iat`) is capped (default
   one hour) so a long-lived token from a lax issuer is not honoured.
5. **No network in the decision path unless configured.** A token is validated with keys already held.

**Key-set caching.** The extension is the only component that may reach the network for this purpose (the GUI never
does). It fetches the discovery document and `jwks_uri` over TLS only, with the system trust store or a pinned CA
from the configuration, and caches the key set on disk (so a restart works offline):

- refresh at the configured interval, clamped to between 5 minutes and 24 hours, whatever the server's headers say;
- **stale if error**: if a refresh fails, the cached set keeps validating for `stale_if_error` (default 7 days) so a
  rig keeps working through an IdP outage, and the audit log records every day that it does;
- an unknown `kid` triggers at most **one refetch a minute** (otherwise a flood of forged `kid`s turns the gateway
  into a request cannon against the IdP, and a way to evict good keys);
- a static key file (`file:`) makes the whole path **offline**: no fetch at all, for air-gapped labs;
- key rotation at the IdP works through the refresh; a key removed there is no longer accepted after the next
  refresh, which is the revocation delay (below).

**Revocation** at level 3 is by short token lifetime (recommended 5 to 15 minutes for human users, up to an hour for
rigs) and, when online and configured, by token introspection (RFC 7662) on a cache of one minute. Studio's own
revocation list does not apply to bearer tokens; an administrator disables the account at the IdP. This is a
property of OAuth and is stated in the dialog.

### Client credentials for unattended rigs

A rig authenticates to the IdP with its own client secret (or a private-key JWT, RFC 7523, recommended) using the
client credentials grant, receives a short-lived access token, and sends it as the credential. The Studio side
validates it as above; the `sub` is the client id, and a `scope_map` entry gives the rig only what its job needs.
Neither the client secret nor a refresh token ever goes to Studio. For a rig that cannot reach an IdP, the answer is
level 2 (a token) or L1, not an exception here.

### Other flows

OIDC for a human who watches Studio (the authorization code flow with PKCE) is done by the **sender**: Studio only
validates the resulting access token. No sign-in page exists in Studio and none is proposed.

## 9. The API

### Shape

- **Name and version.** `control` API, major version in the preface (`VSSCTL` + `1`) and in the `hello` reply.
  Within a major version changes are additive: new operations, new optional fields, new event kinds, new error
  codes in the documented categories. Clients must ignore unknown fields and event kinds. A breaking change is a new
  major version; the gateway can serve two majors at once for a release so that scripts have a migration window.
  **Unlike the agent protocol, this one is a public contract.**
- **Format.** The agent's framing (length-prefixed, JSON header, blob for bytes), so the C ABI is a thin wrapper and
  bytes stay out of JSON. Requests carry a client-chosen `id`; replies echo it.
- **Naming.** `noun.verb` in lower case: `ports.create`, `ports.list`, `links.connect`, `traffic.subscribe`.
  Identifiers are opaque strings assigned by Studio (`p_7f3a`), stable for the life of a port, never COM numbers or
  paths used as keys. Times are `{ "mono_ns": …, "wall": "<RFC 3339>" }` from one clock.
- **Errors.** `{ "code": "forbidden", "message": "…", "retryable": false }` with codes from a closed list:
  `unauthenticated`, `forbidden`, `invalid_argument`, `not_found`, `conflict`, `unsupported`, `busy`, `timeout`,
  `limit_exceeded`, `cancelled`, `unavailable` (the agent is not running), `internal`, and the engine's errors under `engine.<name>`. No stack, no path, no
  engine internals reach the sender.
- **Idempotency.** Mutating requests accept an `idempotency_key`; a repeat within 10 minutes returns the first
  result, so a harness can retry after a lost reply.
- **Capabilities.** `hello` returns `features` (names), so a client can degrade as the GUI does ("Not available in
  this build"); a missing feature is off.
- **Harness helpers kept in the server**, so a test need not stream bytes to wait: `expect.bytes` (wait for a
  pattern on a port with a timeout), `runs.begin` and `runs.end` (a named run that labels audit and report records),
  and `report.junit` for the XML of the run. Results include timestamps in the common clock, so a report is
  reproducible.

### Operations and scopes

| Operation | Scope | Notes |
|---|---|---|
| `hello` | none (after auth) | Version, features, the principal's effective scopes |
| `status` | `monitor` | Edition, uptime, endpoint state, counters |
| `ports.list`, `ports.get`, `links.list`, `links.get`, `lines.get` | `monitor` | |
| `events.subscribe`, `events.unsubscribe` | `monitor` | State, counters, line changes, and that bytes moved and how many; no bytes |
| `traffic.subscribe`, `traffic.unsubscribe`, `traffic.history` | `monitor.data` | The bytes. Port filter applies |
| `ports.create`, `ports.remove`, `ports.rename`, `ports.settings`, `links.connect`, `links.disconnect`, `lines.set` | `control.ports` | Own ports only, if set |
| `ports.send`, `scripts.set`, `scripts.clear` | `control.data` | Bytes in the blob; sizes capped |
| `faults.inject`, `timing.set` | `control.faults` | |
| `topology.apply`, `topology.snapshot` | `control.apply`, `monitor` | Document only; no file paths from the sender; size capped; a named port or link a client does not own is refused for scoped tokens |
| `expect.bytes` | `monitor.data` | |
| `triggers.subscribe`, `triggers.read`, `expect.trigger` | `monitor`; the decoded context of a trigger needs `monitor.data` as well | "Triggers" below. Port filter applies |
| `runs.begin`, `runs.end`, `report.junit` | `control.ports` | |
| `record.start`, `record.stop`, `record.fetch` | `record` | Section 10 |
| `session.close`, `ping` | none | |

Deliberately absent from the remote surface: `release_port` (ends another process), `create_com0com_pair` and
anything that elevates, `devices` and `facts` (they describe the computer, A3), `run_benchmark` (ties up the
machine), path-taking operations, plugin management, and everything in `admin`. Some may be added with their own
scope when there is a need.

**Local-only administration** (the administration endpoint of section 3): `endpoint.stop`, `listeners.*`,
`tls.rotate`, `tls.pairing_bundle`, `tokens.create/list/revoke`, `policy.*`, `audit.*`, `sessions.list/kick`.

### Triggers

A protocol-decoding plugin (Pro) watches a port and raises a **trigger** when it sees something a test wants to know
about. A harness receives triggers through this API so that a test can wait for one, assert on one, or fail when one
it did not expect arrives. Nothing here is built: it needs the plugin framework to raise triggers and the agent to
carry them, and it lands with slice 4.

**What a trigger is.**

```
{"seq": 4812, "kind": "checksum_mismatch", "port": "p_3", "plugin": "modbus-rtu",
 "time": {"mono_ns": 81234567890, "wall": "2026-11-13T09:30:00.123456Z"},
 "span": {"direction": "rx", "offset": 20480, "length": 8},
 "message": "CRC 0x1A2B, expected 0x3C4D",
 "context": {"unit": 17, "function": 3}}            + blob: the bytes of the span
```

| Field | Who sets it | Rule |
|---|---|---|
| `seq` | The agent | One counter for the whole Studio, starting at 1 for each agent, with no gaps at the source. It is what makes loss visible |
| `kind` | The plugin | One of a closed core list: `framing_error`, `checksum_mismatch`, `unknown_message`, `timeout`, `bus_contention`. A plugin's own kinds are `x.<plugin>.<name>`. A client ignores a kind it does not know |
| `port` | The agent | The port the plugin is attached to. A plugin cannot raise a trigger for a port it does not watch |
| `plugin` | The agent | The id of the plugin's manifest, taken from the connection it arrived on. A plugin cannot name another |
| `time` | The engine | The single clock that stamps traffic and events, read when the trigger is received. A plugin does not supply times |
| `span` | The plugin | Which bytes it is about, as a direction, an offset in that direction's stream and a length, so a harness can line a trigger up with the traffic it recorded |
| `message` | The plugin | Text for a person, cleaned and cut as all text from outside is |
| `context` | The plugin | Decoded fields: a flat object of numbers, booleans and short text, 4 KiB at most. With the bytes of the span it is the part that needs `monitor.data` |

**Scope.** `monitor` is enough to learn that a trigger was raised: its `seq`, kind, port, plugin, time, span and
message. The decoded `context` and the bytes of the span are traffic by another route, so they are sent only to a
session that also holds `monitor.data`, and only for ports its token may see. A session without it gets the same
trigger with those two parts left out and `"redacted": true`. There is no new scope: a monitor that may not read
bytes can still fail a test on a checksum error.

**Three ways to receive them.**

| Operation | For | Guarantee |
|---|---|---|
| `expect.trigger {kinds, ports, timeout_ms, after_seq}` | A test step: "after I send this, a `timeout` trigger arrives on `p_3` within two seconds" | Waited for inside the gateway, like `expect.bytes`. With `after_seq` (the `seq` a previous reply or `status` gave) there is no window between arming and the stimulus: a trigger raised in between is found. Returns the first match, or `timeout` |
| `triggers.subscribe {kinds, ports}` | A harness that fails a test on any unexpected trigger | A stream of batches, in `seq` order. May lose triggers under load and always says so (below) |
| `triggers.read {after_seq, limit}` | Catching up after a loss, and a report at the end of a run | Reads the agent's ring of recent triggers: complete for as far back as the ring reaches, and says where the ring begins when asked for something older |

**Ordering.** Triggers are delivered in `seq` order on every path, and `seq` order is the order in which the engine
received them. Two triggers of one port are therefore in the order the plugin raised them. Between ports, and
against traffic and events, the `time` is what orders them; `seq` does not order a trigger against a byte.

**Loss, and how it is reported.** Triggers have a queue of their own for each subscriber (4096 triggers), separate
from the traffic tap, so a flood of traffic cannot push a trigger out. When the queue is full the oldest is dropped,
as for events. Every batch carries the `seq` of its first trigger and `"lost": {"count": n, "from": a, "to": b}` when
the batch does not follow on from the one before, so a gap is never silent and a harness can fetch exactly the
missing range with `triggers.read`. `runs.end` and `report.junit` give the number of triggers of each kind raised
during the run, counted at the source, so a report can show that a harness saw fewer than there were. The default
for a harness adapter is to treat an unrecovered gap as a failed test.

**Back-pressure.** Nothing a client does slows a plugin or the hub.

- A plugin raises a trigger without waiting for anyone: the agent copies it into the ring and the subscribers'
  queues under a short lock, as the event sink does.
- A subscriber that has not read its last batch is sent no new one; its queue fills and then drops, and counts. A
  client that lets 4 MiB of replies and batches pile up is closed (section 10).
- A plugin that raises more than 1000 triggers a second for one port has the excess replaced by one
  `x.core.trigger_storm` trigger a second that says how many were held back, so a decoder that goes wrong cannot
  fill the ring, the queues or a JUnit report. The storm trigger has a `seq` like any other.
- The ring holds the last 65 536 triggers of the agent. It is memory, not a file; a run that needs every trigger
  for longer than that uses a recording (section 10), which stores triggers beside the bytes.

**What can go wrong (section 2).** A trigger comes from a plugin, and a plugin is not trusted to tell the truth
(T8). A forged or missing trigger changes the result of a test (A1), so: the agent, not the plugin, sets `seq`,
`port`, `plugin` and `time`; the manifest must declare that the plugin raises triggers, and the plugin page shows
it; the plugin's id is in every trigger and in the report, so a result says which decoder it rests on; and an
official decoder is signed (editions.md). A harness cannot raise a trigger through this API at all. Subscriptions
are audited (who, which kinds and ports); single triggers are not, because they are test data and may hold bytes.

## 10. Remote monitoring and denial of service

### Live view or recording: recommendation

Decision 5 of editions.md asks whether remote monitoring sends a live view or a recording. **Recommended: a live
view as the base, a recording as a separate, pull-based operation.**

| | Live view | Recording |
|---|---|---|
| What it is | The sender subscribes; the gateway forwards state changes and, with `monitor.data`, bytes, in batches | The agent writes a capture file (`vss::capture` already runs inside the agent, agent.md section 4); the sender fetches it in chunks, later |
| Fits | Watching a rig now; an operator's dashboard | Reviewing a run after the fact; evidence for a failed test; slow or unreliable links |
| Loss | Bounded and honest: queues drop the oldest and count them, as the agent's tap does | None at the source; the fetch is resumable and checksummed |
| Cost on the receiving side | Bandwidth tied to traffic; capped | A file on disk; capped; deletable |
| Security | A running data flow to a remote host; stopping it is one action | Data at rest in a file; named, size-limited, never a path chosen by the sender |
| Why this order | It is what "watching" means, and the existing tap and back-pressure work | Needs the capture-in-agent work (agent.md PR 5) and a retention policy |

The two share the endpoint, the scopes (`monitor.data` for live, `record` for recording), the audit and the limits.
Live view ships first (section 13); recording follows with agent.md's PR 5. A harness that wants the whole record of
a run uses `record`, and a person watching uses the live view. The recording is written by the **agent** so that it
neither passes through nor depends on the gateway.

### Limits

| Resource | Limit | Why |
|---|---|---|
| Pre-authentication connections | 8 at a time, 5 s each | T6 |
| Authenticated sessions | 8 total, 4 per principal | T10, T11 |
| Frame size | 4 KiB before authentication, 1 MiB after | |
| Request rate | 200 a second sustained, burst 400, per session (token bucket); `limit_exceeded` with a retry hint | |
| Outstanding requests | 32 per session; the gateway stops reading from a session beyond that (agent.md's rule) | |
| Event bandwidth | 4 MiB a second per session, and the agent's queue and back-pressure rules (2048 events, 512 KiB, 4 MiB then close) | A client that never reads is closed |
| Heavy operations (`topology.apply`) | One at a time across the gateway | |
| TLS handshakes | At most 10 a second; beyond that the TCP accept is delayed | The handshake costs CPU |
| Audit log | 10 MiB a file, 5 files kept; bursts rate-limited | A log that cannot be filled |
| Recordings | Size and age limits set by the user (default 256 MiB, 7 days) | |

The gateway being its own process means exhausting it costs the user their remote access, not their ports. The
agent's own limits (16 connections, the pool of two request threads) are the second line. The gateway uses a bounded
number of agent connections (one per session) and handles agent errors by closing the session, never by retrying
without a limit.

## 11. Audit logging and log hygiene

### The log

A JSON-lines file in the per-user directory, owner-only, append-only in the program's behaviour, rotated by size.
Nothing is sent anywhere.

Events: the endpoint enabled or disabled and by whom; each listener started or stopped, with address and
transport; certificate created, rotated; each token created, revoked, expired, with id, label and scopes (never the
secret); each authentication success or failure, with principal id, transport, source address, process id and
image path for L1, and a **reason code** (the client sees only `unauthenticated`); each `forbidden` and the scope
that was missing; each session opened and closed; each mutating operation (name, target id, outcome); the start and
end of a run; configuration changes; the OIDC key refresh and any stale-if-error use; rate-limit trips (summarised).

Never: a token, a secret, a private key, a bearer credential, the bytes of traffic, the contents of a script or a
topology document, or a file path supplied by a sender.

### Tamper evidence

Each line carries `prev`, the SHA-256 of the previous line, so a removed or edited line is detectable by
`vss_cli control audit verify`. This is evidence, not protection (T14). The head hash may be copied by the user to
somewhere else; a site that needs more sends the log to its own collector with a plugin of kind `audit sink`, which is
a gateway extension and not part of this design.

### Preventing leaks (T13)

- Secrets are a type, `secret_string`: it has no stream insertion, no `to_string`, no implicit conversion, zeroes its
  memory in the destructor and is excluded from the JSON writer. The only way to print one is to ask for its
  `id()`. A token is split on receipt into `id` and a secret type and the original buffer is wiped.
- The logger accepts only structured fields, not formatted free text from request data. Request text goes through
  the encoder's cut-and-replace (agent.md, section 4) before it is stored, and strings from a sender are quoted as
  data.
- The support bundle (editions.md) lists the files it includes by name and excludes the token store, the key
  material and the audit log's `authentication` lines' sources unless the user ticks them.
- Crash dumps: the gateway is built so that the token store and key are not held longer than needed, and it sets the
  process to exclude them from dumps where the operating system allows (`MADV_DONTDUMP`, `PROC_SET_PROTECTED` as
  appropriate).
- **Canary tests** (section 15) run the whole endpoint with known token strings and require that none appears in the
  log, the support bundle, a dump of stderr or the error replies.

## 12. Secure defaults, enabling, and what the user sees

### Defaults

| Setting | Default |
|---|---|
| `vss_gate` | Not installed in Community; in Pro, not running |
| Control endpoint | **Off** |
| When turned on | L1 only; scope of `local:<user>` is `monitor` |
| L2 (loopback TCP) | Off; needs a port and at least one token |
| L3 (TLS) | Off; needs a certificate (created), a pairing and at least one token; scope ceiling `Monitor` |
| Token expiry | 30 days |
| Token source list | Empty (any) for L1 and L2; **required** for L3, with the pairing address prefilled |
| Level 3 | Off; no issuer configured |
| Audit | On, cannot be turned off, only sized |
| After a restart | A listener comes back only if the user ticked "Start with Studio". L3 asks for the tick separately and shows it again at the next start |

### Enabling

The one place to turn it on is a **Remote control** page in Studio's settings, following the GUI rules (accessible
names, keyboard access, both themes, 24 px targets, strings in `tr()`), and its counterpart `vss_cli control`. The
sequence is by steps, each one requiring a deliberate action:

1. Switch on the endpoint. The dialog says in plain words: "Programs on this computer running as you will be able
   to monitor Studio. Choose Control to let them change ports." (L1)
2. Add a token, choose scopes and the transport. The dialog shows a one-line summary: "This token can read traffic
   and create ports, for 30 days, from 192.0.2.10 only."
3. For remote: switch on a TLS listener. A warning card explains that another computer will be able to reach this
   one, shows the address, the pin, and the ceiling, and requires typing the word *Enable*.

Nothing is enabled by importing a topology, opening a file, installing a plugin or receiving a message.

### Seeing that it is on

- **A permanent status chip** in the window's status bar and a tray tooltip line: *Remote control: off*, *local only*,
  *local and 2 sessions*, *remote on, 1 viewing*. It is never hidden by a setting, and remote-on uses a distinct
  icon and the warning token colour (a colour from `Theme`, never a literal), plus text so colour is not the only cue.
- **Sessions list** with principal, label, source, transport, scope, since, last request; each row has **Disconnect**
  and the token row **Revoke**.
- **A one-click Stop** that closes every session and listener and is available from the tray.
- **A notice** (a toast, not blocking) on the first connection from a new address, and on any `forbidden`.
- When a session is watching traffic, the tapped port shows a small *watched* badge, so the person at the machine
  knows their bytes are being read.

Level 1 with no connections appears as *local only*, so a person can always tell whether the endpoint is on.

## 13. Implementation slices

After the owner's review. Each slice is reviewable alone, green alone and tested in the project's own framework
(`vss_unit_tests`, `vss_system_tests`, the fuzz harness).

| Slice | What | Depends on |
|---|---|---|
| 1 | `libs/vss_control_proto` (messages, limits, errors, the policy types) and `vss_gate` with the **local endpoint with peer credentials** (L1) and the administration endpoint, the agent client, the audit log, `authorise` with the operation registry and three operations (`status`, `ports.list`, `endpoint.stop`); `vss_cli control status` and `stop`; the tests of section 15 for these. `vss_gate` is started by hand, as `vss_agent` is, until the agent starts on demand (agent.md PR 6). The preface belongs to slice 4 (L2) and `secret_string` to slice 2, where each is first used | agent.md PR 1 |
| 2 | **Tokens and scopes**: the store, generation, hashing, verification, constant-time compare, rate limiting, revocation, expiry; scopes and ceilings; `vss_cli control token`; token-leak canary tests; the full operation table | slice 1 |
| 3 | **Read-only monitor**: `events.subscribe`, `traffic.subscribe` with `monitor.data`, the per-session read-only agent connection, the status chip and sessions list in the GUI (a design change in the GUI, to be reviewed on its own), live view | slice 2; agent.md PR 5 and decision 10 for `read_only` |
| 4 | L2 and the control scopes (`control.*`), `expect.bytes`, runs and the JUnit report; the trigger operations of section 9, once the plugin framework and the agent carry triggers | slice 3 |
| 5 | L3: TLS, certificates, pinning, pairing bundle, rotation | decision 1 (TLS library) |
| 6 | Recording and fetch | agent.md PR 5 |
| 7 | The level 3 authenticator interface, the policy table, and the first OIDC plugin | slice 4, plugin framework |
| 8 | Adapters (Python first, C ABI) | slices 2 to 4 |

Slices 1 to 3 are what is requested after review. Slice 3's GUI part is the large one. Simple, well-specified work
(the audit log writer, the rate limiter, the frame tests, CLI plumbing) can be delegated to a smaller model; the protocol, the
policy check, the token code and the peer-credential code are not delegated.

### What slice 1 built

| Claim | Proven by | Still paper |
|---|---|---|
| Only this user can connect, and the gateway asks the kernel who did | The access list and the modes of both endpoints are read back; `peer()` is asked on every connection before a byte is read, and a test checks what both ends learn | Refusal of a second real user was not run for the gateway itself: the listener that closes another user's connection unseen is the agent's, and its `nobody` test covers it on Linux and macOS. Windows needs a guest with two accounts |
| Nothing is done before `hello`, and every refusal there looks the same | Every operation, an unknown one and three things that are no request are sent first on both endpoints: each gets the same `unauthenticated` reply and a closed connection. A request sent in one write with a refused `hello` is not run; a second `hello` ends the session | Tokens: slice 2 |
| One function decides, and `admin` exists only on the administration endpoint | The whole matrix (4 operations x 256 sets of scopes x 4 transports) against a rule written out by hand; a gateway told to give the public endpoint every scope still refuses `endpoint.stop` there | Ceilings for L2 and L3, own-ports rules |
| A client can slow only itself | Tests for the hello size limit, a wrong length, a silent client, the session and waiting limits, the request allowance, and a client that never reads; a stress case stops the gateway under six busy clients | The request pool has two threads and one agent call can take ten seconds, so two sessions that wait for a hung agent hold up the others' agent requests for that long |
| The audit log records it and cannot be written by a client | Records for start, stop, each session, each refusal and `endpoint.stop` are checked field by field; a client name that tries to end the line and start a forged one stays text inside one record; the chain verifies after every such test; a flood of refusals writes one record and a count | On Windows an audit directory that already exists is accepted when this user owns it; its access list is not read back |
| The API is the public one, not the agent's | Ports and links cross as `p_<n>` and `l_<n>`; an agent error crosses as `unavailable` and nothing else | Engine errors by name, which no operation of this slice can produce |

Not built in this slice, and the reason: `vss_cli control` has `status` and `stop` only, because nothing starts
`vss_gate` on demand yet; the gateway is in this repository and is built with the tools, where the editions page puts
it in the Pro set, because that split is the licensing work's to make; and SHA-256 comes from Boost.Hash2, which
raises the oldest Boost that builds this repository from 1.74 to 1.88.

### What slice 2 built

Tokens over the local endpoint: a `hello` may carry a token, and the connection is then the principal `token:<id>`
with the token's scopes, not the local user.

| Claim | Proven by | Still paper |
|---|---|---|
| A token is made on the receiving side only, and only by administration | `tokens.create`, `tokens.list` and `tokens.revoke` need `admin`, which only the administration endpoint holds; the local user and a token with every scope a token can have are refused all three | Studio's dialog; `vss_cli control token` is the only way to make one |
| Only a hash is kept | The store's file and every file in the gateway's directory are searched for the secret, the whole token, their upper-case and hex forms and half of the secret after a token's whole life; the system test does the same for the output of every process | Crash dumps |
| Every way of being wrong looks the same | A wrong secret, an unknown id, two things that are no token, a revoked token, an expired one and a good token on the administration endpoint each get the same reply, byte for byte in its error; the audit log has the reason for each. Each check hashes exactly once, counted | Time measured with a clock: the test counts work, not nanoseconds |
| The comparison does not stop early | `constant_time_equal` is tested for a difference at every position; that it takes the same time is by construction (no branch on the data), not by measurement | |
| A secret cannot be printed by accident | `secret_string` has no stream insertion and no conversion to or from text; the test file does not compile if it gains one. `expose()` is the one way out | A reviewer still has to read each `expose(` |
| Revoking works at once | The sessions of a revoked token are closed by the request that revokes it, without their sending anything; the token is refused afterwards and after a restart | |
| A token ends | With a clock the test moves: accepted one second before its end, refused at it, a session closed at its next request, and still refused when the clock is set back a day and after a restart | |
| Guessing is slowed down | After the configured number of failures the source and the token id are refused without a check, for a delay that doubles; the limiter's rules are tested with a clock moved by hand | On the local endpoint every connection is the same source (`local:<user>`), so one program's wrong tokens slow every program's tokens. Connections without a token are not affected |
| A damaged token file stops the gateway | Five kinds of damage: the store does not open, the gateway does not start, and the file is left as it was | |

Differences from sections 4 and 5 as first written, and why:

- A revoked token's record is kept in the same file until a week after the token would have ended. There is no
  separate deny list: one file is replaced in one step, so the record and its revocation cannot disagree.
- `transports` is in a record and checked; a list of source addresses is not, because nothing here listens on a
  network yet. It comes with L3.
- A successful check resets the failures of the token's id, not of the source: a caller that holds one good token
  cannot use it to wipe out the delay it earned guessing another.
- A `hello` with a token on the administration endpoint is refused: who may use that endpoint is decided by the
  operating system alone.
- The copies of a presented token that the gateway can reach are wiped (the decoded request, the frame, the read
  buffer, the reply that hands out a new token). The copy inside the frame reader's buffer and the one inside the
  JSON parser are freed without being wiped.

### What slice 3 built

The read-only monitor on the local endpoint: `events.subscribe`, `traffic.subscribe`, their two `unsubscribe`
operations, and `sessions.list` and `sessions.kick` on the administration endpoint. In the agent: the `read_only`
flag of agent.md's decision 10, and the engine's time in every event.

A batch, as a client receives it (the bytes are in the frame's blob, in the order of the events):

```
{"event": "batch", "seq": 12, "revision": 7, "dropped": 0,
 "time": {"mono_ns": 81234567890, "wall_ms": 1794562200123},
 "events": [{"kind": "data", "port": "p_2", "name": "b", "direction": "rx", "size": 5, "stored": 5,
             "mono_ns": 81234000001}]}
```

| Claim | Proven by | Still paper |
|---|---|---|
| A monitor learns that bytes moved and is never given them | Every event of an events stream is checked for bytes; `traffic.subscribe` without `monitor.data` is refused and costs only an error. The gateway asks the agent for bytes only on behalf of a session that has the traffic stream, so a monitor's bytes never reach the gateway | That last part is read in the code, not observed: the agent has no counter of which connections asked for bytes |
| A session that can only look cannot change a port, even if the gateway is wrong about a request | A session whose scopes are `monitor` and `monitor.data` and nothing else connects to the agent with `read_only`, counted at the agent; a session with a control scope and the administration endpoint do not. The agent answers `list` and `subscribe` on such a connection and refuses the five other requests, the one that ends it included | The flag holds a connection to its word, not a user: another program of the same user can connect to the agent without it (agent.md, section 6) |
| A stream names its ports | Two lists on one connection, one for events and one for bytes: nothing about another port or about no port arrives, and bytes arrive only for the ports of the traffic list. A text that is not a port id costs an error and not the session | A list of ports in the token itself: a token cannot yet be held to some ports |
| Loss is bounded, and told | A subscriber that stops reading while a port is busy: events are dropped and counted, the session is not closed for it, the batches it reads afterwards are numbered without a gap and one of them says how many were lost. The same with a small allowance and a reader that keeps up | `dropped` counts before the port filter, so it can include events the subscriber would not have been sent. Which events were lost cannot be read back: events have no ring (triggers will, section 9) |
| Nothing a subscriber does slows a port | The agent's queue and its rule (2048 events, 512 KiB, drop the oldest) are unchanged and tested in the agent; the gateway takes what the agent has sent without waiting and holds at most one turn's batches for a client | A session's requests and its stream share one connection to the agent, so a request the agent takes ten seconds over holds that session's own stream for as long |
| A stream cannot outlive the right to it | A token that ends while only its stream is running ends the session at the stream's next turn, with a clock the test moves; revoking a token ends its stream; a session kicked from the administration endpoint is closed | |
| When the agent goes away the session is told | The session with a stream gets `unavailable` and is closed, with `agent_lost` in the audit log; a session without one is told when it asks, stays, and can subscribe once the agent is back | |
| Times are the engine's | The time of a data event lies between two readings of the steady clock taken round the write; each batch carries the steady clock and the time of day read together | The time of day is a number of milliseconds, not RFC 3339 text. Agent and gateway are on one computer, so they share the steady clock |
| The user can see who is connected and end it | `sessions.list` gives each session's principal, transport, program, process, what it called itself, its streams and what they carried and lost; `sessions.kick` closes one. Both are refused on the public endpoint whatever the scopes, and both are in the audit log | The status chip and the sessions list in Studio (below) |

Differences from sections 9, 10 and 12 as first written, and why:

- **One stream for each connection.** It has two switches, events and traffic, each with its own list of ports, and
  one pace: the last subscription says it. Two streams would need two connections to the agent, and the agent takes
  sixteen.
- **A stream turns at its pace.** The gateway asks its agent connection for batches once every interval, so an event
  reaches a subscriber after at most two intervals (10 ms each at the fastest). The agent has no request that ends a
  stream, so ending both kinds lets the session's agent connection go; the next request makes a new one.
- **The allowance** is 4 MiB of batches a second for each session. Batches never take more than half of what may
  wait unread for one client (4 MiB), which leaves the rest for answers: a busy port cannot get its own watcher
  closed as a slow client.
- **Not built: `traffic.history`.** It needs pages of the hub's ring from the agent (agent.md, PR 5).
- **Not built: the status chip and the sessions list in Studio.** Studio does not talk to the gateway yet, and
  nothing starts `vss_gate` for it (agent.md, PR 6, for the agent). The chip, the dialog and their strings need a
  spec under `docs/gui-design` first; that is a pull request of its own. Until then `vss_cli control sessions` and
  `vss_cli control kick` are how a person sees who is connected and ends it.
- **Engine errors by name**, which slice 1 left on paper, are in: an error event carries `engine.<name>`, and what
  the operating system said becomes `engine.io_error`, without its number or its text.

### What the control operations built

The first part of slice 4, on the local endpoint: `ports.create`, `ports.remove` and `links.connect` under
`control.ports`, and `ports.send` under `control.data`. Loopback TCP, `expect.bytes`, runs and the report are
separate changes.

| Claim | Proven by | Still paper |
|---|---|---|
| Arranging ports and speaking on them are two rights | A session with `control.ports` and no `control.data` makes a port and is refused a send; one with `control.data` alone sends and is refused a port; a monitor, with or without `monitor.data`, is refused all four and the agent is never asked. The same for a token, and a ceiling takes the scope away again | "Own ports only": a caller may change any port of the agent, not only the ones it made |
| What the engine refuses crosses by name only | A port that is not there, a kind the agent does not make and a link to nothing come back as `engine.not_found` and `engine.unsupported` with the gateway's own message; the client library turns the name back into the engine's code. Each costs an error and not the session | |
| An id that is not one is not passed on | Five texts that are no port id get `invalid_argument` for each operation, without the agent being asked, and are not written to the audit log as they came | |
| A send is bounded | 64 KiB is taken and one byte more is answered `limit_exceeded`; nothing to send is `invalid_argument`. The session stays in both cases | A rate for bytes sent: only the request rate of the session limits it (200 a second) |
| Every change is recorded, and the bytes are not | One record for each of the four operations, with who, which port, and the outcome, also when it was refused by the engine or was no request at all; the bytes of a send are searched for in everything the gateway keeps and are not there, only their number | |
| Without an agent nothing is half done | A change says `unavailable` and the session stays; when the agent is back the same session makes a port | |

Not built here, and why: `links.disconnect`, `ports.rename`, `ports.settings` and `lines.set` have no request in
the agent yet; `idempotency_key` waits for the first operation whose repeat would do harm (a second `ports.create`
makes a second port, which a caller can see and remove).

### What `expect.bytes` built

The second part of slice 4: a harness waits, inside the gateway, for a pattern on a port, and can have bytes sent
in the same request. Runs and the JUnit report are a separate change.

```
{"id": 7, "op": "expect.bytes", "port": "p_2", "pattern": "4f4b0d0a", "timeout_ms": 2000,
 "send_port": "p_1"}                                  + blob: the bytes to send
{"id": 7, "reply": "matched", "offset": 6, "mono_ns": 81234000001, "events_lost": 0}
```

| Claim | Proven by | Still paper |
|---|---|---|
| Nothing is missed between sending and listening | The gateway asks its agent connection for the bytes and then for the send, in that order, on the session's own queue. A pattern that the sent bytes produce is found every time, with how many bytes went by before it | Bytes that went by before the request are not seen: there is no ring to look back in. A stimulus from another connection can only be sent after the wait is known to have started, which that connection cannot see |
| A pattern cut by a chunk boundary is found | Another session sends the pattern a byte at a time while this one waits | |
| Waiting is reading, and sending with it is sending | Refused without `monitor.data`; with it and without `control.data` a wait works and a wait that sends is refused before anything is sent, which the ports' counters show | |
| A wait is bounded and ends cleanly | 1 to 256 bytes, 1 ms to 60 s, one wait at a time for a connection (`conflict` for a second). A pattern that does not come is `timeout` and the session stays; a send the engine refuses ends the wait at once with the engine's error | |
| It does not change what the client's own stream carries | With an events stream running, the stream goes on in order through a wait and carries no bytes, although the gateway itself was reading them | A wait keeps the pace of a stream the client already has, so its answer can be two of that stream's intervals late |
| Neither the pattern nor the bytes sent are recorded | One audit record for each wait with the port, the two sizes and how it ended; both texts are searched for in everything the gateway keeps | |

The direction of a pattern (`rx`, the usual, or `tx`) is the engine's: a loopback port reports what is written to
it as received and sends it on to whatever it is linked to.

## 14. Decisions for the owner

All twelve were accepted as recommended.

| # | Question | Options | Recommended | Cost of being wrong |
|---|---|---|---|---|
| 1 | TLS library for the gateway | (a) OpenSSL through Boost.Asio; (b) the platform's own (SChannel, Secure Transport, and OpenSSL on Linux) behind one interface; (c) Qt Network in `vss_gate` only | (a) in the Pro repository, linked by `vss_gate` only. One implementation to test and fuzz; the GUI, agent and CLI stay free of it. The licence audit must cover it | Medium: a new dependency to patch. (b) triples the test matrix |
| 2 | Process layout | (a) a separate `vss_gate`; (b) listeners in the agent | (a) | Medium: (b) puts network code in every user's agent |
| 3 | Default scope of the local user | (a) `monitor`; (b) `control` | (a). The user raises it consciously. A harness uses a token with `control` on L1 | Low, but (b) makes every same-user program a controller after a single click |
| 4 | Token verification | (a) hash at rest, token sent over TLS or a local pipe; (b) challenge-response, secret kept in clear on the receiving side | (a). A stolen store gives nothing usable. (b) is stronger on the wire but stores the secret | Medium |
| 5 | Remote monitoring | live view first, recording as a second operation | As section 10 | Low |
| 6 | Network `admin` | (a) never reachable remotely; (b) allowed by explicit ceiling | (a) | High for (b): a stolen admin token mints tokens |
| 7 | Loopback TCP (L2) | (a) include, off by default; (b) leave out | (a), because LabVIEW and some TestStand set-ups need it, and tokens are required | Low |
| 8 | Mutual TLS for unattended rigs | (a) later; (b) in the first TLS slice | (a) | Low |
| 9 | Token default expiry | 30 days, maximum 1 year, no "never" | As written | Low |
| 10 | Does `read_only` in agent.md ship before slice 3 | Yes, with agent.md PR 5 | Yes: the design relies on it for defence in depth | Medium: without it, the gateway's check is the only line |
| 11 | Level 3: where JWTs are validated | (a) in the authenticator plugin; (b) in the gateway, the plugin only fetches keys | (b) for the first version: validation is the security-critical part and belongs with the core and its tests; the plugin (network, discovery, caching, introspection) stays replaceable. Section 8 describes the same checks for either | Medium |
| 12 | Restore a listener at restart | Only if ticked, L3 asked separately | As section 12 | Low |

## 15. Test strategy

Everything runs without a desktop session in the project's own framework; platform cases live under
`tests/unit/platform/<os>/` and are built only there. Waiting is on conditions with a time limit, never a fixed time.

| Layer | What | |
|---|---|---|
| Wire format | Round trip of every message; every limit (4 KiB before authentication, 1 MiB after, depth 8, text and list sizes); bad preface; truncation at every byte; length fields that lie | Unit |
| **Fuzzing the protocol** | `fuzz_control_frame` and `fuzz_control_message` in the same style as `fuzz_agent_frame` (bytes in pieces; what decodes must encode and decode again to the same value); a pre-authentication harness that feeds arbitrary bytes to a gateway with only the connection handler, to require no crash, no allocation beyond the limit and no operation before authentication. The corpus is replayed in every build; a longer run is scheduled with the other fuzzers (`docs/fuzzing.md`). The JWT parser has its own harness | Fuzz |
| **Authentication bypass** | A matrix: for every operation, every state of connection (no preface, preface only, wrong hello, after failed authentication, expired, revoked mid-session, after `session.close`) is refused with the right code. Operations sent before `hello` in one frame with `hello`. A second `hello` cannot change the principal. A request id of 0 or a reused id. A token with the right id and a wrong secret, the right secret and the wrong id, truncated, with trailing bytes, in the wrong case. Revocation applies to an open session on its next request | Unit and system |
| **Authorisation** | The matrix of section 7: every operation x every role x every ceiling x own-ports setting expected from the table; a test fails if an operation is registered without a scope; the read-only agent connection is verified by attempting a write through a deliberately broken gateway check (a test seam that disables the gateway's own check) | Unit and system |
| Timing | The failure path is the same work for an unknown id, a revoked token and a wrong secret (counted by an instrumented hash call, not by a clock); the comparison has no early exit (tested with a function-pointer hook that counts bytes touched) | Unit |
| Rate limits and DoS | Failures from one source back off as specified; a distributed guess at one id; an accept flood with silent connections; slowloris on the preface; a client that never reads; 10 000 requests in a burst; the log under an attack stays within its size | System |
| **Hostile client** | A client that sends valid frames with hostile content: an enormous port list, deeply nested JSON, strings with invalid UTF-8 and control characters, a port name that looks like a path, ids that do not exist, requests that name a port of another principal, a flood of `subscribe`, `topology.apply` with a document that names a file path. The gateway must refuse or bound each, never crash, and never reach beyond `section 9`'s operations | System |
| Peer credentials and ACLs | The access list and the modes are read back (as for the agent); on Linux and macOS a connection as `nobody` through `sudo` is closed unseen (skipped where `sudo` needs a password); a loopback connection without a token is refused; a non-loopback bind of L2 is refused; an L3 listener with TLS off cannot start | Platform |
| TLS | A pinned certificate is accepted; a different key with the same name is refused and no token is sent; an expired certificate beyond the overlap is reported; `next_pin` rotation succeeds with and without the stored hint; TLS 1.2, 1.3 with an unlisted cipher, renegotiation and 0-RTT are refused; a connection that sends the preface in clear to a TLS listener is dropped | System (when slice 5 lands) |
| JWT and policy | Each rule of section 8 with a vector that breaks it: `alg: none`, `HS256` signed with the public key, wrong `iss`, wrong or missing `aud`, expired, `nbf` in the future, `exp - iat` too long, unknown `kid` (one refetch per minute), a `jku`/`x5u` header that must not be followed, a key set that grows beyond a limit, a stale cache within and beyond `stale_if_error`, the static key file with no network. A claim that maps to `admin` is ignored | Unit |
| **Token-leak checks** | Canary tokens and secrets are used end to end: created, presented, revoked, failed, expired, over L1, L2 and L3, and in each error path. The log, the audit file, every reply, stderr and the support bundle are searched for the canary secret in plain, base32 and hex form; none may appear. The test also greps the source tree for a `secret_string` converted to text. A review check forbids adding a `to_string` to it | System |
| Audit | Every event listed in section 11 is produced by its action; the hash chain detects a deleted, edited and reordered line; rotation keeps the chain across files | Unit |
| Threading | The gateway's sinks capture only shared state; the TSan leg runs a stress case of many sessions, revocations and an agent restart at once (at -O1, as the standard requires) | System |
| Status and consent | The GUI cases: the chip shows each state, the sessions list matches the gateway, Stop closes everything; no listener starts on import or install | GUI |
| What this cannot cover | Two real accounts on Windows (a VM guest with two users, `docs/vm-testing.md`); a real IdP (a test issuer that serves keys is used; a vendor's issuer is a manual check); another computer on a real network, with a hostile switch; a real driver port driven through the endpoint | Manual or VM |

Exit rule for slices 1 to 3: the authentication-bypass matrix, the authorisation matrix, the canary checks, and the
fuzz harness corpus pass on every platform build, and the TSan leg is clean.

## What this design does and does not give

| Claim | Because | Not covered |
|---|---|---|
| Nothing listens by default | `vss_gate` is not running, and the agent opens only its own pipe | |
| Another account on the computer cannot control Studio | Access lists or modes, peer credentials, and tokens on loopback | A second real account on Windows is tested only in a VM |
| A leaked token is limited and can be cut off | Scopes, expiry, source list, revocation on the next request | A token is a bearer secret until revoked |
| A network observer or impersonator learns nothing | TLS 1.3, a pin created at pairing, no fallback | A sender paired over a compromised channel |
| A plugin cannot grant itself more | One enforcement point and a policy ceiling | A same-user plugin can open the agent's pipe (T2) |
| Logs hold no secrets | A type that cannot print, structured logs, canary tests | A bug in a future logging change; the canary test is the guard |
