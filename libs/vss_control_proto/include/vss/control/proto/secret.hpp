//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace vss::control
{

/// Overwrites `size` bytes at `data` with zeros in a way the compiler may not
/// leave out.
void secure_wipe(char* data, std::size_t size) noexcept;

/// True when the two ranges hold the same bytes.  For ranges of the same
/// length the time it takes does not depend on where they differ: every byte
/// is looked at.  Ranges of different lengths are not equal, and that the
/// lengths differ is not a secret.
bool constant_time_equal(std::string_view first,
                         std::string_view second) noexcept;

/// A token or another credential, held so that it cannot be printed by
/// accident (docs/remote-control.md, section 11).
///
/// It has no stream insertion, no `to_string` and no conversion to a string
/// or a string view, so a secret cannot be passed to the audit log, to a
/// logger or to `printf` without the one call that says so: `expose()`.  A
/// search for `expose(` finds every place a secret leaves this type.  Its
/// memory is wiped when it is destroyed, moved from or assigned to.
class secret_string
{
public:
    secret_string() noexcept = default;

    /// Copies `text`.  The caller wipes its own copy.
    explicit secret_string(std::string_view text) : bytes_(text) {}

    secret_string(const secret_string&) = default;

    secret_string(secret_string&& other) noexcept
        : bytes_(std::move(other.bytes_))
    {
        other.wipe();
    }

    secret_string& operator=(const secret_string& other)
    {
        if (this != &other)
        {
            wipe();
            bytes_ = other.bytes_;
        }
        return *this;
    }

    secret_string& operator=(secret_string&& other) noexcept
    {
        if (this != &other)
        {
            wipe();
            bytes_ = std::move(other.bytes_);
            other.wipe();
        }
        return *this;
    }

    ~secret_string() { wipe(); }

    bool empty() const noexcept { return bytes_.empty(); }
    std::size_t size() const noexcept { return bytes_.size(); }

    /// The secret itself.  The one way out of this type: every use is a
    /// place where a secret is hashed, sent to its owner or shown once.
    std::string_view expose() const noexcept { return bytes_; }

    /// Compared in constant time.
    friend bool operator==(const secret_string& first,
                           const secret_string& second) noexcept
    {
        return constant_time_equal(first.bytes_, second.bytes_);
    }

private:
    void wipe() noexcept
    {
        secure_wipe(bytes_.data(), bytes_.size());
        bytes_.clear();
    }

    std::string bytes_;
};

} // namespace vss::control
