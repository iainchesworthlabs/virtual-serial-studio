//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/update/error.hpp>

#include <string>
#include <utility>

namespace vss::update
{
namespace
{

class update_category_impl final : public boost::system::error_category
{
public:
    const char* name() const noexcept override { return "vss.update"; }

    std::string message(int value) const override
    {
        switch (static_cast<update_errc>(value))
        {
        case update_errc::success:
            return "success";
        case update_errc::no_update_keys:
            return "this build has no keys to check an update feed with";
        case update_errc::signature_invalid:
            return "the update information could not be trusted";
        case update_errc::feed_malformed:
            return "the update information is not in a form this version "
                   "reads";
        case update_errc::feed_version_unsupported:
            return "the update information is in a newer format";
        case update_errc::feed_rolled_back:
            return "the update information is older than one already seen";
        }
        return "unknown update error";
    }
};

} // namespace

const boost::system::error_category& update_category() noexcept
{
    static const update_category_impl instance;
    return instance;
}

boost::system::error_code make_error_code(update_errc value) noexcept
{
    return {std::to_underlying(value), update_category()};
}

std::string_view to_string(update_errc value) noexcept
{
    switch (value)
    {
    case update_errc::success:
        return "success";
    case update_errc::no_update_keys:
        return "no_update_keys";
    case update_errc::signature_invalid:
        return "signature_invalid";
    case update_errc::feed_malformed:
        return "feed_malformed";
    case update_errc::feed_version_unsupported:
        return "feed_version_unsupported";
    case update_errc::feed_rolled_back:
        return "feed_rolled_back";
    }
    return "unknown";
}

} // namespace vss::update
