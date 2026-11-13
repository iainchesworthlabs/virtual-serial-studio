//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/license/error.hpp>

#include <string>

namespace vss::license
{

namespace
{

class category_impl final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.license"; }

    std::string message(int value) const override
    {
        switch (static_cast<errc>(value))
        {
        case errc::success:
            return "success";
        case errc::too_large:
            return "the file is too large";
        case errc::malformed:
            return "the file is damaged or is not a signed file";
        case errc::newer_version:
            return "the file was made for a newer version";
        case errc::wrong_purpose:
            return "the file is signed for a different purpose";
        case errc::unknown_key:
            return "the file is signed with a key this version does not know";
        case errc::key_not_valid:
            return "the signing key was withdrawn or was not valid then";
        case errc::bad_signature:
            return "the signature does not verify";
        }
        return "unknown licence error";
    }
};

} // namespace

const boost::system::error_category& error_category() noexcept
{
    static const category_impl instance;
    return instance;
}

boost::system::error_code make_error_code(errc value) noexcept
{
    return {static_cast<int>(value), error_category()};
}

} // namespace vss::license
