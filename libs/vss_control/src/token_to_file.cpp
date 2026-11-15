//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/token_to_file.hpp>

#include <vss/control/proto/secret.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace vss::control
{

result<token_to_file_outcome> create_token_to_file(
    client& administration, const std::string& label,
    const std::vector<std::string>& scopes, std::uint64_t life_s,
    const std::vector<std::string>& transports, private_file_claim& file)
{
    const auto made =
        administration.create_token(label, scopes, life_s, transports);
    if (!made)
        return made.error();

    token_to_file_outcome outcome;
    outcome.entry = made->entry;

    // The line is a copy of the token, so it is wiped when it has been written.
    const std::string_view secret = made->token.expose();
    std::string line;
    line.reserve(secret.size() + 1);
    line.assign(secret);
    line.push_back('\n');
    outcome.write_error = file.write(line);
    secure_wipe(line.data(), line.size());

    if (outcome.write_error)
    {
        // A token that nobody holds must not stay good for 30 days.
        outcome.revoked =
            static_cast<bool>(administration.revoke_token(outcome.entry.id));
    }
    return outcome;
}

} // namespace vss::control
