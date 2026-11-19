//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/trace_text.hpp>

#include "json_support.hpp"

#include <algorithm>
#include <vector>

namespace vss::debug
{
namespace
{

constexpr std::size_t last_calls_when_no_failure = 50;

} // namespace

std::string entry_line(const trace::entry& value)
{
    std::string line = time_text(value.wall_ms) + "  #" +
                       std::to_string(value.seq) + "  " +
                       std::string(value.call_text());
    if (!value.args_text().empty())
        line += " (" + std::string(value.args_text()) + ")";
    if (!value.operation_text().empty())
        line += "  in " + std::string(value.operation_text());
    if (value.endpoint != 0)
        line += "  port " + std::to_string(value.endpoint);
    if (value.failed)
    {
        line += "  FAILED " + std::string(value.category_text()) + ":" +
                std::to_string(value.code) + " " +
                std::string(value.message_text());
        if (value.cause != 0)
            line += "  (from #" + std::to_string(value.cause) + ")";
    }
    line += "  " + std::to_string(value.duration_us) + " us";
    return line;
}

std::string failure_context_text(const trace::recorder& source,
                                 std::chrono::seconds around)
{
    const std::vector<trace::entry> all = source.read();
    const std::int64_t span_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(around).count();

    std::string out =
        "Calls Studio made to Windows and to its libraries, as it recorded "
        "them.\nEach failed call is listed with the calls made in the " +
        std::to_string(around.count()) +
        " seconds around it.\nThe bytes your ports carried are not recorded "
        "and are not here.\n\n";

    std::vector<const trace::entry*> chosen;
    const bool any_failure = std::ranges::any_of(
        all, [](const trace::entry& item) { return item.failed; });
    if (any_failure)
    {
        for (const trace::entry& item : all)
        {
            const bool near_failure = std::ranges::any_of(
                all,
                [&](const trace::entry& other)
                {
                    return other.failed &&
                           item.wall_ms >= other.wall_ms - span_ms &&
                           item.wall_ms <= other.wall_ms + span_ms;
                });
            if (near_failure)
                chosen.push_back(&item);
        }
    }
    else
    {
        out += "No call has failed since Studio started.  The last calls:\n\n";
        const std::size_t first = all.size() > last_calls_when_no_failure
                                      ? all.size() - last_calls_when_no_failure
                                      : std::size_t{0};
        for (std::size_t index = first; index < all.size(); ++index)
            chosen.push_back(&all[index]);
    }
    if (chosen.empty())
    {
        out += "Nothing has been recorded yet.\n";
        return out;
    }
    for (const trace::entry* item : chosen)
        out += entry_line(*item) + "\n";
    return out;
}

} // namespace vss::debug
