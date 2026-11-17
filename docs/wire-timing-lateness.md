# Wire timing: measured lateness

What a paced link ([wire timing](architecture.md#wire-timing)) keeps of the times its pacer gives it. This is a
measurement on one machine, not a promise. Linux and macOS have not been measured; the case below runs on all three
(`vss_timing_bench --filter="wire timing"`).

## What is measured

The `bench: a long chunk on a link with wire timing` case sends one long write through a mock port, over a link with
wire timing at a given rate, and reads the link's own counters afterwards:

* `paced_releases`: the releases made through the pacer.
* `late_releases`: those that left more than 250 microseconds after the time the wire gave them.
* `max_late_us`: the largest lateness, in microseconds.
* `spin_refused`: releases that wanted to spin for the last part of a gap and found the spin budget (100 ms in every
  second, one tenth of a core) used up. They were made from the timer instead.

The lateness is against the pacer's time for the release, not against when the program wrote, so it is what the timer
and the spin leave, and no more.

## Windows 11, Release build, 2026-11-17

The machine is the owner's workstation; the hub asks for the 1 ms timer period and the high resolution waitable timer is
available. Three runs of each, in one process after the other:

| Case | Releases | Late (> 250 us) | Largest | Spin refused |
|---|---|---|---|---|
| 2000 bytes at 115200 8N1 (runs of 5 bytes, 434 us apart) | 400 | 49, 53, 51 | 1869, 782, 875 us | 78, 75, 76 |
| 2000 bytes at 250000 8N1 (runs of 12 bytes, 480 us apart) | 167 | 0, 2, 1 | 74, 971, 583 us | 0 |
| 100 bytes at 9600 8N1 (one byte, 1.04 ms apart) | 100 | 6, 3, 5 | 775, 710, 805 us | 0 |

What it says:

* **Inside the spin budget the release is on time.** The first 100 ms of spinning is free, so the 250000 baud case
  (89 ms of wire time) is within a few hundred microseconds nearly every release, and late once or twice by a timer
  trip.
* **A stream of sub-millisecond gaps that lasts longer than the budget is late by about the timer's accuracy.** At
  115200 baud the 175 ms chunk uses up the 100 ms allowance, and the late releases (about 50) are almost exactly the
  releases after it ran out (about 76 refused spins; some of those are just early enough to be on time from the timer).
  This is what the budget is for. A long stream of gaps under 0.5 ms needs more than a tenth of a core on any system.
* **A gap of a millisecond or more does not spin.** The 9600 baud case is on the high resolution waitable timer alone and
  is late (more than 250 us) in about 5 releases in 100, by up to 0.8 ms.
* **Without the high resolution timer** the same hub would be as late as asio's timer is. The bench's bare timer, with
  no timer period requested, overshoots by about 15 ms at the median on this machine (`timer_overshoot_1ms` p50 14517 us),
  and with the 1 ms period by about 1 ms. The waitable timer is why a paced route does not depend on that request.

A Debug build is slower per release (each release writes to an endpoint and publishes an event) and was late more often:
115200 baud, 245 late of 400 before the pass that delivers several runs in one go was added, 48 after.

## Not measured

* Linux and macOS lateness, and the machine's CPU use at 1000 paced releases a second.
* A stress run of 64 paced links. The spin allowance is shared by every paced route of a hub, so 64 links spin for no
  more than 100 ms in a second between them.
* A Windows before 10 version 1803, where the system has no high resolution timer. The platform code falls back to
  asio's timer there, with a stated accuracy of 1 ms.
