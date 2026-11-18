//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vss::debug
{

/// Removes what a debug reply must not carry: the user's home folder, the
/// user and computer names, device serial numbers, and credentials (a URL
/// with a password, `password=`, `token:`, `secret=`, `api_key=` and bearer
/// tokens).  Every string that leaves the debug API goes through it.
///
/// A literal is compared without regard to case in the ASCII range, so a
/// Windows path matches however it was written.  A literal shorter than three
/// characters is ignored: replacing `a` would ruin the text and hide nothing.
class redactor
{
public:
    /// Replaces `needle` with `replacement` wherever it occurs.
    void add_literal(std::string needle, std::string replacement);

    /// The usual set for this process: the home folder, the user name and the
    /// computer name.  Any may be empty.
    static redactor for_user(std::string_view home, std::string_view user,
                             std::string_view computer);

    /// `text` with every literal and every credential shape replaced.
    std::string apply(std::string_view text) const;

private:
    struct literal
    {
        std::string needle;
        std::string replacement;
    };
    std::vector<literal> literals_;
};

} // namespace vss::debug
