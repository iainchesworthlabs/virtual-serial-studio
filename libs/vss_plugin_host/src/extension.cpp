//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/extension.hpp>

#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace vss::plugin::host
{

bool extension_context::may(permission wanted) const noexcept
{
    return std::ranges::find(granted, wanted) != granted.end();
}

api_message error_answer(api_errc code, const std::string& text)
{
    // The session sets the number of the request this answers.
    return make_error(0, code, text);
}

api_message ok_answer()
{
    api_message message;
    message.op = std::string(op_of(message_body{ok_message{}}));
    message.body = ok_message{};
    return message;
}

} // namespace vss::plugin::host
