# The plugin API: every message of the extension points

This page lists the messages that a plugin and the host send each other for the five extension points (generator,
fixture, modifier, adapter, decoder), for parameters and instances, and for recordings and bundle sections. It is for
people who write plugins. [The plugin framework design](plugins.md) says why; this page says exactly what is on the
wire. The definition in code is `libs/vss_plugin/include/vss/plugin/api_*.hpp`. Where this page and the code differ,
the code is right and this page has a mistake.

The messages every plugin uses (`hello`, `hello_ack`, `ping`, `pong`, `status`, `log`, `shutdown`, `bye`, `ok`, `error`)
are in `protocol.hpp` and in section 7.3 of the design, and are not repeated here. Nothing in Studio sends or receives
the messages on this page yet.

## How a message looks

- A message is one frame of the agent's framing: a JSON object (the header) and a blob. The header has at most 64 KiB
  and nests at most 8 deep. A frame has at most 1 MiB.
- The header has `op` (the name of the message, as in the tables), and `id` or `re` where the table says so.
- **Kind.** A *request* has an `id` (a whole number from 1 to 9007199254740991) and no `re`. It is answered with `ok`,
  with an `error`, or with the answer message the table names. An *answer* has a `re` that repeats the request's `id`,
  and no `id`. A *notification* has neither, and is never answered. A message that is not as its kind says is refused.
- **From.** `host` means the host sends it to the plugin, and `plugin` the other way. A receiver refuses a message that
  comes from the wrong side.
- **Bytes** are in the blob and never in JSON. A message that has bytes says so (a member called `data` in the
  tables); every other message must have an empty blob. The size of the blob is limited as the table says.
- **Whole numbers** are JSON integers. Every instance, port, link, sequence number and time is from 0 or 1 up to
  9007199254740991 (2^53 - 1), as the table says. Times with `_ns` are nanoseconds on the host's clock, and `_us` are
  microseconds.
- **Text** is UTF-8 with no control character and no mark that changes the direction of text, and has the length limit
  the table gives, in bytes. Text is never markup and never a format string. A *name* is a text of 1 to 128 bytes. A
  *token* is lower-case letters, digits, dots, hyphens and underscores.
- **Lists** have the limit the table gives, and it is checked before any entry is read.
- **A JSON object as text** (`params`, `args`, `settings`, `result`) is written as one JSON object inside the header. A
  number with a fraction is written in the form the JSON writer uses (0.5 is written `5E-1`), and a number that would
  not read back as the same value is refused.
- **A member the reader does not know** is ignored, so that a later minor version may add optional members (section 7.2
  of the design). A member that is known but wrong (the wrong type, out of range, too long, missing when required) makes
  the whole message `bad_request`. Two members with the same key in one object are refused before the message is read.
- **Defaults.** A member marked optional, or with a default, may be left out.
- Whether a message is allowed at all depends on the permissions the user granted and on what the manifest declares.
  A message that is not allowed is answered with `not_granted` or `not_declared`. These checks are the host's, and a
  reader of the frame cannot make them.


## Instances and parameters

Design: 7.3, 7.8. `instance` is a whole number from 1 to 9007199254740991, which the host gives an instance when it makes
it. Each kind has its own message that makes an instance (`generator.start`, `fixture.create`, `modifier.create`,
`decoder.create`); `instance.create` is the generic form that section 7.3 names. A refused parameter is the common
`error` answer to `params.changed`, with the value `refused` and a message that says why. There is no message to save or
restore the state of an instance: the plugin keeps its own files in its data directory.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `instance.create` | host | request | `instance`; `kind` (`generator`, `fixture`, `modifier`, `adapter` or `decoder`); `type` (token of 1 to 64 bytes: the `id` of the extension point in the manifest); `params` (JSON object, default `{}`) | 7.3 |
| `instance.stop` | host | request | `instance` | 7.3 |
| `params.changed` | host | request | `instance`; `params` (JSON object: the parameters that changed, already checked against the manifest) | 7.3, 7.8 |


## Generators

Design: 7.4. The host starts with up to 64 KiB of credit and tops it up as the engine takes bytes. A plugin that writes
more than it has credit for is answered with `busy`, and is ended as misbehaving if it goes on; the reader cannot see
credit, so this is the host's check. The timing class of a generator (`rules` or `async`) is in the manifest, and a
`generator.pattern` from an instance that did not declare `rules` is answered with `not_declared`.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `generator.start` | host | request | `instance`; `port` (the endpoint, whole number from 1); `params` (JSON object, default `{}`) | 7.4 |
| `generator.write` | plugin | notification | `instance`; the bytes in the blob, 1 to 65536 bytes (`max_generator_write`) | 7.4 |
| `generator.credit` | host | notification | `instance`; `bytes` (whole number, 1 to 65536: `max_generator_credit`) | 7.4 |
| `generator.pattern` | plugin | request | `instance`; the bytes of the pattern in the blob, 1 to 1024 bytes (`max_pattern_bytes`); `repeat` (0 to 4294967295, default 1; 0 is without end); `period_us` (0 to 60000000, default 0); `burst` (1 to 4096, default 1); `jitter` (microseconds, 0 to 60000000, default 0) | 7.4 |
| `generator.stop` | host | request | `instance` | 7.4 |
| `generator.done` | plugin | notification | `instance`; `reason` (plain text of 1 to 200 bytes) | 7.4 |


## Fixtures and rule sets

Design: 6.3, 6.4, 7.5, 7.9, and [response timing](response-timing.md) for the rule set.

`tests/unit/test_plugin_api_fixture.cpp`. Design: `docs/plugins.md` sections 6.3, 6.4, 7.5, 7.9. `instance` is a whole
number 1 to `max_api_number` in every message that has one.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `fixture.create` | host | request | `instance` (1..max_api_number); `endpoint` (whole number 1..max_api_number); `params` (JSON object text, default `{}`) | 7.5 (line 579) |
| `fixture.data` | host | notification | `instance`; `ts_ns` (whole number 0..max_api_number); blob `data` 1..8192 bytes (`max_fixture_data`) | 7.5 (line 580) |
| `fixture.line` | host | notification | `instance`; `ts_ns` (0..max_api_number); `dtr` (bool, required); `rts` (bool, required); no blob | 7.5 (line 581) |
| `fixture.reply` | plugin | notification | `instance`; blob `data` 1..4096 bytes (`max_fixture_reply`, equal to `rules::max_reply_bytes`, static_assert in the source); `after_us` (optional whole number 0..max_api_delay_us, `std::optional<std::uint64_t>`) | 7.5 (line 582), 6.3 (line 416) |
| `fixture.line.set` | plugin | notification | `instance`; `dtr` (optional bool); `rts` (optional bool), at least one of the two present; `after_us` (optional whole number 0..max_api_delay_us); no blob | 7.5 (line 583) |
| `fixture.fault` | plugin | notification | `instance`; `fault_kind` (text, one of `parity`, `framing`, `overrun`, `break`, `hang_up`, `disconnect`; C++ enum `line_fault`); no blob | 7.5 (line 584) |
| `rules.set` | plugin | request | `instance`; the rule set is the blob `data`: UTF-8 JSON, 1..max_api_blob (262144) bytes, one well formed JSON object, nesting at most `rules::max_document_depth` (16), no key twice, valid UTF-8. Decode does not check the meaning of the rules | 7.5 (line 585), 6.3 |
| `rules.result` | host | answer (re = the id of the `rules.set`) | `accepted` (bool, required); only when not accepted: `rule` (optional whole number 0..255, the zero-based rule number from a problem path that starts `rules[N]`; absent when the problem is not in a rule), `path` (optional text 0..256, such as `rules[2].do[0].delay`), `reason` (text 1..200, plain). When `accepted` is true `rule`, `path` and `reason` must be absent (refused if present); no blob | 7.5 (line 585: "The reply says ...") |
| `rules.event` | host | notification | `instance`; `ts_ns` (0..max_api_number); `rule` (text 1..64, plain: the name of the rule); `action` (whole number 0..15: the zero-based number of the action in the rule's action list); no blob | 7.5 (line 586) |

Public function in the header: `rules_result_message check_rule_set(const rules_set_message&)`. It copies the blob to
text, runs `vss::rules::compile_rule_set` with a problem list and builds the `rules.result`. The first problem gives
`path` and `reason`; both are cut to their limits and every byte that is not printable ASCII becomes `?`, so the answer
always reads back. `rule` is taken from a path that starts `rules[N]` with N below 256. If the compiler gives an error
and no problem the reason is "the rule set is not accepted". A thrown exception gives "the rule set could not be
checked". Nothing else in the header includes a rules header (the library links `Vss::rules` privately).

Also in the header, for the modifier group: `enum class line_fault` (`parity`, `framing`, `overrun`, `break_signal`,
`hang_up`, `disconnect`), `line_fault_names` (an inline constexpr array of spelling and value, the form `choice_of`
takes) and `to_string(line_fault)`.


## Behaviour modifiers

Design: 7.6, 7.9. A batch and its result are matched by `seq`.

`tests/unit/test_plugin_api_modifier.cpp`. Design: `docs/plugins.md` sections 7.6 and 7.9. The header includes
`api_fixture.hpp` for `line_fault` and `manifest.hpp` for `timeout_action`.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `modifier.create` | host | request | `instance` (1..max_api_number); `link` (1..max_api_number); `direction` (`a_to_b` or `b_to_a`; enum `modifier_direction`); `params` (JSON object text, default `{}`); `budget_us` (1..1000000, `max_modifier_budget_us`); `max_batch_bytes` (1..65536, `max_modifier_batch`); `max_batch_us` (1..1000000, `max_modifier_batch_us`); `on_timeout` (`fail_open` or `fail_closed`; `timeout_action` of manifest.hpp); `flush_on` (optional whole number 0..255, `std::optional<std::uint8_t>`) | 7.6 (line 644, 650-651) |
| `modifier.batch` | host | notification | `instance`; `seq` (1..max_api_number); `ts_ns` (0..max_api_number, the time of the first chunk); `chunks` (list of 1..256 objects, `max_modifier_chunks`): each `offset` (0..65536), `length` (1..65536), `ts_ns` (0..max_api_number); blob `data` 1..65536 bytes (`max_modifier_batch`), the bytes of all chunks. Rules: the length of the list is checked before any entry is read; every chunk lies inside the blob (checked without a sum that can wrap); chunks are in order and do not overlap (a gap is allowed); `ts_ns` does not go back and the first equals the `ts_ns` of the message | 7.6 (line 645) |
| `modifier.result` | plugin | notification | `instance`; `seq` (1..max_api_number); `actions` (list of 1..256): one object per chunk, in order, each with a text `action` from the closed set below; blob `data` 0..max_api_blob (262144) bytes, the result blob. Every range lies inside the result blob | 7.6 (line 646, 655-659) |
| `modifier.stop` | host | request | `instance`; no blob | 7.6 (line 647) |

Actions (the `action` member, `modifier_action_kind`; the other members of the object are those of its kind, and a
member of another kind is ignored on reading and not written):

| `action` | Other members |
|---|---|
| `pass` | none |
| `drop` | none |
| `replace` | `offset` (0..262144), `length` (0..262144): a range of the result blob |
| `delay_us` | `us` (1..max_api_delay_us) |
| `split` | `parts` (2..16, `max_modifier_parts`), `gap_us` (0..max_api_delay_us) |
| `insert_before` | `offset` (0..262144), `length` (1..262144): a range of the result blob |
| `insert_after` | `offset`, `length` as `insert_before` |
| `fault` | `fault` (one of the `line_fault` spellings: `parity`, `framing`, `overrun`, `break`, `hang_up`, `disconnect`) |

Public function: `bool modifier_result_within_bounds(std::size_t batch_bytes, const modifier_result_message&) noexcept`
is true when the result blob is at most 4 times `batch_bytes` plus 1024 bytes (`modifier_result_extra`) and the sum of
`delay_us` and of the split gaps is at most `max_api_delay_us` (one minute). A split with `parts` parts has
`parts - 1` gaps and each counts. It does not wrap for any value of the struct. It does not check that the number of
actions equals the number of chunks (the host knows the batch) or the ranges (the decoder did).


## Automation adapters

Design: 7.7.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `agent.call` | plugin | request | `agent_op` (token, 1 to 64 bytes of lower-case letters, digits, dots, hyphens, underscores; required); `args` (JSON object text, default `{}`); `data` (0 to `max_api_blob` bytes, in the blob) | 7.7 |
| `agent.result` | host | answer | `result` (JSON object text, default `{}`); `data` (0 to `max_api_blob` bytes, in the blob) | 7.7 (name chosen, see Choices) |
| `agent.subscribe` | plugin | request | `data` (bool, default false; a flag in the header, not the blob); `places` (list of 0 to 256 names, each plain text 1 to `max_api_name` bytes; empty or absent is every place the grant covers) | 7.7, 19.4 |
| `adapter.state` | plugin | notification | `listening` (bool, required); `address` (plain text 1 to 256 bytes; required when `listening` is true, refused when false); `protocol` (plain text 1 to 64 bytes, required) | 7.7 |

Free function: `api_message make_plugin_unavailable(std::uint64_t re)` returns the `error` answer with category
`vss.plugin.api`, value `plugin_unavailable` and the message `The plugin is not running.` (made with `make_error`).

Constants: `max_agent_op` = 64, `max_subscribe_places` = 256, `max_adapter_address` = 256, `max_adapter_protocol` = 64.

Details of reading: a bad `agent_op` says `agent.call.agent_op: must be 1 to 64 lower-case letters, ...`; a place entry
says `agent.subscribe.places[2]: ...`; an `address` given with `listening` false says
`adapter.state.address: must not be given when listening is false`. `agent.subscribe` has no blob.


## Recordings, timelines and bundle sections

Design: 19.4, 19.5, 19.6, 20.8.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `capture.start` | plugin | request | `places` (list of 1 to 256 names, each plain text 1 to `max_api_name` bytes, none twice; required); `limit_bytes` (1 to 268435456, default 67108864); `limit_seconds` (1 to 3600, default 600); `ring` (whole minutes 0 to 60, default 0 is not a ring); `snapshot_on` (optional object: `kinds` list of 1 to 16 names of the closed list, none twice; `places` list of 0 to 256 names); `before_s`, `after_s` (whole seconds 0 to 600, default 0). `snapshot_on`, `before_s` and `after_s` are refused when `ring` is 0 | 19.4, 19.6, 20.4, 20.8 (c) |
| `capture.stop` | plugin | request | none | 19.4 |
| `capture.status` | plugin | request | none | 19.4 |
| `capture.result` | host | answer | `running` (bool); `bytes`, `seconds`, `dropped` (0 to `max_api_number`); `ring` (bool); all required | 19.4, 19.6 (name chosen, see Choices) |
| `timeline.load` | plugin | request | `instance` (1 to `max_api_number`); `recording` (plain text 1 to 1024 bytes); `place` (plain text 1 to `max_api_name` bytes); `time_scale` (number 0.01 to 100, default 1.0); `seed` (optional whole number 0 to `max_api_number`) | 19.4 |
| `bundle.prepare` | host | request | `section` (token 1 to `max_api_name` bytes: lower-case letters, digits, dots, hyphens, underscores); `settings` (JSON object text, default `{}`) | 19.4, 19.5 |
| `bundle.result` | plugin | answer | `files` (list of 0 to 16 objects, required): `name` (plain file name, 1 to 128 bytes, no `/`, no backslash, not `.` or `..`, no control character, none twice) and `size` (0 to 268435456); the sizes add up to at most 268435456 | 19.5 (name chosen, see Choices) |

Structs: `capture_start_message` (with `std::optional<snapshot_spec> snapshot_on`, and `before_s`, `after_s` as members of
the message itself), `snapshot_spec` (`kinds`, `places`), `capture_stop_message`, `capture_status_message`,
`capture_result_message`, `timeline_load_message`, `bundle_prepare_message`, `bundle_file` (`name`, `size`),
`bundle_result_message`.

Constants: `max_capture_places` = 256, `max_capture_bytes` = 268435456, `default_capture_bytes` = 67108864,
`max_capture_seconds` = 3600, `default_capture_seconds` = 600, `max_capture_ring_minutes` = 60, `max_snapshot_kinds` = 16,
`max_snapshot_seconds` = 600, `min_time_scale` = 0.01, `max_time_scale` = 100.0, `max_recording_path` = 1024,
`max_bundle_files` = 16, `max_bundle_file_name` = 128, `max_bundle_section_bytes` = 268435456, and the array
`snapshot_kinds` (framing, unknown_type, unknown_value, check, length, timeout, sequence, contention, line_error,
decoder_lost, custom).

Details of reading: `capture.start.snapshot_on: is accepted only when ring is not 0` (the same for `before_s` and
`after_s`; a member that is present is refused even when it is 0). A time scale is checked for reading back exactly:
`encode_as` refuses a `time_scale` whose text would read back as a different double with this library's JSON reader
(it parses numbers with the default precision, which is not always exact). A name that is not plain text is refused as
`...: must be plain text`; a control character in a file name of `bundle.result` is refused by that check or by the file
name check.


## Decoders, the decode stream and triggers

Design: 20.3 to 20.7.

`tests/unit/test_plugin_api_decoder.cpp`. Branch `agent/api-decoder`.

Every name is a text (plain, with no control character or direction mark) unless it says otherwise. "name" is 1 to
`max_api_name` (128) bytes. "label" is 1 to `max_decoder_name` (64) bytes. A whole number is 0 to `max_api_number`
unless a range is given.

| Message (op) | From | Kind | Members (type, limit) | Design section |
|---|---|---|---|---|
| `decoder.create` | host | request | `instance` (1..max_api_number); `places` (list of 1..64 objects, no `place` twice): `place` (name), `rate` (optional, whole number 1..100,000,000, `max_place_rate`; 0 in the struct is "not given"), `format` (optional, plain text 1..16; empty in the struct is "not given"), `bus` (optional bool, default false); `params` (optional JSON object, default `{}`). No blob | 20.6 (table, line 2044) |
| `decoder.data` | host | notification | `seq` (1..max_api_number); `items` (list of 1..256 objects, each with `item` of `data`, `line`, `line_error`, `bus_fault`, `rule`, `time` or `lost`; any other kind is refused); the blob is the bytes of all `data` items, 0..262144 (`max_decoder_blob`), and the `length`s of the `data` items must add up to its size exactly. No `instance` | 20.4 (lines 1925-1949), 20.6 (line 2045) |
| item `data` | | | `place` (name), `direction` (`rx`/`tx`), `offset`, `t_ns`, `end_ns` (optional, not before `t_ns`), `length` (1..65536) | 20.4 (line 1930) |
| item `line` | | | `place` (name), `t_ns`, `lines` (object of optional bools `dtr`, `rts`, `cts`, `dsr`, `dcd`, `ri`), `changed` (list of 0..6 of those names, none twice, each given in `lines`) | 20.4 (line 1931) |
| item `line_error` | | | `place` (name), `t_ns`, `kind` (`parity`, `framing`, `overrun`, `break`) | 20.4 (line 1932) |
| item `bus_fault` | | | `link` (name), `sender` (name), `t_ns`, `fault` (`overlap`, `too_soon`, `line_held`, `transmitter_off`, `overrun`), `offset`, `length` (1..65536) | 20.4 (line 1933) |
| item `rule` | | | `place` (name), `t_ns`, `rule` (label), `action` (label) | 20.4 (line 1934) |
| item `time` | | | `t_ns` | 20.4 (line 1935) |
| item `lost` | | | `place` (name), `direction` (`rx`/`tx`), `from_offset`, `bytes` (1..max_api_number), `from_ns`, `to_ns` (not before `from_ns`) | 20.4 (line 1936) |
| `decoder.ack` | plugin | notification | `seq` (1..max_api_number). No `instance` | 20.4 (line 1947), 20.6 (line 2046) |
| `decoder.frame` | plugin | notification | `instance` (1..max_api_number, added); `place` (name); `direction` (`rx`/`tx`); `offset`; `length` (1..65536); `t_start_ns`; `t_end_ns` (not before `t_start_ns`); `message` (label); `summary` (optional, plain text 0..200); `fields` (optional list of 0..64 objects): `name` (label), `offset` (from the start of the frame), `length` (0..65536), `value` (plain text 0..200, required but may be empty); each field lies within the frame (`offset + length` not past `length`) | 20.6 (line 2047) |
| `trigger.publish` | plugin | notification | `instance` (1..max_api_number, added); `kind` (`framing`, `unknown_type`, `unknown_value`, `check`, `length`, `timeout`, `sequence`, `contention`, `line_error`, `decoder_lost`, `custom`); `code` (token 1..64, given exactly when `kind` is `custom`); `when_ns`; `place` (name); `direction` (`rx`/`tx`); `sender` (optional name); `protocol` (optional): `id` (plain text 1..128), `version` (plain text 1..64); `frame` (optional): `offset`, `length` (1..65536), `t_start_ns`, `t_end_ns` (not before `t_start_ns`), `message` (plain text 0..64, required); `field` (optional): `name` (label), `offset`, `length` (1..65536); `expected`, `actual` (optional plain text 1..128, both or neither); `covers` (optional list of exactly two whole numbers, the second not before the first); `cause` (optional object with exactly one of `bus_fault` (one of the five fault names) with a required `event` (whole number), or `own_action` (whole number)); `request` (optional): `offset`, `length` (1..65536), `t_end_ns`; `text` (plain text 1..200). No `id`, `from` or publish time; no blob | 20.6 (line 2048), 20.7 (lines 2065-2102) |
| `decoder.stop` | host | request | `instance` (1..max_api_number). No blob | 20.6 (line 2049) |

Structs: `decoder_item` is `std::variant<decoder_data_item, decoder_line_item, decoder_line_error_item,
decoder_bus_fault_item, decoder_rule_item, decoder_time_item, decoder_lost_item>` in the order of the table.
`decoder_line_item::lines` is a `std::array<std::optional<bool>, 6>` indexed by `decoder_line`.
`trigger_publish_message::cause` is `std::optional<std::variant<trigger_bus_cause, trigger_own_action>>`.
No change was made to `api_codec.hpp/.cpp`.


## Limits that are named in the code

Every limit above is a named constant in the header of its group, so that a plugin written in C++ can use it. A limit
that the design does not give was chosen as a conservative value, and the design may change it in a later minor version
by raising it. A plugin must not depend on a limit being exactly the value shown here beyond what it needs to stay
under it.
