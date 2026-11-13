# Fuzzing

Code that reads input from outside has a libFuzzer harness in
`libs/<lib>/fuzz/`, with a seed corpus in `corpus/` and the inputs of past
crashes in `regressions/`.

| Harness | Reads |
|---|---|
| `libs/vss_config/fuzz/fuzz_topology.cpp` | A saved topology (JSON). It must be rejected or accepted without a crash, and the text written for an accepted one must read back and write again unchanged |
| `libs/vss_ports/fuzz/fuzz_endpoint_uri.cpp` | An endpoint URI such as `tcp://host:4001` or `serial://COM3?baud=9600` |
| `libs/vss_agent_proto/fuzz/fuzz_agent_frame.cpp` | The bytes of a connection to the agent: frames, requests, replies and event batches. They must be refused or accepted without a crash, read the same whole or in pieces, and what decodes must encode and decode again to the same value |
| `libs/vss_rules/fuzz/fuzz_rule_set.cpp` | A response timing rule set (JSON), then traffic after the first zero byte. The set must be rejected or accepted without a crash; an accepted one is run on the traffic, fed in pieces of 1 to 7 bytes with line and port changes, and nothing it plans may be due before its match (unless timed from the start of the run), no reply may be over the limit, the kept tail must be shorter than the longest pattern, and every byte fed must be counted |

## Where it runs

| Run | When | What |
|---|---|---|
| Replay in the normal tests | Every build | CTest tests `fuzz_topology`, `fuzz_endpoint_uri`, `fuzz_agent_frame` and `fuzz_rule_set` (label `fuzz`) run each corpus and regression file once, through the `fuzz_topology_replay`, `fuzz_endpoint_uri_replay`, `fuzz_agent_frame_replay` and `fuzz_rule_set_replay` programs. They work with any compiler, and run under AddressSanitizer and UBSan in the `AddressSanitizer and UBSan` job |
| `Fuzz regression replay` (`fuzz-regress` in `ci.yml`) | Every ready pull request and merge-queue entry | Builds the `linux-fuzz` targets and replays the same files under libFuzzer with `tools/fuzz.py --mode regress`, then runs the unit tests of that script |
| `Fuzz` in `.github/workflows/fuzz.yml` | Push to `develop`: 60 seconds a target | Replays the files, then fuzzes each target |
| | Every night: 10 minutes a target | The same; the working corpus is kept between runs in the Actions cache |
| | On request (`workflow_dispatch`) | Either length, chosen in the run form |

A crash fails the run, uploads the input as the `fuzz-crashes` artifact and, at
night, opens or updates a `nightly-analysis` issue.

## Running it yourself

Use Clang with libFuzzer. Configure with `CC=clang CXX=clang++`.

```bash
cmake --preset linux-fuzz
cmake --build --preset linux-fuzz
python3 tools/fuzz.py --mode short
```

`--mode regress` only replays the files, `--mode short` fuzzes for 60 seconds a
target, `--mode nightly` for ten minutes, and `--seconds` sets another time.
`--build-dir` (default `build/linux-fuzz`) and `--work-dir` (default
`.cache/fuzz`) move the two directories. New inputs go to
`.cache/fuzz/<harness>`, which Git ignores; crash files go to
`.cache/fuzz/artifacts`. The script finds a harness by the name
`libs/<lib>/fuzz/fuzz_*.cpp`, and exits with status 1 if any target fails.

The `linux-fuzz` build is RelWithDebInfo with `ICL_ENABLE_FUZZING`, which adds
AddressSanitizer and UBSan at -O1 and stops at the first finding. It cannot be
combined with the sanitizer presets, coverage or Tracy, so it has a build tree
of its own.

## After a crash

Fix the bug, then copy the crashing input into the harness's `regressions/`
directory with the fix, so the replay checks it from then on. Add a harness
for any new function that parses untrusted input: create `fuzz_<thing>.cpp`
beside the others, add a seed corpus and a `regressions/` directory (keep it in
Git with an empty `.gitkeep`), and declare it in `tests/fuzz/CMakeLists.txt`.
