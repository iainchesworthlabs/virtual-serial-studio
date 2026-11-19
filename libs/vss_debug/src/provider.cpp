//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/provider.hpp>

namespace vss::debug
{
namespace
{

op_result unsupported()
{
    return op_result::fail("unsupported", "Not available in this build.");
}

} // namespace

op_result provider::snapshot()
{
    return unsupported();
}
op_result provider::com_numbers()
{
    return unsupported();
}
op_result provider::driver()
{
    return unsupported();
}
op_result provider::plugins()
{
    return unsupported();
}
op_result provider::traffic(const std::string&)
{
    return unsupported();
}
op_result provider::run(const std::string&)
{
    return unsupported();
}
op_result provider::gui_state()
{
    return unsupported();
}
op_result provider::screenshot(const std::string&)
{
    return unsupported();
}

} // namespace vss::debug
