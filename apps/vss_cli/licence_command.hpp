//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/license/store.hpp>

#include <iosfwd>
#include <string>
#include <vector>

namespace vss_cli
{

/// `vss_cli licence ...`. `arguments` is everything after the program name,
/// starting with "licence". Looks in the licence folders of this computer and
/// prints to standard output and standard error. Returns the process exit
/// code: 0 when `licence` could look, whatever it found, and for an import
/// that copied the file or found it installed already; 1 when an import
/// copied nothing; 2 for a command line it does not understand.
int command_licence(const std::vector<std::string>& arguments);

/// The same with the folders, keys and clock in `options` and the text sent
/// to `out` and `err`, so a test uses folders of its own.
int command_licence(const std::vector<std::string>& arguments,
                    const vss::license::store_options& options,
                    std::ostream& out, std::ostream& err);

/// The text of `licence` in `--help`.
const char* licence_usage() noexcept;

} // namespace vss_cli
