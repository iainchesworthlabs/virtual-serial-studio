# The plugin API: every message of the extension points

This page lists the messages that a plugin and the host send each other for parameters and instances, the five
extension points (generator, fixture, modifier, adapter, decoder), and recordings and bundle sections. It is for
people who write plugins. [The plugin framework design](plugins.md) says why; this page says exactly what is on the
wire. The definition in code is `libs/vss_plugin/include/vss/plugin/api_*.hpp`, and a test checks that every message
of the code has a row here. Where this page and the code differ, the code is right and this page has a mistake.

The API version is 1.1. Version 1.1 added the three messages of the section "Licence" and the permission `licence.read`;
a plugin that needs them writes `"min": "1.1"` in its manifest.

The messages every plugin uses (`hello`, `hello_ack`, `ping`, `pong`, `status`, `log`, `shutdown`, `bye`, `ok`, `error`)
are in `protocol.hpp` and in section 7.3 of the design, and are not repeated here. Nothing in Studio sends or receives
the messages on this page yet.

## How a message looks

- A message is one frame of the agent's framing: a JSON object (the header) and a blob. The header has at most 64 KiB
  and nests at most 8 deep. A frame has at most 1 MiB. A message is written only when its header is inside that limit,
  and every list and name limit below is set so that a message at all of its limits is.
- The header has `op` (the name of the message, as in the tables), and `id` or `re` where the table says so.
- **Kind.** A *request* has an `id` (a whole number from 1 to 9007199254740991) and no `re`. An *answer* has a `re` that
  repeats the request's `id`, and no `id`. A *notification* has neither, and is never answered, so nothing can be
  refused or answered with `busy` for it. A message that is not as its kind says is refused.
- **Answered by.** Every request is answered with `ok`, with an `error`, or with the message the column names. The
  `error` answers are those of section 7.3: `not_granted`, `not_declared`, `unsupported`, `refused` and the others.
- **From.** `host` means the host sends it to the plugin, and `plugin` the other way. A receiver refuses a message that
  comes from the wrong side.
- **Bytes** are in the blob and never in JSON. A message that has bytes says so (the tables name the blob); every
  other message must have an empty blob. The size of the blob is limited as the table says.
- **Whole numbers** are JSON integers. An instance, a sequence number and a time are from 0 or 1 up to
  9007199254740991 (2^53 - 1), as the table says. Times with `_ns` are nanoseconds on the host's clock, and `_us` are
  microseconds.
- **Places.** A port, a link or an endpoint is always named by the name the hub gives it, as text, in a member called
  `place` (or a list called `places`). A name is plain text of 1 to 128 bytes. A list of places never holds a name
  twice. No message uses a number for a port or a link.
- **Paths.** A path of a recording or of a file of a bundle section is inside the plugin's data directory and follows
  the rules of a path in a manifest (no `..`, no drive, no reserved device name, at most 240 bytes). Two paths of one
  list never differ only in case.
- **Text** is UTF-8 with no control character and no mark that changes the direction of text, and has the length limit
  the table gives, in bytes. Text is never markup and never a format string. A *token* is lower-case letters, digits,
  dots, hyphens and underscores.
- **Lists** have the limit the table gives, and it is checked before any entry is read.
- **A JSON object** (`params`, `args`, `settings`, `result`) is written compactly, as one JSON object inside the
  header. A number with a fraction in it is written the way the JSON writer writes it (0.5 is `5E-1`), and a number
  that would not read back as the same value is refused.
- **A member the reader does not know** is ignored, so that a later minor version may add optional members (section 7.2
  of the design). A member that is known but wrong (the wrong type, out of range, too long, missing when required) makes
  the whole message `bad_request`. Two members with the same key in one object are refused before the message is read.
- **A message is written only as it is read.** What is written is read back, and a message that would read back as
  something else (a member that its kind does not use, a JSON object with spaces in it) is not written.
- **Defaults.** A member marked optional, or with a default, may be left out.
- Whether a message is allowed at all depends on the permissions the user granted and on what the manifest declares,
  and a host also keeps rates, credit and the numbers of its own. These checks are not made by reading a frame.

## Instances and parameters

Design: 7.3, 7.8. `instance` is a whole number from 1 to 9007199254740991, which the host gives an instance when it makes
it. Each kind has its own message that makes an instance (`generator.start`, `fixture.create`, `modifier.create`,
`decoder.create`); `instance.create` is the generic form that section 7.3 names. A refused parameter is the common
`error` answer to `params.changed`, with the value `refused` and a message that says why. There is no message to save or
restore the state of an instance: the plugin keeps its own files in its data directory.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `instance.create` | host | request | `ok` | `instance`; `kind` (`generator`, `fixture`, `modifier`, `adapter` or `decoder`); `type` (token of 1 to 64 bytes: the `id` of the extension point in the manifest); `params` (JSON object, default `{}`) | 7.3 |
| `instance.stop` | host | request | `ok` | `instance` | 7.3 |
| `params.changed` | host | request | `ok`, or `error` with a reason | `instance`; `params` (JSON object: the parameters that changed, already checked against the manifest) | 7.3, 7.8 |

## Generators

Design: 7.4. An instance is made in two steps, for every kind and not only for a generator. The host first sends
`instance.create`, with the `kind` and the `type` (the `id` of the extension point in the plugin's manifest) and the
parameters, and the plugin answers `ok`. Only then does the host send `generator.start` for that `instance` (or
`fixture.create`, `modifier.create` or `decoder.create` for the other kinds). That is where a plugin learns which type of
its own an instance is. An adapter has no start message of its own, so `instance.create` is all it gets.

The host starts with up to 64 KiB of credit and tops it up as the engine takes bytes. A `generator.write`
is a notification, so a write without credit is not answered: the host drops the bytes and counts them, and ends a
plugin that keeps going. The timing class of a generator (`rules` or `async`) is in the manifest, and a
`generator.pattern` from an instance that did not declare `rules` is answered with `not_declared`.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `generator.start` | host | request | `ok` | `instance`; `place` (the port, a name); `params` (JSON object, default `{}`) | 7.4 |
| `generator.write` | plugin | notification | none | `instance`; the bytes in the blob, 1 to 65536 bytes | 7.4 |
| `generator.credit` | host | notification | none | `instance`; `bytes` (1 to 65536) | 7.4 |
| `generator.pattern` | plugin | request | `ok` | `instance`; the bytes of the pattern in the blob, 1 to 1024 bytes; `repeat` (0 to 4294967295, default 1; 0 is without end); `period_us` (0 to 60000000, default 0); `burst` (1 to 4096, default 1); `jitter` (microseconds, 0 to 60000000, default 0) | 7.4 |
| `generator.stop` | host | request | `ok` | `instance` | 7.4 |
| `generator.done` | plugin | notification | none | `instance`; `reason` (plain text of 1 to 200 bytes) | 7.4 |

## Fixtures and rule sets

Design: 6.3, 6.4, 7.5, 7.9, and [response timing](response-timing.md) for the rule set. A rule set is the blob of
`rules.set`: it is a JSON document that does not fit the header. The reader checks only that it is one JSON object of
valid UTF-8 that nests at most 16 deep and has no key twice. The host checks what it means with the rule compiler and
answers with `rules.result`.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `fixture.create` | host | request | `ok` | `instance`; `place` (the port, a name); `params` (JSON object, default `{}`) | 7.5 |
| `fixture.data` | host | notification | none | `instance`; `ts_ns`; the bytes in the blob, 1 to 8192 bytes | 7.5 |
| `fixture.line` | host | notification | none | `instance`; `ts_ns`; `dtr` (true or false); `rts` (true or false) | 7.5 |
| `fixture.reply` | plugin | notification | none | `instance`; the bytes in the blob, 1 to 4096 bytes; `after_us` (optional, 0 to 60000000: the engine holds the bytes for that long) | 7.5, 6.3 |
| `fixture.line.set` | plugin | notification | none | `instance`; `dtr` (optional true or false); `rts` (optional true or false), at least one of the two; `after_us` (optional, 0 to 60000000) | 7.5 |
| `fixture.fault` | plugin | notification | none | `instance`; `fault` (`parity`, `framing`, `overrun`, `break`, `hang_up` or `disconnect`) | 7.5, 6.3 |
| `rules.set` | plugin | request | `rules.result` | `instance`; the rule set in the blob, 1 to 262144 bytes | 7.5, 6.3 |
| `rules.result` | host | answer | none | `accepted` (true or false); when not accepted: `rule` (optional, 0 to 255: the number of the rule that the first problem is in, counted from 0), `path` (optional text of 0 to 256 bytes, such as `rules[2].do[0].delay`), `reason` (plain text of 1 to 200 bytes). When accepted, none of the three | 7.5 |
| `rules.event` | host | notification | none | `instance`; `ts_ns`; `rule` (plain text of 1 to 64 bytes: its name); `action` (plain text of 1 to 64 bytes: the kind of the action, as response timing names it, such as `reply`, `lines`, `mark` or `drop`) | 7.5 |

`rules.set` replaces the instance's rule set. When the answer says it was not accepted, the old set stays.

## Behaviour modifiers

Design: 7.6, 7.9. A batch and its result are matched by `seq` and are notifications, so a lost result is a timeout of the
host, and a late result for an old `seq` is dropped.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `modifier.create` | host | request | `ok` | `instance`; `place` (the link, a name); `direction` (`a_to_b` or `b_to_a`); `params` (JSON object, default `{}`); `budget_us` (1 to 1000000); `max_batch_bytes` (1 to 65536); `max_batch_us` (1 to 1000000); `on_timeout` (`fail_open` or `fail_closed`); `flush_on` (optional, 0 to 255: a byte that ends a batch) | 7.6 |
| `modifier.batch` | host | notification | none | `instance`; `seq` (from 1); `ts_ns` (the time of the first chunk); `chunks` (list of 1 to 256): each `offset` (from 0), `length` (1 to 65536) and `ts_ns`; the bytes of all the chunks in the blob, 1 to 65536 bytes | 7.6 |
| `modifier.result` | plugin | notification | none | `instance`; `seq`; `actions` (list of 1 to 256, one for each chunk, in order, each as below); the bytes that actions refer to in the blob, 0 to 263168 bytes | 7.6 |
| `modifier.stop` | host | request | `ok` | `instance` | 7.6 |

The chunks of a batch lie inside the blob, in order and not overlapping; their times do not go back and the first is
the message's `ts_ns`. An action is an object with a text `action` and the members of its kind. A member of another kind
is not written, and is ignored when read.

| `action` | Other members | Effect |
|---|---|---|
| `pass` | none | The chunk goes on unchanged |
| `drop` | none | The chunk is dropped |
| `replace` | `offset`, `length` (0 or more): a range of the result blob | The range replaces the chunk |
| `delay_us` | `us` (1 to 60000000) | The engine holds the chunk for that long |
| `split` | `parts` (2 to 16), `gap_us` (0 to 60000000) | The chunk goes on in that many parts with that gap between them |
| `insert_before`, `insert_after` | `offset`, `length` (1 or more): a range of the result blob | The range is sent before or after the chunk |
| `fault` | `fault` (as for `fixture.fault`) | The fault is raised as the chunk passes |

The host bounds a result by what its actions can send on: the batch itself and the length of every `replace` and insert,
whether or not their ranges are the same range of the blob, at most four times the batch and 1 KiB, and the delays and
the gaps of splits at most one minute in all. A result outside the bounds fails the whole batch.

## Automation adapters

Design: 7.7, 9.1. When a plugin is not running, a request for it is answered with the `error` value `plugin_unavailable`.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `agent.call` | plugin | request | `agent.result` | `agent_op` (token of 1 to 64 bytes: the agent's operation); `args` (JSON object, default `{}`); optional bytes in the blob, 0 to 262144 bytes | 7.7 |
| `agent.result` | host | answer | none | `result` (JSON object, default `{}`); optional bytes in the blob, 0 to 262144 bytes | 7.7 |
| `agent.subscribe` | plugin | request | `ok`; events follow as `agent.event` | `data` (true or false, default false: whether the events carry bytes; needs `traffic.tap`); `places` (list of 0 to 128 names, none twice; none is every place the grant covers) | 7.7, 19.4 |
| `agent.event` | host | notification | none | `subscription` (the `id` of the `agent.subscribe`); `kind` (token of 1 to 64 bytes); `place` (a name); `t_ns`; the bytes of the event in the blob, 0 to 65536 bytes | 7.7 |
| `adapter.state` | plugin | notification | none | `listening` (true or false); `address` (plain text of 1 to 256 bytes, given when listening and not otherwise); `protocol` (plain text of 1 to 64 bytes) | 7.7 |

## Recordings, timelines and bundle sections

Design: 19.4, 19.5, 19.6, 20.8. The host names a recording: `capture.start` may give a `name`, and `capture.result` says
the path of the file the host writes and how large it is so far.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `capture.start` | plugin | request | `ok` | `places` (list of 1 to 128 names, none twice); `name` (optional: the stem of the file's name, 1 to 64 bytes, no slash, by the rules of a manifest path name; the host chooses one when it is left out); `limit_bytes` (1 to 268435456, default 67108864); `limit_seconds` (1 to 3600, default 600); `ring` (whole minutes, 0 to 60, default 0 is not a ring); `snapshot_on` (optional object: `kinds`, a list of 1 to 16 of `framing`, `unknown_type`, `unknown_value`, `check`, `length`, `timeout`, `sequence`, `contention`, `line_error`, `decoder_lost`, `custom`, none twice; `places`, a list of 0 to 128 names, none twice); `before_s`, `after_s` (0 to 600, default 0). `snapshot_on`, `before_s` and `after_s` are refused when `ring` is 0 | 19.4, 20.8 |
| `capture.stop` | plugin | request | `ok` | none | 19.4 |
| `capture.status` | plugin | request | `capture.result` | none | 19.4 |
| `capture.result` | host | answer | none | `running` (true or false); `bytes`, `seconds`, `dropped` (0 or more); `ring` (true or false); `recording` (a path inside the data directory, 1 to 240 bytes); `size` (0 or more: the bytes of the recording on disk so far); `failed` (optional, true when the recording ended because it could not be written, such as a full disk: what it holds may be incomplete) | 19.4, 19.6 |
| `timeline.load` | plugin | request | `ok` | `instance`; `recording` (a path inside the data directory, 1 to 240 bytes); `place` (a name); `time_scale` (0.01 to 100, default 1; a number that does not read back as itself is refused); `seed` (optional, 0 or more); `include_sent` (optional, default false: whether the bytes the recorded place sent are played too) | 19.4 |
| `bundle.prepare` | host | request | `bundle.result` | `section` (token of 1 to 128 bytes); `settings` (JSON object, default `{}`) | 19.5 |
| `bundle.result` | plugin | answer | none | `files` (list of 0 to 16): each `name` (a path inside the section's folder, by the rules of a manifest path, 1 to 128 bytes) and `size` (0 to 268435456); the sizes add up to at most 268435456 bytes | 19.5 |

The section's folder is `bundles/<section id>/` under the plugin's data directory, by convention. The plugin writes the
files of a section there before it answers `bundle.prepare`, and each `name` of `bundle.result` is a path inside that
folder. The host reads the files from there; the plugin is not told where the bundle is saved.

## Decoders, the decode stream and triggers

Design: 20.3 to 20.7. The host does not wait for a decoder. `decoder.data` carries items in the order the engine saw them,
and a decoder acknowledges what it has taken so that the host sends at most 256 KiB that is not acknowledged. Loss is in
the stream, as a `lost` item.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `decoder.create` | host | request | `ok` | `instance`; `places` (list of 1 to 64, no `place` twice): each `place` (a name), `rate` (optional, 1 to 100000000 bits per second), `format` (optional, 1 to 16 bytes, such as `8N1`) and `bus` (optional, default false); `params` (JSON object, default `{}`) | 20.6 |
| `decoder.data` | host | notification | none | `instance`; `seq` (from 1); `items` (list of 1 to 128, each as below); the bytes of all `data` items together in the blob, 0 to 262144 bytes, and the `length`s of the `data` items add up to its size | 20.4, 20.6 |
| `decoder.ack` | plugin | notification | none | `instance`; `seq` | 20.4, 20.6 |
| `decoder.frame` | plugin | notification | none | `instance`; `place`; `direction` (`rx` or `tx`); `offset`; `length` (1 to 65536); `t_start_ns`; `t_end_ns` (not before `t_start_ns`); `message` (plain text of 1 to 64 bytes); `summary` (optional, plain text of 0 to 200 bytes); `fields` (optional list of 0 to 64): each `name` (1 to 64 bytes), `offset` (from the start of the frame), `length` (0 to 65536) and `value` (plain text of 0 to 200 bytes), lying inside the frame | 20.6 |
| `trigger.publish` | plugin | notification | none | `instance`; `kind` (`framing`, `unknown_type`, `unknown_value`, `check`, `length`, `timeout`, `sequence`, `contention`, `line_error`, `decoder_lost` or `custom`); `code` (token of 1 to 64 bytes, given exactly when `kind` is `custom`); `when_ns`; `place`; `direction`; `sender` (optional name); `protocol` (optional: `id`, 1 to 128 bytes, and `version`, 1 to 64 bytes); `frame` (optional: `offset`, `length` 1 to 65536, `t_start_ns`, `t_end_ns` not before it, `message` 0 to 64 bytes); `field` (optional: `name` 1 to 64 bytes, `offset` and `length` 0 to 65536, from the start of the frame); `expected` and `actual` (optional, both or neither, 1 to 128 bytes); `covers` (optional: two whole numbers, the first and the last byte, from the start of the frame); `cause` (optional, one of `fault` (a bus fault name) with its `event`, or `own_action`); `request` (optional: `offset`, `length` 1 to 65536, `t_end_ns`); `text` (plain text of 1 to 200 bytes). `field` and `covers` need a `frame` and lie inside it. The host stamps `id`, `from` and the time of publishing, so they are not members | 20.6, 20.7 |
| `decoder.stop` | host | request | `ok` | `instance` | 20.6 |

The items of `decoder.data` are objects with a text `item` that is one of these. Any other is refused.

| `item` | Members |
|---|---|
| `data` | `place`; `direction` (`rx` or `tx`); `offset` (the bytes at that place and direction before this chunk); `t_ns`; `end_ns` (optional, not before `t_ns`); `length` (1 to 65536) |
| `line` | `place`; `t_ns`; `lines` (an object of optional true or false for `dtr`, `rts`, `cts`, `dsr`, `dcd` and `ri`); `changed` (list of 0 to 6 of those names, none twice, each given in `lines`) |
| `line_error` | `place`; `t_ns`; `kind` (`parity`, `framing`, `overrun` or `break`) |
| `bus_fault` | `place` (the link); `sender` (a name); `t_ns`; `fault` (`overlap`, `too_soon`, `line_held`, `transmitter_off` or `overrun`); `event` (its id, which `cause.event` of a trigger names); `offset`; `length` (1 to 65536) |
| `rule` | `place`; `t_ns`; `rule` and `action` (plain text of 1 to 64 bytes each) |
| `time` | `t_ns` |
| `lost` | `place`; `direction`; `from_offset`; `bytes` (1 or more); `from_ns`; `to_ns` (not before `from_ns`) |

## Control sessions

Design: 7.7, and `docs/remote-control.md` for the gateway's own protocol. A plugin never holds a token for the control
gateway and never opens a connection to it. It asks the host for a session; the owner is asked in Studio and must allow
it each time; the host then makes a token that lasts 24 hours, speaks to the gateway itself and passes frames both ways
unchanged. `admin` is never given. A plugin that has a session must keep reading: a plugin that lets more than 4 MiB wait
is ended.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `control.session.request` | plugin | request | `control.session.opened` | `scopes` (list of 1 to 16 scope names of the control API, each 1 to 64 bytes, never `admin`); `peer` (optional: who the plugin says is behind the session, text of 0 to 64 bytes, shown to the owner as the plugin's claim); `reason` (optional: text of 0 to 256 bytes, shown as the plugin's own words). One request at a time and at most 4 sessions. Errors: `bad_request`; `not_granted` (the owner refused, `admin` was asked for, or the program has nobody to ask); `busy`; `plugin_unavailable` (no gateway runs, or it did not answer in time) | 7.7 |
| `control.session.opened` | host | answer | none | `session` (1 or more: names the session from here on); `scopes` (the scopes the session has); `expires_ms` (when it ends, in milliseconds since 1970-01-01 UTC) | 7.7 |
| `control.frame` | plugin | notification | none | `session`; `frame` (one request header of the gateway's protocol as a JSON object, with the plugin's own `id`); the request's bytes in the blob. A frame that says `hello`, that is not a header of the gateway or that is for a session that is not open ends the session | 7.7 |
| `control.frame` | host | notification | none | `session`; `frame` (one reply or event batch of the gateway, unchanged); the gateway frame's bytes in the blob | 7.7 |
| `control.session.close` | plugin | notification | none | `session` | 7.7 |
| `control.session.closed` | host | notification | none | `session`; `code` (`revoked`, `expired`, `gateway_closed`, `refused_frame` or `host_stopped`); `reason` (one plain line for a log, at most 256 bytes). A plugin acts on the code and never on the words | 7.7 |

## Secrets shown to the user

A plugin has no window, and its status text and log lines are kept, so a secret such as a pairing code must never
travel in them. The plugin hands the secret to the host, which shows it once in a dialog of its own and keeps it nowhere.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `secret.show` | plugin | request | `ok` | `label` (what the secret is, plain text of 1 to 64 bytes, such as `Pairing code`); `text` (the secret: 1 to 512 printable characters of ASCII, space to tilde); `expires_s` (how long it is good for, 1 to 3600). Answered `ok` when the user has seen it. Errors: `bad_request`; `not_granted` (the user closed it without looking, or the program cannot show it); `busy` (one at a time for each plugin) | 7.7 |

## Licence

Design: `plugins.md` 8.7 and `licensing.md`. An official plugin checks the licence itself and never takes the host's
word, so the host hands it the bytes of the licence file and the plugin tells the host what it found. These messages need
the engine capability `licence_file`. An official plugin is always answered. A plugin from anyone else is answered only
when it declared the permission `licence.read` and the user granted it, because the file holds the user's name; without
it the answer is the `error` value `not_granted`. A host that names the capability never answers `unsupported`. Added in
version 1.1 of the API.

| Message (op) | From | Kind | Answered by | Members (type, limit) | Design section |
|---|---|---|---|---|---|
| `licence.get` | plugin | request | `licence.file` | none; no blob | 8.7 |
| `licence.file` | host | answer | none | `present` (true or false, required); the bytes of the licence file, exactly as on disk, in the blob: 1 to 65536 bytes when `present` is true, none when it is false | 8.7 |
| `licence.report` | plugin | notification | none | `reason` (required: `ok`, `no_licence`, `expired`, `not_yet_valid`, `clock_suspect`, `malformed`, `newer_version`, `bad_signature`, `revoked`, `wrong_computer` or `not_in_licence`, the reasons of the licence library); optional `licence_id` (plain text of 1 to 64 bytes) | 8.7 |

**What a plugin may assume of the bytes: nothing.** They are untrusted until the plugin has verified them with the
licence library. The host does not judge them for the plugin. The plugin sends a reason by its name and no text of its
own, and Studio shows its own sentence for the reason. The host does not act on a report: it asked its own question
(section 8.7), and a report changes nothing in it.

## Engine capabilities

`hello_ack.capabilities` lists, by name, what this build of the engine can do. A plugin checks the list for what it
needs and does without when it is not there: an operation that needs a missing capability is answered with an `error`
whose value is `unsupported`. These are the names:

| Capability | What it says | Design section |
|---|---|---|
| `engine_rules` | The engine runs rule sets for a port (`rules.set`) | 6.6 |
| `rules_link` | Rule sets can be put on a link | 6.6 |
| `rules_microseconds` | Delays in rule sets are exact to the microsecond | 6.6 |
| `route_hook` | The engine has the stage on a route that a modifier uses | 7.6 |
| `events_bus_fault_detail` | A bus fault event has its value, link and time | 20.4 |
| `timeline` | The engine plays a recording as a timeline (`timeline.load`) | 19.4 |
| `licence_file` | The host sends the licence file to an official plugin, or to one that has the permission `licence.read`, when it asks (`licence.get`) | 8.7 |

Studio offers `timeline` from release candidate 7. A host without it answers `timeline.load` with an `error` whose value
is `unsupported`, and a plugin that replays a recording checks for the capability and shows that it cannot.

What Studio does with `timeline.load`: it needs `ports.write` and an instance of kind generator or fixture that the
plugin holds and runs. It checks the recording whole, as the open format of section 19.6 says, then reads it as it plays,
a block ahead, so a recording of any size plays. It plays the `data` lines the recorded place received (`R`) onto the
port named by `place`, one write for each line, so the chunk boundaries and times of the recording are kept. Each is at
its recorded time multiplied by `time_scale`, counted from the `ok`. It is never early, and the latest lateness is
counted. Where the port follows control lines it also sets CTS, DSR, DCD and RI as the recording has them after each
recorded line change. The recorded place is the one named like the port, or the only place of a recording that has one.
It answers `refused` for a recording that is missing, breaks the format or has no fitting place, and for a port the
plugin may not write to; `busy` while the instance plays another. Not played, and counted by the host: faults, and line
changes on a port that does not follow control lines. The bytes the recorded place sent (`W`) are the program's, not the
device's, so they are counted and not played unless the request has `include_sent` true, which plays them too in recorded
order. A read that fails part way ends the replay after what was read. `seed` has no effect, since a
replay has no random choice.

## Not built yet

The design names these, and this version of the API has no message for them.

| What | Design section |
|---|---|
| `inject` and `fault` for an async modifier | 7.6 |
| Progress and cancel of `bundle.prepare` | 19.5 |
| The `clock` of `hello_ack` | 7.3 |
| Typed forms of `triggers.query`, `trigger_stats` and `update_rules` | 20.4, 20.8 |
| A message from the host to a plugin for a user starting a recording. A recorder is an `adapter` instance, made with `instance.create` | 19.1, 19.4 |
