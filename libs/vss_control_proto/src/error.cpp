//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/proto/error.hpp>

#include <array>
#include <utility>

namespace vss::control
{
namespace
{

struct code_row
{
    api_errc value;
    std::string_view name;
    const char* message;
    bool retryable;
};

constexpr std::array rows{
    code_row{api_errc::unauthenticated, "unauthenticated", "not authenticated",
             false},
    code_row{api_errc::forbidden, "forbidden", "not allowed", false},
    code_row{api_errc::invalid_argument, "invalid_argument",
             "the request is not valid", false},
    code_row{api_errc::not_found, "not_found", "not found", false},
    code_row{api_errc::conflict, "conflict", "conflict", false},
    code_row{api_errc::unsupported, "unsupported", "not supported", false},
    code_row{api_errc::busy, "busy", "too many sessions", true},
    code_row{api_errc::timeout, "timeout", "timed out", true},
    code_row{api_errc::limit_exceeded, "limit_exceeded", "limit exceeded",
             true},
    code_row{api_errc::cancelled, "cancelled", "cancelled", false},
    code_row{api_errc::unavailable, "unavailable", "the agent is not running",
             true},
    code_row{api_errc::internal, "internal", "internal error", false}};

const code_row* find_row(api_errc value) noexcept
{
    for (const code_row& row : rows)
    {
        if (row.value == value)
            return &row;
    }
    return nullptr;
}

class api_category_type final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.control"; }

    std::string message(int value) const override
    {
        if (value == 0)
            return "success";
        const code_row* const row = find_row(static_cast<api_errc>(value));
        return row == nullptr ? "unknown control error" : row->message;
    }
};

} // namespace

const boost::system::error_category& api_category() noexcept
{
    static const api_category_type instance;
    return instance;
}

boost::system::error_code make_error_code(api_errc value) noexcept
{
    return {static_cast<int>(value), api_category()};
}

std::string_view code_name(api_errc value) noexcept
{
    const code_row* const row = find_row(value);
    return row == nullptr ? std::string_view("internal") : row->name;
}

std::optional<api_errc> parse_code(std::string_view name) noexcept
{
    for (const code_row& row : rows)
    {
        if (row.name == name)
            return row.value;
    }
    return std::nullopt;
}

bool retryable(api_errc value) noexcept
{
    const code_row* const row = find_row(value);
    return row != nullptr && row->retryable;
}

api_error to_api_error(api_errc value)
{
    const code_row* row = find_row(value);
    if (row == nullptr)
        row = find_row(api_errc::internal);
    api_error out;
    out.code = row->name;
    out.message = row->message;
    out.retryable = row->retryable;
    return out;
}

error_code from_api_error(const api_error& error) noexcept
{
    if (const auto known = parse_code(error.code))
        return make_error_code(*known);
    try
    {
        for (int value = 1; value <= static_cast<int>(errc::internal); ++value)
        {
            const error_code candidate =
                vss::make_error_code(static_cast<errc>(value));
            if (engine_error_name(candidate) == error.code)
                return candidate;
        }
        return make_error_code(api_errc::internal);
    }
    catch (...)
    {
        // Out of memory while comparing names.
        return make_error_code(api_errc::internal);
    }
}

api_error engine_api_error(const error_code& error)
{
    api_error out;
    out.code = engine_error_name(error);
    out.message = "the engine refused the request";
    out.retryable = false;
    return out;
}

std::string engine_error_name(const error_code& error)
{
    if (!error)
        return {};
    std::string_view name = "io_error";
    if (error.category() == vss::error_category())
    {
        switch (static_cast<errc>(error.value()))
        {
        case errc::success:
            return {};
        case errc::not_found:
            name = "not_found";
            break;
        case errc::already_exists:
            name = "already_exists";
            break;
        case errc::invalid_argument:
            name = "invalid_argument";
            break;
        case errc::invalid_uri:
            name = "invalid_uri";
            break;
        case errc::unsupported:
            name = "unsupported";
            break;
        case errc::not_open:
            name = "not_open";
            break;
        case errc::already_open:
            name = "already_open";
            break;
        case errc::cycle_detected:
            name = "cycle_detected";
            break;
        case errc::queue_full:
            name = "queue_full";
            break;
        case errc::timeout:
            name = "timeout";
            break;
        case errc::canceled:
            name = "canceled";
            break;
        case errc::io_error:
            name = "io_error";
            break;
        case errc::permission_denied:
            name = "permission_denied";
            break;
        case errc::config_error:
            name = "config_error";
            break;
        case errc::device_busy:
            name = "device_busy";
            break;
        case errc::port_exhausted:
            name = "port_exhausted";
            break;
        case errc::internal:
            name = "internal";
            break;
        }
    }
    return "engine." + std::string(name);
}

} // namespace vss::control
