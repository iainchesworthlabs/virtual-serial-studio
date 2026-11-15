//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <string_view>
#include <type_traits>

namespace vss::update
{

/// Why an update feed was refused (docs/updates.md, section 4). A value never
/// changes its number once a release has carried it: new ones are added at
/// the end.
enum class update_errc
{
    success = 0,
    /// The list of trusted keys given was empty. A build without update keys
    /// cannot check for updates.
    no_update_keys,
    /// The text is not a signed file, or it has another purpose, or it names
    /// a key that is not trusted for `update-feed`, or the signature does
    /// not verify, or the key's window does not cover `issued_at`.
    signature_invalid,
    /// The payload is not a feed this version reads: bad JSON, an unknown or
    /// repeated key, a wrong type or a value outside its rule.
    feed_malformed,
    /// `feed_version` is a whole number other than 1.
    feed_version_unsupported,
    /// `serial` is lower than the highest one accepted before.
    feed_rolled_back
};

/// The category of `update_errc`, named `vss.update`.
const boost::system::error_category& update_category() noexcept;
boost::system::error_code make_error_code(update_errc value) noexcept;

/// The code as it is written in logs and in "Copy details": the enumerator's
/// own name, such as `feed_malformed`.
std::string_view to_string(update_errc value) noexcept;

template <class T> using result = boost::system::result<T>;

} // namespace vss::update

namespace boost::system
{
template <> struct is_error_code_enum<vss::update::update_errc> : std::true_type
{
};
} // namespace boost::system
