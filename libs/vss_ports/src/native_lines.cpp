//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>
#include <vss/ports/native_lines.hpp>
#include <vss/trace/recorder.hpp>

#include "platform/native_lines_platform.hpp"

namespace vss::native
{

bool supported() noexcept
{
    return true;
}

result<modem_status> read_modem_status(std::intptr_t handle)
{
    if (handle == 0)
        return make_error_code(errc::not_open);
    VSS_PROFILE_ZONE("ports.native.read_modem_status");

    auto out = detail::platform_read_modem_status(handle);
    if (!out)
    {
        // Polled often, so only a failure is kept.
        trace::record_failure("native.read_modem_status", 0, {}, out.error());
        return out;
    }

    out->lines.set(control_line::cts, out->cts);
    out->lines.set(control_line::dsr, out->dsr);
    out->lines.set(control_line::dcd, out->dcd);
    out->lines.set(control_line::ri, out->ri);
    return out;
}

result<void> write_line(std::intptr_t handle, control_line line, bool state)
{
    if (handle == 0)
        return make_error_code(errc::not_open);

    VSS_PROFILE_ZONE("ports.native.write_line");
    trace::call_scope scope("native.write_line", 0,
                            state ? "line set" : "line clear");
    auto done = detail::platform_write_line(handle, line, state);
    scope.finish(done ? boost::system::error_code{} : done.error());
    return done;
}

result<void> set_break(std::intptr_t handle, bool active)
{
    if (handle == 0)
        return make_error_code(errc::not_open);

    VSS_PROFILE_ZONE("ports.native.set_break");
    trace::call_scope scope("native.set_break", 0,
                            active ? "break on" : "break off");
    auto done = detail::platform_set_break(handle, active);
    scope.finish(done ? boost::system::error_code{} : done.error());
    return done;
}

} // namespace vss::native
