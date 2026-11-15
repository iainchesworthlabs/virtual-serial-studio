//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/private_file.hpp>

#include <memory>
#include <string_view>

namespace vss::control
{

error_code create_private_file(const std::filesystem::path& path,
                               std::string_view bytes)
{
    auto claim = claim_private_file(path);
    if (!claim)
        return claim.error();
    if (const error_code failed = (*claim)->write(bytes))
        return failed;
    (*claim)->keep();
    return {};
}

} // namespace vss::control
