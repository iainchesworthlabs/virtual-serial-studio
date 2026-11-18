//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "json_support.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>

namespace vss::debug
{

std::string time_text(std::int64_t wall_ms)
{
    using namespace std::chrono;
    const sys_time<milliseconds> point{milliseconds(wall_ms)};
    const auto whole = floor<seconds>(point);
    const auto millis = duration_cast<milliseconds>(point - whole).count();
    const year_month_day day{floor<days>(whole)};
    const hh_mm_ss<seconds> clock{whole - floor<days>(whole)};

    std::array<char, 40> text{};
    const int length = std::snprintf(
        text.data(), text.size(), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ",
        static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
        static_cast<unsigned>(day.day()),
        static_cast<int>(clock.hours().count()),
        static_cast<int>(clock.minutes().count()),
        static_cast<int>(clock.seconds().count()), static_cast<int>(millis));
    return std::string(text.data(), length > 0
                                        ? static_cast<std::size_t>(length)
                                        : std::size_t{0});
}

boost::json::object entry_object(const trace::entry& value)
{
    boost::json::object out;
    out["seq"] = value.seq;
    out["mono_ns"] = value.mono_ns;
    out["wall"] = time_text(value.wall_ms);
    out["thread"] = value.thread;
    out["endpoint"] = value.endpoint;
    out["call"] = std::string(value.call_text());
    if (!value.args_text().empty())
        out["args"] = std::string(value.args_text());
    if (!value.operation_text().empty())
        out["operation"] = std::string(value.operation_text());
    out["result"] = value.failed ? "failed" : "ok";
    out["duration_us"] = value.duration_us;
    if (value.failed)
    {
        out["code"] = value.code;
        out["category"] = std::string(value.category_text());
        out["message"] = std::string(value.message_text());
        if (value.cause != 0)
            out["cause"] = value.cause;
    }
    return out;
}

void redact_value(boost::json::value& value, const redactor& redact)
{
    if (auto* text = value.if_string())
    {
        *text = redact.apply(std::string_view(*text));
    }
    else if (auto* list = value.if_array())
    {
        for (auto& item : *list)
            redact_value(item, redact);
    }
    else if (auto* object = value.if_object())
    {
        boost::json::object rebuilt;
        for (auto& member : *object)
        {
            boost::json::value item = std::move(member.value());
            redact_value(item, redact);
            rebuilt[redact.apply(std::string_view(member.key()))] =
                std::move(item);
        }
        *object = std::move(rebuilt);
    }
}

} // namespace vss::debug
