//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Includes every public header on its own to prove each one is
// self-contained.

#include <vss/agent/client.hpp>
#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/agent/proto/message.hpp>
#include <vss/agent/server.hpp>

#include <vss/core/bus_line.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/control_lines.hpp>
#include <vss/core/endpoint.hpp>
#include <vss/core/error.hpp>
#include <vss/core/event.hpp>
#include <vss/core/hub.hpp>
#include <vss/core/link.hpp>
#include <vss/core/serial_settings.hpp>
#include <vss/core/stats.hpp>
#include <vss/core/traffic_log.hpp>
#include <vss/core/transport.hpp>
#include <vss/core/types.hpp>

#include <vss/ports/com0com.hpp>
#include <vss/ports/loopback_transport.hpp>
#include <vss/ports/native_lines.hpp>
#include <vss/ports/serial_enumeration.hpp>
#include <vss/ports/serial_transport.hpp>
#include <vss/ports/virtual_port.hpp>

#include <vss/config/document.hpp>

#include <vss/rules/compile.hpp>
#include <vss/rules/machine.hpp>
#include <vss/rules/model.hpp>
#include <vss/rules/parse.hpp>
#include <vss/rules/random.hpp>
#include <vss/rules/scheduler.hpp>

#include <vss/studio/studio.hpp>

#include <vss/testkit/harness.hpp>

#include <cstdio>
#include <exception>

int main()
try
{
    // Touch a few symbols so the translation unit is not optimised away.
    const auto size = vss::parse_byte_size("64KB");
    const auto label = vss::to_label(vss::serial_settings{});
    std::printf("headers ok: %s %s\n", size ? "size" : "no-size",
                label.c_str());
    return size && *size == std::uint64_t{64} * 1024 ? 0 : 1;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "error: %s\n", error.what());
    return 2;
}
catch (...)
{
    std::fputs("error: unknown exception\n", stderr);
    return 2;
}
