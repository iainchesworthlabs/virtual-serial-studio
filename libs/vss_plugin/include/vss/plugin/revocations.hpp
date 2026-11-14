//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The list of plugins and signing keys that were withdrawn.  docs/plugins.md,
// section 8.5, is the design.
//
// The list travels as a signed file.  What is read here is its payload, the
// bytes the signature covers.  Checking the signature is a separate step,
// and a caller must not use a list whose signature it has not checked.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/manifest.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin
{

/// The format of the list this version reads.
inline constexpr std::uint32_t revocations_format_version = 1;

inline constexpr std::size_t max_revocations_size =
    std::size_t{256} * 1024;
inline constexpr std::size_t max_revocations_depth = 4;
inline constexpr std::size_t max_revoked_entries = 4096;
/// The largest serial: every integer up to it is exact in any JSON reader.
inline constexpr std::uint64_t max_revocations_serial = (1ULL << 53) - 1;

/// Every version of one plugin up to and including `max_version`.
struct revoked_plugin
{
    std::string id;
    plugin_version max_version;

    friend bool operator==(const revoked_plugin&,
                           const revoked_plugin&) = default;
};

struct revocation_list
{
    /// A list replaces another only when its serial is higher.
    std::uint64_t serial = 0;
    std::chrono::year_month_day issued_at{};
    std::vector<std::string> key_ids;
    std::vector<plugin_hash> plugin_hashes;
    std::vector<revoked_plugin> plugins;

    bool revokes_key(std::string_view key_id) const noexcept;
    bool revokes_hash(const plugin_hash& hash) const noexcept;
    bool revokes_plugin(std::string_view id,
                        const plugin_version& version) const noexcept;

    friend bool operator==(const revocation_list&,
                           const revocation_list&) = default;
};

/// Reads the payload of a revocation list.
///
/// `errc::config_error` for anything that is not a list this version reads:
/// more than `max_revocations_size` bytes, text that is not JSON, a key the
/// format does not have or one given twice, a wrong type, a
/// `revocations_version` other than `revocations_format_version`, a serial
/// of zero or above `max_revocations_serial`, or a list over its limit.
/// `detail`, when given, receives one line for a person.
result<revocation_list> parse_revocations(std::string_view payload,
                                          std::string* detail = nullptr);

} // namespace vss::plugin
