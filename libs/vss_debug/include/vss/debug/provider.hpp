//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/debug/protocol.hpp>

#include <string>

namespace vss::debug
{

/// What a studio can tell and do for the debug server.  The server owns the
/// connections, the approval, the scopes, the audit and the redaction; a
/// provider only produces the content.  Every call may be made from a worker
/// thread of the server, never from the thread that reads the connections,
/// and several at once.  A call that needs the window's thread must marshal
/// there itself and give up after a deadline instead of waiting for ever.
///
/// An operation a provider does not have says so: the default of every call
/// is `unsupported`, which the client shows as "not available in this build".
class provider
{
public:
    virtual ~provider() = default;

    /// Every endpoint and link with its state, counters and last error.
    virtual op_result snapshot();
    /// The COM number picture (Windows).
    virtual op_result com_numbers();
    /// The driver and the helper service.
    virtual op_result driver();
    /// The plugin host.
    virtual op_result plugins();
    /// The bytes of traffic, for the ports `args_json` names.  Called only
    /// when the person's second switch is on.
    virtual op_result traffic(const std::string& args_json);
    /// Runs one engine operation from a closed list and returns the result
    /// with the whole error chain.  `args_json` is `{"op": "...", ...}`.
    virtual op_result run(const std::string& args_json);
    /// Which pane is open, the document, the values of the open sheet.
    virtual op_result gui_state();
    /// A picture of a window as PNG bytes in the blob.
    virtual op_result screenshot(const std::string& args_json);
};

} // namespace vss::debug
