//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include <type_traits>

namespace vss::license
{

/// Why a signed file or a licence payload was refused.
enum class errc
{
    success = 0,
    /// Larger than `signed_file::max_file_size`.
    too_large,
    /// Not the envelope or payload this version writes: bad JSON, a duplicate
    /// key, a missing or mistyped field, bad base64url, a bad date.
    malformed,
    /// An envelope format, or a field named in `critical`, that this version
    /// does not know. The file was made for a newer version.
    newer_version,
    /// The envelope's purpose is not the one the caller asked for.
    wrong_purpose,
    /// No trusted key has the envelope's key id for this purpose.
    unknown_key,
    /// The key is revoked, or the file was signed outside the key's window.
    key_not_valid,
    /// The signature does not verify.
    bad_signature
};

const boost::system::error_category& error_category() noexcept;

boost::system::error_code make_error_code(errc value) noexcept;

template <class T> using result = boost::system::result<T>;

} // namespace vss::license

namespace boost::system
{

template <> struct is_error_code_enum<vss::license::errc> : std::true_type
{
};

} // namespace boost::system
