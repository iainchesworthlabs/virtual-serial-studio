//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Semantic Versioning 2.0.0 for the update feed (docs/updates.md, section 4.3):
// parse, write and compare. No build metadata and no leading "v".
//

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::update
{

/// The most characters a version may have.
inline constexpr std::size_t max_version_length = 64;
/// The largest major, minor or patch number.
inline constexpr std::uint32_t max_version_number = 999999;
/// The most identifiers a pre-release may have.
inline constexpr std::size_t max_prerelease_identifiers = 16;

struct version
{
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    /// The dot-separated identifiers after the hyphen; empty for a final
    /// release.
    std::vector<std::string> prerelease;

    bool is_prerelease() const noexcept { return !prerelease.empty(); }

    friend bool operator==(const version&, const version&) = default;
};

/// Reads `major.minor.patch` with an optional `-prerelease`. Refused: build
/// metadata ("+..."), a leading "v", more than 64 characters, a number with a
/// leading zero or over 999999, and a pre-release with no identifier, more
/// than 16 of them, an empty one, a character outside `[0-9A-Za-z-]` or a
/// numeric one with a leading zero.
std::optional<version> parse_version(std::string_view text);

/// The text `parse_version` reads back as an equal value.
std::string to_string(const version& value);

/// Precedence as section 11 of the Semantic Versioning 2.0.0 specification:
/// the numbers, then a pre-release is lower than its final release, then the
/// identifiers from the left (a number is lower than a word, numbers by
/// value, words by ASCII order, and a longer list wins when every earlier
/// identifier is equal).
std::strong_ordering compare(const version& left,
                             const version& right) noexcept;

/// True for the version a build without a tag carries: its pre-release is
/// exactly the one identifier "dev", as in 0.1.0-dev.
bool is_developer_build(const version& value) noexcept;

} // namespace vss::update
