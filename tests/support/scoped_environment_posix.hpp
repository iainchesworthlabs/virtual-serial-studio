//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// An environment variable that a test sets or removes, and that is put back
// as it was when the test ends.  For the tests of Linux and macOS.
//

#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace vss_test_support
{

class scoped_environment
{
public:
    /// Sets `name` to `value`, or removes it when `value` is null.
    scoped_environment(const char* name, const char* value) : name_(name)
    {
        // NOLINTBEGIN(concurrency-mt-unsafe)
        if (const char* old = std::getenv(name))
            old_ = std::string(old);
        if (value != nullptr)
            static_cast<void>(::setenv(name, value, 1));
        else
            static_cast<void>(::unsetenv(name));
        // NOLINTEND(concurrency-mt-unsafe)
    }

    ~scoped_environment()
    {
        // NOLINTBEGIN(concurrency-mt-unsafe)
        if (old_.has_value())
            static_cast<void>(::setenv(name_.c_str(), old_.value().c_str(), 1));
        else
            static_cast<void>(::unsetenv(name_.c_str()));
        // NOLINTEND(concurrency-mt-unsafe)
    }

    scoped_environment(const scoped_environment&) = delete;
    scoped_environment& operator=(const scoped_environment&) = delete;

private:
    std::string name_;
    std::optional<std::string> old_;
};

} // namespace vss_test_support
