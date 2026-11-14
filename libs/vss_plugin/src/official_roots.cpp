//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The root public keys this program is built with.
//
// The owner made the root keys offline, with the procedure of
// docs/plugins.md, section 8.9, and gave the public halves below.  The
// private halves are with the owner and are never on a computer that runs
// Studio or in this repository.  Only public keys ever go in this file.
//
// A root key signs official key lists and nothing else.  There are two: the
// one in use, and the next one, so that a release can move to a new root
// without a time when Studio trusts none.  To change a root, replace its
// line here; the tests hold the same values and fail until they agree.
//

#include <vss/plugin/signature.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

namespace vss::plugin
{

namespace
{

/// Calling this in a constant expression does not compile, which is how a
/// wrong key is refused at build time.  It is not constexpr on purpose.
[[noreturn]] void a_public_key_is_wrong()
{
    std::terminate();
}

/// The 32 bytes written as 64 lower-case hex characters.  Anything else
/// does not compile.
consteval key_bytes key_from_hex(std::string_view text)
{
    if (text.size() != 64)
        a_public_key_is_wrong();
    const auto value = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f')
            return static_cast<std::uint8_t>(c - 'a' + 10);
        a_public_key_is_wrong();
    };
    key_bytes out{};
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<std::uint8_t>(value(text[i * 2]) * 16 +
                                           value(text[i * 2 + 1]));
    }
    return out;
}

constexpr std::array<root_key, 2> roots{
    root_key{"root-2026",
             key_from_hex("3a94dda0542ec5631e0ce810151baa83"
                          "1dd7f128ff9c393a68fef153026b0ca1")},
    root_key{"root-next",
             key_from_hex("648b4ce8ef09b6ce5d959d741e81e106"
                          "3e73af05d3d156c5a0072b78c7503e1c")}};

} // namespace

std::span<const root_key> builtin_root_keys() noexcept
{
    return roots;
}

} // namespace vss::plugin
