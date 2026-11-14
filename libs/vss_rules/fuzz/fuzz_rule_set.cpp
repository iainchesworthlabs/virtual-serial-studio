//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for the rule set reader and the machine that runs a set. The
// input is split at its first zero byte: what comes before is the document, a
// rule set in JSON; what comes after is traffic. Any bytes may arrive from a
// file a person opened, so the reader must reject bad input with an error and
// never crash. A set that compiles is run on the traffic, fed in pieces of 1
// to 7 bytes 100 microseconds apart, with line and port changes and signals
// driven by the traffic, and must keep to its promises: nothing is due before
// its match (unless it is timed from the start of the run or, for a signal,
// from its cause), no reply is longer than the limit, a signal carries no more
// than the limit, the bytes kept for a successor are fewer than the longest
// pattern, and every byte fed is counted.
//

#include <vss/rules/compile.hpp>
#include <vss/rules/machine.hpp>
#include <vss/rules/parse.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
namespace rules = vss::rules;

/// Stops the run when a firing breaks a promise of the machine.
void check_firings(const std::vector<rules::firing>& firings)
{
    for (const auto& fired : firings)
    {
        for (const auto& planned : fired.actions)
        {
            const bool from_start =
                planned.source != nullptr &&
                (planned.source->when.after == rules::edge::run_start ||
                 planned.source->when.after == rules::edge::cause);
            if (!from_start && planned.due < fired.start)
            {
                std::abort();
            }
            if (planned.bytes.size() > rules::max_reply_bytes)
            {
                std::abort();
            }
        }
        if (fired.signal_bytes > rules::max_signal_bytes ||
            (fired.signal == rules::no_index && fired.signal_bytes != 0))
        {
            std::abort();
        }
    }
}

void check_tail(const rules::machine& machine)
{
    const std::size_t longest = machine.set().longest_pattern;
    const std::size_t kept = machine.unmatched_tail().size();
    if (kept != 0 && kept >= longest)
    {
        std::abort();
    }
}

void run(const std::shared_ptr<const rules::compiled_rule_set>& set,
         vss::bytes_view traffic)
{
    rules::machine machine(set, 1);
    std::vector<rules::firing> firings;

    const vss::time_point origin = vss::time_point{} + 1s;
    for (auto& each : machine.start(origin))
    {
        firings.push_back(std::move(each));
    }
    check_firings(firings);

    vss::time_point now = origin;
    vss::control_lines lines;
    std::size_t at = 0;
    while (at < traffic.size())
    {
        const std::size_t chunk = std::min<std::size_t>(
            static_cast<std::size_t>(1 + traffic[at] % 7), traffic.size() - at);
        now += 100us;

        firings.clear();
        machine.feed(traffic.subspan(at, chunk), now, firings);
        machine.tick(now, firings);

        // The last byte of the piece decides whether something else happens.
        const vss::byte last = traffic[at + chunk - 1];
        if ((last & 0x80U) != 0)
        {
            const auto changed = vss::control_lines::from_bits(last & 0x7fU);
            machine.lines_changed(lines, changed, now, firings);
            lines = changed;
        }
        else if ((last & 0x40U) != 0)
        {
            machine.port_changed((last & 1U) != 0 ? rules::port_event::opened
                                                  : rules::port_event::closed,
                                 now, firings);
        }
        if ((last & 0x20U) != 0 && !machine.set().signals.empty())
        {
            // The rest of the traffic is what the signal carries, so that a
            // long input is cut at the limit.  Its cause is a little before.
            machine.signal(last % machine.set().signals.size(),
                           traffic.subspan(at), now - 50us, now, firings);
        }

        check_firings(firings);
        check_tail(machine);
        at += chunk;
    }

    // Whatever waited for silence has it now.
    firings.clear();
    if (const auto deadline = machine.next_deadline())
    {
        machine.tick(*deadline, firings);
    }
    machine.tick(now + 1h, firings);
    check_firings(firings);
    check_tail(machine);

    if (machine.stats().bytes_seen != traffic.size())
    {
        std::abort();
    }
}

} // namespace

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    const std::uint8_t* const end = data + size;
    const std::uint8_t* const zero = std::find(data, end, std::uint8_t{0});
    const std::string_view document(reinterpret_cast<const char*>(data),
                                    static_cast<std::size_t>(zero - data));
    vss::bytes_view traffic;
    if (zero != end)
    {
        traffic =
            vss::bytes_view(zero + 1, static_cast<std::size_t>(end - zero - 1));
    }

    const auto set = rules::compile_rule_set(document);
    if (!set)
    {
        return 0;
    }
    run(*set, traffic);
    return 0;
}
