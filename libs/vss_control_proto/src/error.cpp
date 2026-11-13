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
    return make_error_code(parse_code(error.code).value_or(api_errc::internal));
}

} // namespace vss::control
