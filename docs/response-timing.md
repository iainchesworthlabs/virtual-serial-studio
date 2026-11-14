# Response timing rules (design)

This page answers open decision 1 of [the editions proposal](editions.md): how
response timing is declared to the engine, so that the fast cases run without a
round trip to a plugin. It is a design for the owner to review. Nothing here is
built except the benchmark that produced the numbers in
[Measured evidence](#measured-evidence).

## The decision

The fast cases do not go through a plugin. A harness or plugin hands the engine
a **rule set**: a small, declarative, bounded program that says what to match,
what to do and when. The engine checks it, compiles it, and carries it out on its
own threads, with no round trip to anything. So a plugin is never in a path
measured in microseconds, such as the turn-around of an RS-485 bus. The plugin
hears about every rule that fired, with a timestamp, after the fact.

A plugin can sit in the path of the bytes. A behaviour modifier
(`docs/plugins.md`, section 7.6) is given the bytes of one direction of a link
and answers with what is to go on, and each batch of bytes costs it a round
trip. That is for changes that have the time. Where a direction of a link has an
`inline` rule set and modifiers, the rule set runs first and the modifiers
follow (see [Where a rule set sits](#where-a-rule-set-sits)), so what the rules
do never waits for a plugin.

Three things make this the answer and not just one option:

1. The measurements below. A reply decided in another process arrives about
   15 ms after its request with today's protocol, because events leave the agent
   in batches. The crossing itself is cheap (about 65 µs each way for a C++
   client), so a faster channel could bring that down to a fraction of a
   millisecond, but only for a plugin that is running, is written in a fast
   language and is not paused; none of that is the engine's to promise. Inside
   the engine a reply with no delay leaves about 0.1 ms after its request. The
   measurements also found that every *timed* action in the engine today is 10
   to 15 ms late on Windows, which this design has to fix whichever way the
   decision goes (see [Timers](#timers)).
2. A rule that the engine holds keeps working when the plugin is slow, stopped
   in a debugger, or dead.
3. A rule set is data. It can be checked before the run, limited in size, saved
   with the results and run again with the same seed. Code in a plugin cannot.

The rest of the page is the model, what the engine promises for it, and where
the line between "a rule" and "plugin code" falls. The plugin framework's own
design (`docs/plugins.md`, on its own branch) covers the plugin side: its tier
T0 is a rule set of this page, its T1 is the `ask` action, and its T2 is a
plugin that only watches and sends commands.

## What is true today

Read from the code, because the design builds on it.

| Fact | Where | What it means here |
|---|---|---|
| A link's delay and jitter are whole milliseconds, drawn uniformly, one timer and one strand for each route; due times never go backwards | `libs/vss_core/src/delay_line.cpp` | The shape (a queue, one timer, its own strand, weak handlers, `close()` that waits for a write) is the one the rule scheduler reuses. The unit is too coarse for a turn-around time |
| The jitter generator of a delay line is seeded from `std::random_device` and cannot be given a seed | the same file | A run with link jitter cannot be repeated today |
| `vss::responder` is a scripted device already: masked byte patterns matched as a stream, a reply after a delay with jitter, a poll list, an optional seed | `libs/vss_studio/src/responder.cpp` | It is the starting point, and it is replaced by the fixture of slice 3. It has no counts, sequences, faults, line changes, events or limits, and it cannot be changed while it runs |
| The responder hears bytes as events on the dispatcher strand, after routing, and times a reply from the event's `when` | the same file, `endpoint::on_read` | Its reply waits behind every other event in the hub. A busy capture delays a reply |
| A data event's `when` is read when the event is published, after the chunk was routed, not when the chunk was read | `endpoint::emit_data` | "End of the frame" has no exact timestamp today |
| `hub_options::clock` lets a test move time by hand, and the half-duplex bus uses it. The delay line, the responder's queue and every event's `when` read the steady clock directly | `hub::now`, `bus_wire.cpp`, `delay_line.cpp`, `timed_queue.hpp` | There is one clock type but several places that read it. A hand-moved clock does not move a delay line |
| `std::uniform_int_distribution` and the other standard distributions give different numbers on different standard libraries | the C++ standard | A seed alone does not repeat a run on another platform |
| A half-duplex bus knows when a chunk's last byte leaves the wire, judges a reply that comes inside the turn-around time (`bus_fault::too_soon`), and delivers a chunk at the end of its time on the wire | `bus_line.cpp`, `bus_wire.cpp` | Exact turn-around stays there. A rule's delay is measured from the same instant, so the two agree |
| Line errors, hang-up and disconnect are `studio::inject_fault`, for a fixture and for a Windows driver port | `libs/vss_studio/include/vss/studio/studio.hpp` | A rule's fault action calls the same code, with the same per-port limits |
| The agent sends events in batches, at an interval the client chooses, 10 ms at the least | [the agent](agent.md), section 4 | A plugin that answers bytes from outside waits for a batch first |

## Where a rule set sits

A rule set is attached to one place, in one of three roles. The role says which
bytes it sees and where what it sends goes.

| Role | Attached to | Sees | Sends | Stands for |
|---|---|---|---|---|
| `peer` | A port a program has open | The bytes the program wrote (the endpoint's received bytes) | By writing to the port, so the program reads them | The device on the far end of the port. What `vss::responder` does today |
| `station` | A fixture | The bytes delivered to the fixture over its links | As the fixture, so the bytes travel its links like any others | A device on a link or a bus, among other ports |
| `inline` | One direction of one link (a source and a target) | The bytes passing | Nothing of its own; it passes, holds, changes or removes what passes | A fault or a delay on the wire |

A place has at most one rule set. A `station` on a half-duplex bus is the case
that matters most for timing: its replies are judged by the line (rate,
turn-around, collision) exactly as a real member's are.

An `inline` set runs after the owner policy (bytes the policy refused are never
seen) and before the half-duplex line and the link's delay line. So a link's
`delay_ms` is added after an inline rule's own delay, and a byte an inline rule
removed never reaches the wire or the counters of the line.

A plugin modifier (`docs/plugins.md`, section 7.6) is a second user of the same
stage. When one direction of a link has both, the `inline` set runs first and
the modifiers follow, in the order of the link's plugin bindings. So a rule
matches the bytes the program sent, at the time the engine read them, and what a
rule set does never depends on a plugin being fast, slow or stopped. The stage
keeps the bytes of a direction in order from one user to the next: when a
modifier is removed, bytes a rule still holds go straight on when they are due,
and bytes the modifier had not yet answered for are passed on unchanged first.

## The rule model

A rule set is one JSON document, apart from the topology file, whose schema does
not change. It replaces the responder script (there are no deployed scripts to
keep reading, so there is no compatibility reader).

```json
{
  "version": 1,
  "name": "meter 17",
  "attach": { "port": "meter-17", "role": "station" },
  "seed": 7,
  "late_after_us": 500,
  "slots": { "reading": "00 00 12 34" },
  "states": ["normal", "busy"],
  "rules": [
    {
      "name": "read holding registers",
      "when": { "bytes": "11 03 ?? ?? 00 02 ?? ??", "state": "normal" },
      "sequence": [
        { "do": [
            { "reply": ["11 03 04", { "slot": "reading" },
                        { "crc16_modbus": "reply" }],
              "after": "match_end",
              "delay": { "normal": { "mean_us": 3500, "sd_us": 400,
                                     "min_us": 1750, "max_us": 8000 } } } ] },
        { "do": [
            { "reply": ["11 83 06", { "crc16_modbus": "reply" }],
              "delay": { "fixed_us": 2000 },
              "fault": { "corrupt": { "at": 1, "xor": "01" } } },
            { "set_state": "busy" } ] }
      ],
      "then": "repeat",
      "count": 0
    },
    {
      "name": "busy until DTR drops",
      "when": { "lines": { "dtr": false }, "state": "busy" },
      "do": [ { "set_state": "normal" } ]
    }
  ],
  "timeline": [
    { "at_us": 5000000, "do": [ { "lines": { "dcd": false } } ] }
  ]
}
```

### What a rule matches

| Match | Meaning |
|---|---|
| `bytes` | A pattern of fixed length, 1 to 256 bytes. Each byte is hex, `??` (any byte) or has one `?` nibble (`1?`, `?3`). A `mask` beside it does the same bit by bit, as the responder's does |
| `gap_us` beside `bytes` | The pattern counts only if nothing arrived for this long before its first byte, after its last byte, or both (`"gap": "before"`, `"after"`, `"both"`). This is how a frame that ends in silence (Modbus RTU) is told from the same bytes inside a longer one |
| `lines` | An input line of the place took this value (an edge, not a level). On a `peer` these are the program's DTR, RTS and BREAK |
| `port` | `opened` or `closed`: a program opened or closed the port, where the platform can tell (the peer-open capability) |
| `quiet_us` | Nothing has arrived at the place for this long since the last byte. It fires once for each silence. With a state it is a timeout: a request moves the set to `waiting`, and silence in `waiting` is the reply that never came |
| `bus_fault` | The half-duplex line gave a verdict other than a clean transmission: `overlap`, `too_soon`, `line_held`, `transmitter_off`, `overrun` or `any`, with `"from": "self"`, `"other"` or `"any"` (the default) for whose bytes they were |
| `signal` | The plugin that owns the set raised the named signal (see [Triggers from a plugin](#triggers-from-a-plugin)) |
| `state` | The rule is considered only while the set is in this state |

Bytes are matched as a stream, so a pattern that arrives in two reads is found.
A match is decided when its last byte arrives. Of the rules that complete on the
same byte, the longest pattern wins (it started first), then the rule listed
first; the match uses up the bytes, so every partial match is dropped. What
matches therefore never depends on how the bytes were split into reads, which
the responder's rule (the earliest start among the bytes in hand) did. A rule
that needs silence after its pattern is decided when the silence has passed, and
loses to a rule that completes on the same byte without that need. A rule with
no `bytes` (lines, port) uses up nothing, and of those the first listed that
matches a change is the one that fires.

Patterns have a fixed length on purpose. The matcher is a table walked one byte
at a time, with no backtracking and a known amount of memory, so its cost for
each byte has a bound that does not depend on the traffic. A frame whose length
is in the frame, or whose reply needs a calculation the list below does not
have, is matched on its fixed start and answered from a slot the plugin keeps
filled (see [Staying one step ahead](#staying-one-step-ahead)), or it stays in
the plugin.

### Triggers from a plugin

A decoding plugin reads traffic and understands it: framing, fields, a checksum
that failed, a message type it does not know, a reply out of sequence. It
watches and is not a modifier, so it learns of bytes after they have passed.
When it decides something is worth acting on it raises a **signal**, and a rule
that matches the signal does the acting: a fault, a line change, a reply. This
is how a trigger fires an engine action without the plugin doing I/O of its own.

```json
{ "name": "answer a bad checksum with a break",
  "when": { "signal": "checksum_failed", "state": "armed" },
  "do": [ { "line_error": "break", "after": "cause",
            "delay": { "fixed_us": 20000 } } ] }
```

`raise_signal(set, name, cause_ns, bytes)` is one operation of the plugin's
connection. The rules for it:

* **The clock.** `cause_ns` is the time of the event the plugin decoded, copied
  from that event, so it is on the run's one clock and the plugin's own clock is
  never used. The engine also notes when the signal arrived. An action's edge is
  `match_end` (the arrival) or `cause` (the decoded event's time). The firing's
  event carries both, so the log shows how long the decision took outside the
  engine. A `cause_ns` in the future, or more than a minute old, is refused.
* **Lateness.** A signal has crossed a process boundary and waited for the
  plugin, so an action timed from `cause` may be due already when the signal
  arrives. It is then done at once and flagged `late`, like any late action. A
  signal is therefore for reactions measured in tens of milliseconds. It is the
  slow path, and the page's rule for choosing it stands.
* **Bytes.** A signal may carry up to 256 bytes of decoded context (an address,
  a sequence number). A reply uses them as it uses the bytes of a match.
* **Limits.** A set declares its signals (`"signals": [...]`, at most 64). A
  signal that is not declared, or raised by a connection that does not own the
  set, is refused. Signals count against the set's budget of firings, so a
  plugin that raises them in a loop suspends its own set and nothing else.
* **Order.** A signal is handled on the place's strand, between two chunks,
  like a line change. The first rule listed that matches it fires, and it uses
  up no bytes.

What stays in the engine and is never left to a signal:

| Need | In the engine as |
|---|---|
| A frame that ends in silence | `gap_us` on the pattern |
| A reply that did not come in time | `quiet_us` with a state |
| Two stations sending at once, a reply inside the turn-around, a transmitter held or off | `bus_fault`, decided from the line's own verdict at the instant of the verdict, with no event queue between |
| A fault or a reply at an exact time after a frame | Any rule on `bytes` |

So the split with a decoding plugin is: what can be told from bytes, silence
and the line is a rule and is exact; what needs the meaning of a message is the
plugin's, arrives as a signal, and is honest about how late it was.

`bus_fault` rules are for a `station` on a half-duplex bus and for an `inline`
set on one of its links. The verdict already exists (`bus_line::transmit`), and
the published `event_kind::bus_fault` is unchanged: the rule is run from the
same verdict, and the rule's own event names the fault and the link, so a
recording shows the fault and what was done about it side by side.

Not built yet: `signal`, `bus_fault` and `quiet_us` are not in the first slice's
model. `quiet_us` and `signal` are small additions to the machine (it already
keeps a deadline for silence and already handles events that use up no bytes).
`bus_fault` needs the hub and comes with the engine path. The capability names
are `rules_signal` and `rules_bus_fault`.

### What a rule does

A rule has one `do` list, or a `sequence` of them. Successive matches take the
steps of a sequence in order, and `then` says what follows the last: `repeat`,
`hold` (the last step from then on) or `stop` (the rule matches no more, and its
bytes fall through to the rules after it). `count` limits the matches of the
whole rule; zero means no limit. `chance_ppm` on a step makes it apply to that
share of the matches, drawn from the seeded source, and otherwise the step is
skipped and counted.

| Action | Roles | Meaning |
|---|---|---|
| `reply` | `peer`, `station` | Send bytes. The parts are hex text, `{"slot": name}`, `{"match": [first, last]}` (bytes of the match) and a check value over `"reply"` or a range of it: `crc16_modbus`, `crc16_ccitt`, `sum8`, `xor8`, `lrc`. The list is fixed |
| `fault` on a `reply` | `peer`, `station` | `drop` (send nothing, count it), `corrupt` (xor bytes at a place), `truncate` (to a length), `duplicate` (send it twice) |
| `pass`, `drop`, `corrupt`, `truncate`, `duplicate`, `hold` | `inline` | The same, on the matched bytes as they pass. `hold` keeps them for the action's delay. Bytes no rule matches pass untouched and at once |
| `lines` | `peer`, `station` | Set output lines of the place (DCD, DSR, CTS, RI as the program sees them on a `peer`) |
| `line_error` | `peer`, `station` | `parity`, `framing`, `overrun`, `break`, `hang_up`, `disconnect`: `studio::inject_fault`, on the ports that take it |
| `set_state` | all | Move the set to a named state. It happens at the match and takes no delay, so the next byte is already matched in the new state |
| `set_slot` | all | Copy bytes of the match into a slot, at the match |
| `mark` | all | Publish an event with a text, and do nothing else |
| `ask` | all | Hand the decision to the plugin, with a time limit (see [Rule or plugin](#rule-or-plugin)) |

### When an action happens

Every action has a time, made of an edge and a delay.

| `after` | The edge |
|---|---|
| `match_end` (the default) | The instant the last byte of the match arrived. On a half-duplex bus, the instant it left the wire, which is the instant the line's turn-around is counted from |
| `match_start` | The instant the first byte of the match arrived (on a half-duplex bus, started on the wire) |
| `previous` | The time of the action before it in the same `do` list, for a reply in parts with gaps between them |
| `run_start` | The start of the run. With `fixed_us` this is an absolute test time. The `timeline` list is shorthand for actions on this edge with no match |
| `cause` | For a rule on a `signal`: the time of the event the plugin decoded |

"Arrived" means the time the endpoint read the chunk that holds the byte, read
once on the endpoint's strand before the chunk is routed. A virtual port hands
over a whole write at once, so every byte of one chunk has one time: the engine
does not invent a time for each bit, except on a half-duplex bus, which has a
line rate and already does.

| `delay` | Meaning |
|---|---|
| `fixed_us` | That long |
| `uniform` | `min_us` to `max_us`, every microsecond as likely |
| `normal` | `mean_us`, `sd_us`, cut off at `min_us` and `max_us` (both required) |
| `exponential` | `mean_us`, cut off at `max_us` (required) and at `min_us` (zero unless given) |
| `table` | A list of values in microseconds, taken in order or picked by the seeded source: a recorded instrument's own timings |

A reply's bytes are built at the match, from the slots as they are then, and
sent at the due time. So the log can say at the match exactly what will be
sent, and a slot changed between the match and the due time does not change a
reply already promised.

Delays are whole microseconds, zero to 60 seconds (`max_link_delay_ms`, the same
limit as a link); a `run_start` time may be up to 24 hours. Every distribution
has hard bounds, so the latest possible time of every action is known when the
set is compiled.

An action may say `"precision": "tight"`. A tight action is waited for on the
hub's timing thread, which sleeps until shortly before the due time and then
watches the clock without sleeping (see [Timers](#timers)). It costs a
processor core for the last part of each wait, so a set may have at most 16
tight actions waiting, the watched part of a wait is at most 2 ms, and a hub has
one timing thread. Everything else is `"normal"`.

By default (`"order": "fifo"`) the actions of one place never overtake each
other: a reply is never due before the reply to an earlier match, as bytes on a
delay line are not. `"order": "independent"` lets them, for a device that
answers a later, shorter request first.

## One clock

There is one clock: the hub's (`hub::now`, the steady clock unless
`hub_options::clock` replaces it). Three changes make it the only one, and they
are the first engine changes of slice 2:

1. **The read time.** `endpoint::on_read` reads the clock once, before routing.
   That time is the chunk's time for the matcher, for the half-duplex line and
   for the data event's `when`. Today the event's `when` is read after routing.
2. **One reader.** The delay line and the rule scheduler read `hub::now`, not
   the steady clock directly, so a test that moves the time by hand moves every
   timer (they look at a hand-moved clock every millisecond while they hold
   something, as `bus_wire` does).
3. **The run.** `start_run` fixes the run's origin and its seed. Every event
   carries its time as nanoseconds since that origin, and the run's header
   records the wall-clock time of the origin once. A harness never compares
   times from two clocks: its own clock does not appear in the results.

A rule event carries three times: the edge, the time the action was due, and the
time the engine did it (handed the bytes to the endpoint's write queue). The
difference between the last two is the engine's own lateness, reported, not
hidden.

## Determinism

**The seeded source.** The set's `seed` (or the run's, when the set has none)
feeds a generator the project owns: SplitMix64 to derive a stream for each rule
from the seed, the set's name and the rule's name, and PCG32 for the draws. The
distributions are the project's own whole-number code (a rejection loop for
uniform, the sum of twelve uniform draws for normal, a fixed-point logarithm for
exponential), never the standard library's, whose results differ between
platforms. The tests pin the first draws of each as literal values, so a
platform that drew differently would fail them. So a rule's n-th draw is the same number on
every platform and compiler, and adding a rule does not shift the draws of the
others. A set with no seed is given one, and the event that reports the
installation says which, so any run can be repeated.

**What a seed repeats.** Given the same rule set, seed and bytes arriving at a
place in the same order with the same chunk times, the engine makes the same
decisions: which rule matched at which byte offset, which step of a sequence,
which draw, which due time relative to the edge. These are the *decisions*, and
they are what the log records.

**What it does not.** When the machine actually ran a timer, and in which order
bytes arrived at two different places, depend on the machine. A rule that
depends on time (`gap_us`) decides from real arrival times, so it can decide
differently on a second run if the program under test was slower. A link
direction that has a plugin modifier is not exactly repeatable either: the
modifier's round trip follows the rule's delay, so the rule's delay is no longer
the last thing before the wire. A run marks that direction "not exactly
repeatable" and its report names the modifier. The rules' own decisions on that
direction still repeat.

**Replay.** Two modes, and a run says which it was:

* *Live, seeded.* Real time, repeatable decisions. A run can be compared with
  another by its decision log (rule, offset, draw); the times will differ by the
  jitter in [What the engine guarantees](#what-the-engine-guarantees).
* *Replayed.* A capture of the first run supplies the bytes and their times, the
  hub runs on a hand-moved clock, and the result is the same event for event and
  nanosecond for nanosecond. This needs every party to be a fixture or a
  recording; a real program on a real port cannot be replayed, only run again.

Link jitter must come from the same source for either mode to hold, so the delay
line takes its seed from the run (decision 4 below).

### Replaying a recording

A recording made in one Studio (bytes, line changes and their times on that
run's clock) is played into another, by the Diagnostics and replay plugin or as
test data. The rule and clock model has to serve that, and does so with four
rules:

* **A recording is a timeline.** Each recorded item (a chunk at a place, a line
  change) becomes an action on the `run_start` edge, at its recorded time since
  the recording's origin. The player is therefore the same scheduler the rules
  use, with the same "never early" and the same reported lateness, and
  `vss::replay` moves onto it in slice 4.
* **Scaled timing.** `time_scale` multiplies the recording's times: 1 is the
  original timing, 0.5 is twice as fast, 10 is a tenth of the speed. The
  arithmetic is whole nanoseconds, rounded down, so a scale gives the same
  schedule on every machine. By default the scale applies to the recording only:
  a rule's delay is the simulated device's behaviour and a link's delay is the
  wire's, and neither changes because the stimulus was sped up.
  `"scale_rules": true` scales every rule and link delay of the run as well, for
  running a whole scenario faster.
* **Deterministic.** A recording replayed live at any scale, with a seed, repeats
  its decisions, as any seeded run does. Replayed on a hand-moved clock, with
  fixtures at every place, it repeats event for event and nanosecond for
  nanosecond, and the scale then changes the times in the result and not how
  long the run takes.
* **Comparable.** Every event of a run carries its time on the run's one clock,
  as nanoseconds since the run's origin: data, lines, faults and every rule
  event alike. An event caused by a recorded item also carries that item's
  recorded time (`source_ns`) and its index in the recording. So a replay is
  compared with its recording item by item, without matching by content: the
  difference between `when` and `source_ns × time_scale` is how far the replay
  was from the recording at that item, and a reply that the device under test
  gave in the recording stands beside the one it gave in the replay with both
  response times on comparable clocks.

A recording keeps the wall-clock time of its origin, its seed and the rule sets
that were installed (their text and generations), so the recording alone is
enough to run it again.

## Compiling and installing

**Compile.** `compile_rule_set(text)` is a pure function: no hub, no clock, no
threads. It returns either an immutable compiled set or a list of problems, each
with the path of the offending value (`rules[2].do[0].delay.max_us`), as the
responder's parser does. Unknown keys are errors. The compiled set holds the
match tables, the reply templates, the bounds (longest pattern, latest action,
most bytes one match can send) and its size in bytes. Everything that can be
refused is refused here: sizes over the limits, a delay with no upper bound, a
name that is not declared, an action the role does not have.

**Install.** `install_rules(compiled, owner)` checks what depends on the running
hub (the place exists, has no set already, the port takes the faults the set
uses, the [loop check](#limits-and-safety)), allocates everything the set will
ever need (its scheduler, its queue up to the limit), and then publishes the set
to the place by posting to the endpoint's strand. So the set takes effect
*between two chunks*: a chunk is matched by no set or by the whole set, never by
half of one. Nothing is allocated and no lock of the hub is taken on the path of
the bytes; a place with no set costs one test of a null pointer, as a link with
no half-duplex line does.

**Several places at once.** `install_rules` takes a list. Either every set is
valid and installed, or none is. The sets do not start at one instant, because
each place switches at its own chunk boundary on its own strand; a set can be
installed disabled and the group enabled with `"at_us"` on the run's clock when
a common start matters. Every installation has a generation number, and every
event names the generation that produced it.

## Changing rules during a run

No change to the rules drops, repeats or reorders a byte of traffic. For `peer`
and `station` that follows from the roles: they watch bytes and send their own,
and hold nothing that belongs to the traffic. The cases are:

| Change | How | What happens to what is in hand |
|---|---|---|
| A slot's bytes (`set_slot`) | One atomic exchange of a pointer; no compile | A reply built before the exchange has the old bytes, one built after has the new. Never a mixture |
| A rule on or off, a delay's numbers, a count reset | `update_rules` with a patch; values are checked against the same limits and swapped at a chunk boundary | Actions already scheduled keep their times |
| A whole new set (`replace_rules`) | Compiled first, swapped at a chunk boundary | **Bytes not yet matched** (at most the longest pattern less one) are given to the new set, so a frame that straddles the change is still matched, once, by the new rules. **Scheduled actions** of the old set are kept (the default: a reply already promised is sent) or cancelled, as the call says. **State, counts and sequence positions** carry over to rules of the same name when asked, and start fresh otherwise |
| Removing a set | The same swap, to nothing | As above. An `inline` set that is removed, or replaced, releases every byte it holds at once and in order, ahead of anything routed after the change; the link's `retiming` gate keeps a later chunk from passing them, as it does for a delay line that is replaced |

A change is reported by an event with the old and the new generation and the
time it took effect, so the log shows which rules any reply came from.

## What the engine guarantees

| Guarantee | Statement |
|---|---|
| Never early | An action never happens before its due time. This is exact, on every platform, as it is for a delay line |
| Lateness is reported | Each action's event carries how late it was. An action later than the set's `late_after_us` is also counted (`late`) and flagged in its event. The engine never claims a time it did not keep |
| Lateness on this machine | Measured, not promised. Today: 10 to 15 ms on Windows. With the timer period raised (the plan for `normal`): about 0.6 to 0.9 ms at the median and under 2 ms at the worst seen. Watching the clock (the plan for `tight`): 0 at the median, 29 µs at the 99th percentile and 182 µs at the worst seen. See [Measured evidence](#measured-evidence) |
| Order at one place | Actions happen in due-time order; ties go to the earlier match, then the earlier action in the list. With `fifo`, in the order of their matches. A reply is one write and is never mixed with another reply's bytes |
| Order of traffic | Bytes a rule does not touch are neither delayed nor reordered by a set's presence |
| Byte accounting | Every byte is in exactly one counter. For each place: bytes seen, matched, passed over; replies and bytes sent; and for `inline`: passed, held, dropped, cut off, duplicated, corrupted. `seen = passed + dropped + cut off` and `delivered = passed + duplicated` hold at any quiet moment, and a test can assert them |
| Bounded cost | The memory of a set and the work for each byte have limits fixed at compile time (see [Limits and safety](#limits-and-safety)) |
| Decisions repeat | As [Determinism](#determinism) says |

What the engine cannot guarantee, and says so:

* **An upper bound on lateness.** Windows, Linux and macOS are not real-time
  systems. The numbers above are what was measured, not a promise; the per-action
  lateness in the log is the evidence for any one run.
* **When the program under test sees the bytes.** The guarantee ends when the
  bytes enter the endpoint's write queue. After that come the transport, the
  driver or the pseudo terminal, and the program's own read. A real serial port
  adds the UART and, for a USB adapter, its latency timer (often 16 ms).
* **Time inside a chunk.** A virtual port delivers a write whole. The engine
  knows when the chunk arrived, not when each byte would have been on a wire.
  Only a half-duplex bus, which has a line rate, has byte times.
* **Lines in step with bytes.** As for RTS on a half-duplex bus: where a
  transport does not report a line change in step with the data, a `lines` match
  is ordered against the bytes only as well as the transport reports it. The
  table in [Architecture](architecture.md#the-half-duplex-line) applies.
* **A late event stream.** Events can be dropped under load by the ring and by a
  slow client, and say how many. The counters are exact; a recording that must
  not lose an event is a capture inside the agent.
* **The same order at two places.** Two ports are two strands.

A feature a port cannot do is refused at installation with `unsupported` and the
name of the feature (a `line_error` on a TCP port, a `lines` match on a pseudo
terminal). It is never approximated silently.

## How it composes with what exists

| Existing piece | With rules |
|---|---|
| Link `delay_ms`, `jitter_ms` | Unchanged, and still "the slow wire". A `station`'s reply crosses its links, so it arrives after the rule's delay plus the link's. A `peer`'s reply is written to the port and crosses no link. In a run the delay line draws from the run's seed |
| Owner policy | A `station`'s reply is a publisher's bytes like any other: on an arbitrated route it is dropped and counted unless the fixture owns the link. `inline` rules see only what the policy let through |
| Half-duplex bus: line rate, collision | A `station`'s reply takes the wire like a member's: it occupies the line for its bytes' time, collides if another member is sending, and is garbled or reported as the bus says |
| Half-duplex bus: turn-around | The turn-around stays in the line model and stays exact, because it is a comparison of two times on one clock and not a timer. A rule's `match_end` is the same instant the line counts from, so "reply 1 ms after the frame on a line with a 2 ms turn-around" gives `too_soon` on every run, and "reply after 2500 µs" never does, however late the timer ran (an action is never early). A rule does not replace or set the turn-around |
| Fault tiles (`inject_fault`), manual sends, fixture echo | Still there, and usable during a run. They and the rules reach the port through the same calls, and each is an event, so the log shows a manual fault between two rule events in the order they happened. An echoing fixture with a `station` set is refused: the echo and the rules would both answer |
| `vss::responder` | Becomes the scripted-reply fixture of slice 3, on the rule engine. `vss_cli` and its script option move to rule sets |
| Capture and replay | A capture records rule events with the traffic. Replay supplies the bytes for a replayed run |
| The agent protocol | New operations (`compile_rules`, `install_rules`, `update_rules`, `set_slot`, `remove_rules`, `rule_stats`, `start_run`) and the new events. The plugin framework decides how a plugin reaches them; this page only needs a connection that the engine can see end |

## Observability

Every fired rule is an event, published on the dispatcher strand like every
other, after the path of the bytes has moved on.

| Event | Carries |
|---|---|
| `rule_fired` | Place, set, generation, rule, step of the sequence, the stream offset and length of the match (or the signal's name, arrival and cause times, or the bus fault and its link), the edge time, and for each action: kind, draw, due time, done time, bytes, and `late`, `dropped` or `suspended` if so |
| `rule_set` | Installed, changed, removed, suspended, orphaned: generations, seed, the reason |
| `run` | Started and ended: origin, seed, wall-clock time of the origin |

`rule_stats(set)` returns the counters of [the guarantees](#what-the-engine-guarantees)
at any time, exact whatever happened to the events. The GUI shows a set on its
port as it shows a fixture's script today, read-only, with its counters; editing
rules in the window is not part of this design.

## Limits and safety

| Limit | Value | When it is reached |
|---|---|---|
| Rules in a set | 256 | Refused at compile |
| Pattern length | 256 bytes | Refused at compile |
| Reply length | 4096 bytes | Refused at compile |
| States, slots | 64 each; a slot holds 4096 bytes | Refused at compile, or by `set_slot` |
| Compiled size of a set | 1 MiB | Refused at compile |
| All sets of a hub | 16 MiB, 256 sets | Refused at install |
| Actions waiting at one place | 1024 | The new action is not scheduled; it is counted (`overrun`) and its event says so |
| Bytes an `inline` set holds | 1 MiB | The oldest are released early, counted and reported |
| Firings of one set | 20 000 a second, as a budget that refills | The set is suspended (below) |
| Rule set document | 1 MiB of JSON, depth 16 | Refused at compile |

**Loops.** Two things can make rules feed themselves: a set whose reply matches
one of its own patterns on a port that echoes, and two `station` sets on one bus
that answer each other. The hub already refuses a cycle of routes; rules add a
second graph on top of it.

* *At install.* The engine builds the graph "a reply of set A can reach the
  place of set B (by `hub`'s own reachability) and can match a pattern of B".
  Whether a reply can match is decided from the templates: fixed bytes are
  compared, and a part that is not fixed (a slot, a copy of the match, a check
  value) is taken to match anything. A cycle in that graph is refused with
  `cycle_detected` unless every way round it passes a rule with a `count`, or the
  caller passes `allow_loops` (two devices that really do talk for ever, such as
  a token ring, are a legitimate test). A set whose own reply can match its own
  pattern on an echoing transport is the cycle of length one.
* *While running.* The check above is conservative about slots, and
  `allow_loops` exists, so there is a second defence that needs no analysis: the
  budget of firings. A set that fires faster than its budget is **suspended**:
  it matches nothing, its scheduled actions are cancelled, an `inline` set
  releases what it holds, a `rule_set` event says why, and the traffic flows as
  if the set were not there. It stays suspended until the harness resumes it. A
  loop therefore costs a bounded burst and never the hub.

Everything a set can ever allocate is allocated at install. On the path of the
bytes a set takes no lock of the hub, makes no allocation in the usual case (a
reply's buffer comes from the set's pool), and does no I/O.

## When the plugin dies

A set has an owner: the connection that installed it. The engine learns that the
owner is gone in two ways: the connection closes (a crash, a kill), or the owner
stops renewing its lease (`lease_ms`, optional; a plugin stopped in a debugger or
deadlocked holds its connection open).

What happens then is declared when the set is installed, because the right
answer differs between a bench and an unattended rig:

| `on_owner_lost` | Meaning | For |
|---|---|---|
| `remove` (the default) | Scheduled actions are cancelled, held bytes are released in order, the set is removed and the traffic flows untouched. The device the set was simulating stops answering | A developer's bench: nothing keeps happening that nobody is watching |
| `keep` | The set runs on to the end of its `count`s or the run. The log goes on, to the capture if one is running | A rig where the harness only starts things |
| `freeze` | The set stays but is suspended, and a new connection may adopt it | A plugin that restarts |

In every case a `rule_set` event (`orphaned`) records the time, and the run is
marked as incomplete, so a report cannot present a run whose harness died as a
clean one. An `ask` that is waiting when the owner goes takes its `on_timeout`
at once. Rules never wait for a plugin, so there is nothing else to unblock.

## Rule or plugin

An `ask` action is the one place a rule waits for another process: the engine
holds the action, sends the plugin the match, and applies the plugin's answer if
it comes within `timeout_us`; otherwise `on_timeout` (one of the other actions,
or nothing) happens, and either way the event says which. An `ask` is sent to
the plugin at once, on its own, and not in the next batch of events; that is a
requirement on the plugin protocol. The compiler refuses an `ask` whose timeout
is under 2 ms (about ten times the 99th percentile of a request and a reply
across the process boundary on this machine), and an `ask` on the edge of a
half-duplex turn-around.

The decision rule, in the order to apply it:

1. **If the answer must come sooner, or more evenly, than a round trip allows,
   it is a rule.** On this machine a request and its reply cross the boundary
   in about 65 µs at the median and 130 µs at the 99th percentile from a C++
   client, with half a millisecond seen at the worst; a plugin in Python, .NET,
   Java or LabVIEW adds its own time and its own pauses, which were not
   measured. Through the event stream as it is today the same reply takes 15 ms.
   The working line is: a response time under 5 ms, or a tolerance tighter than
   1 ms, is never asked of a plugin.
2. **If it must go on when the plugin does not, it is a rule.** Unattended rigs,
   soak tests, anything running while a debugger holds the harness.
3. **If it must repeat exactly, it is a rule.** A plugin's own timing and its own
   random source are outside the seed.
4. **If it is "these bytes, then those bytes, after this long", it is a rule**
   even when there is time to spare, because it is then checked, limited and
   logged for nothing.
5. **Otherwise it is plugin code:** a frame with a length inside it, a reply
   worked out from a model of the instrument, anything that reads a file, a
   database or another device, a protocol with real state. The plugin then uses
   the rules as its hands: slots for what to say next, `ask` where it has the
   time, events to follow what happened. A change to the bytes crossing a link
   that needs the meaning of a message is a plugin modifier, which sits in the
   path of that link after its `inline` rules.

A behaviour is *added to the engine* (a new match kind, action, distribution or
check value) only when all three hold: it is needed inside the round-trip time,
it is bounded (fixed memory, a fixed number of steps for each byte), and it is
general (a check value many protocols use, not one instrument's rule). The rule
language does not grow loops, arithmetic or variables beyond states and slots;
the day a test needs those, it needs a plugin, and later perhaps an in-process
plugin behind a signature, which is a different decision.

### Staying one step ahead

Most protocols a lab simulates are polled: the request for a value arrives long
after the value was known. The plugin does not answer the request; it keeps the
answer ready. It computes the next reply whenever its model changes and puts it
in a slot (`set_slot`, one atomic exchange). The engine answers the poll from the
slot at the declared time. The plugin's speed then decides how fresh the answer
is, never how late it is. This covers checksums the engine does not know,
variable-length replies and instrument models, with engine timing.

## Measured evidence

Measured with `vss_timing_bench` (`tests/bench/test_timing_bench.cpp`), which
uses the project's test framework, is built with the tests and is not run by
CTest, because a time must never decide whether a change passes.

```
cmake --build --preset windows-msvc-release --target vss_timing_bench vss_agent
build/windows-msvc-release/bin/vss_timing_bench --verbose
```

The machine: AMD Ryzen 9 5950X (16 cores), Windows 11 Pro 10.0.26200, MSVC
19.51, Release, the Balanced power plan, and not idle (a virtual machine and
other work held about a quarter of the processor). Times are microseconds
unless they say otherwise. Each row is the range of the medians and of the
99th percentiles over the undisturbed runs (two to four, of 100 to 2000
samples each); one early run was disturbed by something else on the machine and
is described after the tables. Only Windows was measured. Linux and macOS have
no 15.6 ms tick, and slice 2 measures them before it claims anything for them.

How the times were taken: the start is the steady clock read on the endpoint's
strand just before the bytes are injected into a mock device, and the end is
the `when` of the data event for the write at the target, which the endpoint
reads as the write completes. Both are the engine's own clock, with 100 ns
resolution, and no thread wake-up is inside the interval.

### With no timer

| Path | Median | 99th percentile | Worst seen |
|---|---|---|---|
| Routed across a link with no delay | 33 to 108 | 129 to 289 | 1936 |
| The same, until a subscriber's sink has run | 88 to 130 | 149 to 307 | 2013 |
| Responder reply with no delay, from the bytes arriving | 64 to 221 | 201 to 397 | 483 |

A reply the engine sends at once is out about a tenth of a millisecond after its
request. This is the floor for anything the engine does through the event
queue, and the engine path of slice 2, which does not go through the queue,
should be under it.

### With a timer, as the engine is today

Lateness against the time asked for.

| Case | Asked for | Median late by | 99th percentile | Worst seen |
|---|---|---|---|---|
| A bare Asio timer | 1 ms | 14 230 to 14 370 | 15 206 to 15 479 | 20 041 |
| | 5 ms | 10 323 to 10 398 | 11 661 to 11 749 | 12 287 |
| | 20 ms | 10 594 to 10 878 | 12 286 to 12 395 | 18 042 |
| Link delay (`delay_ms`) | 1 ms | 14 251 to 14 405 | 15 395 to 15 616 | 15 974 |
| | 5 ms | 10 194 to 10 375 | 11 468 to 11 706 | 11 797 |
| | 20 ms | 10 529 to 10 815 | 12 132 to 12 291 | 12 822 |
| | 100 ms | 7 017 to 7 916 | 9 245 to 11 141 | 11 241 |
| Responder reply | 1 ms | 14 185 to 14 360 | 15 331 to 15 628 | 16 515 |
| | 5 ms | 10 252 to 10 336 | 11 173 to 11 606 | 11 889 |
| | 20 ms | 10 505 to 10 700 | 11 915 to 12 408 | 12 908 |

Nothing ever arrived early. But a link set to 1 ms delays by about 15 ms, and
one set to 5 ms by about 15 ms too: the timers fire on the 15.6 ms tick of the
Windows clock. A link with 20 ms delay and 10 ms jitter, which should spread
replies evenly over 10 to 30 ms, gave 14.3 to 46 ms with a median of 30.4 ms, on
nine or ten distinct values. The bare timer shows the same figures, so this is
the platform's default and not the delay line or the responder. It is true of
Studio as shipped, and matters to anyone using the delay tile today.

### With the timer period raised to 1 ms

The same cases inside `timeBeginPeriod(1)`.

| Case | Asked for | Median late by | 99th percentile | Worst seen |
|---|---|---|---|---|
| A bare Asio timer | 1, 5, 20 ms | 507 to 521 | 1 359 to 1 534 | 1 807 |
| Link delay | 1 ms | 891 to 943 | 1 631 to 1 731 | 1 970 |
| | 5, 20 ms | 560 to 661 | 1 667 to 1 741 | 1 875 |
| Responder reply | 1 ms | 917 to 944 | 1 538 to 1 771 | 1 881 |
| | 5 ms | 589 to 595 | 1 744 to 1 785 | 1 883 |

Twenty times better for one call, and good enough for most response delays a
lab sets (an instrument that answers in 50 ms, a timeout at 500 ms). It is not
good enough for a turn-around of 1.75 ms.

### Waiting precisely

A dedicated thread waiting for a time, with a Windows high-resolution waitable
timer, and the same with the clock watched for the last 300 µs.

| Wait | Asked for | Median late by | 99th percentile | Worst seen |
|---|---|---|---|---|
| High-resolution timer | 100 µs to 20 ms | 301 to 534 | 686 to 9 508 | 18 247 |
| The same, then watching the clock for 300 µs | 500 µs to 20 ms | 8 to 45 | 367 to 2 821 | 5 737 |
| Watching the clock only | 100 µs | 0 | 3 to 29 | 182 |

The high-resolution timer alone is no better than the raised period: it wakes
0.3 to 0.5 ms late. Watching the clock is exact, but a margin of 300 µs is too
small, because the sleep before it is often later than that. So the tight wait
of this design sleeps until 2 ms before the due time and watches the clock from
there. That combination was not measured; the rows above bound it between "0 at
the median, 29 µs at the 99th percentile" and what a busy machine does to any
thread. Slice 2 measures it and puts the result here.

### Across the process boundary

The real `vss_agent` as a child process, a C++ client, the named pipe.

| Operation | Median | 99th percentile | Worst seen |
|---|---|---|---|
| A request and its reply (`list`) | 61 to 79 | 126 to 146 | 507 |
| A write of 8 bytes and its reply | 61 to 69 | 96 to 124 | 494 |
| A plugin's reply, today: a tap sees the bytes in an event batch (10 ms interval, the least allowed), then a write | 15 312 to 15 444 | 16 485 to 16 755 | 24 844 |

The boundary costs little. What makes an outside reply slow today is the batch
timer, which is itself on the 15.6 ms tick. An `ask` therefore needs a message
sent at once, and then costs two crossings, 0.15 to 0.3 ms for a client like
this one. That makes `ask` worth having. It does not move the decision: the tail
is unbounded, the plugin's own language and pauses are added to it, and nothing
on that path is seeded.

### The disturbed run

In the first of the runs something else on the machine disturbed two minutes of
it: a 5 ms timer was up to 102 ms late, a 5 ms link delay up to 495 ms, and the
jitter case spread over 78 values. The engine reported nothing, because today it
has no way to. This is the case for the `late` flag and the per-action lateness
in every rule event: on a shared machine the only honest timing claim is the one
made for each action after the fact.

### Timers

What the design takes from the numbers:

1. While a hub is running it asks Windows for a 1 ms timer period, and gives it
   back when it stops. This is a platform file chosen by CMake
   (`src/platform/windows`), and nothing on the other platforms. It brings link
   delay, jitter and every `normal` action to within about 1 ms, 2 ms at worst.
   On current Windows the request affects only the process that makes it.
2. `tight` actions use the timing thread: a sleep to 2 ms before the due time,
   then the clock. One thread for each hub, started with the first tight action
   and stopped when none is left.
3. Every timed action reports its own lateness, and the flags and counters make
   a disturbed run visible.
4. Exact turn-around stays a comparison of times in the line model, which no
   timer can make late.

## Where the code goes

| Piece | Where | Notes |
|---|---|---|
| The model, the compiler, the matcher, the seeded source | A new library, `vss_rules`, depending on `vss_core` only for byte and time types | Pure and clockless, like `bus_line`: every call is given the time, so the tests write the times. Fuzzed like the other parsers |
| The seam | `vss_core`: one optional stage on an endpoint's read path and one on a route, null unless a set is installed; the read time; `hub::now` in the delay line; a seed for the delay line | The only changes to `vss_core`. Behaviour with no set installed is unchanged, and the route benchmark's floor holds that to account |
| The scheduler | `vss_rules`, on a strand of its own for each place, shaped like `delay_line` | Weak handlers; `close()` waits for a write under way |
| Install, owners, runs, faults, the fixture | `vss_studio` | `inject_fault` and the capability checks are already here |
| The operations and events | `vss_agent_proto`, the agent | With the plugin framework's session model |
| The capability | In the agent's `hello` and the window's `capabilities`, by the names the plugin framework's design (`docs/plugins.md`, section 6) uses: `engine_rules` (`peer` and `station`), `rules_link` (`inline`), `rules_microseconds` (the timers of this page; without it delays are honoured to the platform's tick and the set says so), and `rules_ask` | Absent: the fixture's script page is disabled with "Not available in this build", as `08-engine-gaps.md` requires. A harness that asks for a feature the build lacks gets `unsupported` with its name |

## The slices

After the owner has reviewed this page, in this order, each a pull request with
its tests in the project's framework:

1. **Model and compiler.** `vss_rules`: the document, the checks, the compiled
   set, the seeded source and distributions with fixed expected values, the
   matcher driven by written bytes and times. No hub. A fuzz target for the
   parser.
2. **The engine path, behind the capability.** The read time, the seam, the
   scheduler, `peer` and `station`, install, replace and remove, events and
   counters, limits and suspension, the loop check, owner loss. Stress tests in
   the shape of `test_hub_options_stress.cpp`; the TSan leg; a floor for routing
   with no set installed and one for the matcher.
3. **The scripted-reply fixture.** The responder moved onto the engine, the
   command line option, a fixture's rule set shown in the window read-only.
4. **Then:** `inline`, `ask`, the agent operations and the run log, replayed
   runs, in the order the plugin framework needs them.

## Decisions for the owner

Decided on 13 November 2026: 2 (the additions to `vss_core`), 3 (the rule
document replaces the responder script), 5 (rules are removed when their owner
is lost) and the first half of 7 (the 1 ms Windows timer period while a hub
runs). Decision 1 follows from approving the design.

Decided later the same day, each as recommended: 4 (a run seeds link jitter),
6 (fixed-length patterns only in the first version), the second half of 7 (the
timing thread for `tight` actions, at most 16 in a rule set; slice 2 measures
that combination) and 8 (rule sets stay out of the topology file). The section
"Triggers from a plugin" is approved too: `quiet_us` and `signal` go into the
matcher and `bus_fault` into the engine path, all in slice 2. Nothing on this
list is open.

1. **Rules are data the engine runs, with no round trip to a plugin.** `ask` is
   the only wait a rule makes, bounded and refused under the floor. A plugin
   modifier can sit in the path of a link's bytes, after the link's `inline`
   rules; it is never what a fast case depends on. (Recommended: yes.)
2. **Three small additions to `vss_core`:** the stage seam, the read time taken
   before routing (which moves a data event's `when` earlier by the time routing
   took), and `hub::now` in the delay line. The alternative keeps `vss_core`
   untouched and builds on events as the responder does; the cost is that
   `inline` is impossible, the edge of a reply is the time an event was
   published and not the time the bytes were read, and every reply waits behind
   the event queue (about 0.1 ms on an idle hub, and as long as the queue is
   when a capture or a slow sink is busy). (Recommended: make the additions.)
3. **The rule document replaces the responder script**, with no reader for the
   old format. (Recommended: yes; nothing is deployed.)
4. **A run seeds link jitter too**, so the delay line takes a seed from the hub
   when a run is started, and stays as it is otherwise. (Recommended: yes.)
5. **The default when the owner is lost is `remove`.** (Recommended; `keep` is
   one word for a rig.)
6. **Fixed-length patterns only in the first version**, with slots for the rest.
   Bounded variable-length patterns (a length byte, a terminator) are the
   likeliest later addition. (Recommended: yes.)
7. **Timers.** Raise the Windows timer period to 1 ms while the hub runs, which
   fixes link delay and jitter for everyone, and add the timing thread for
   `tight` actions. The first is a change in the behaviour of `vss_core` that
   users will see as delays becoming correct; the second costs a core while a
   tight action is close. (Recommended: both; the first as its own small pull
   request before slice 2.)
8. **The topology file does not hold rule sets.** A rule set is a file of its
   own, named by the harness. Saving a fixture's rules with a topology would
   change the JSON schema and is left for a separate request.
