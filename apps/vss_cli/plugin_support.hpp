//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the `vss_cli plugin` commands share: showing text that came from a
// plugin safely, finding this computer's plugin settings, and the words and
// the JSON of the verdict on one plugin folder.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/install.hpp>

#include <boost/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vss_cli::plugin_support
{

/// `text` with every byte outside printable ASCII replaced by `?`.
std::string plain(std::string_view text);

/// `text` as valid UTF-8, with each byte that is not part of a character
/// replaced by `?`.  A JSON text must be valid UTF-8.
std::string valid_utf8(std::string_view text);

/// The path with `/` between the names, as UTF-8.
std::string path_text(const std::filesystem::path& path);

/// The folder of the running program, or an empty path when it cannot be
/// found.
std::filesystem::path program_directory();

boost::json::value text_value(std::string_view text);

/// The reason code as a word (`signature_invalid`), or the system's own words
/// for any other error.
std::string code_text(const vss::error_code& problem);

/// What this computer says about plugins, and what could not be read.
struct computer
{
    vss::plugin::inspect_context context;
    /// The policy file, or empty when it was not read.
    std::filesystem::path policy_file;
    std::vector<std::string> notes;
};

/// The context for this computer.  The policy file is read only when
/// `read_policy` is true.
computer this_computer(bool read_policy);

/// The trust line of the plugin screen: "Official", "Signed by <author>",
/// "Unsigned" or "Signed by an author you have not approved".  Empty when
/// the plugin has no level.
std::string trust_line(const vss::plugin::inspection& found,
                       const vss::plugin::inspect_context& context);

/// The signature sentence of the plugin screen, with the key's id.
std::string signature_line(const vss::plugin::inspection& found,
                           const vss::plugin::inspect_context& context);

/// "yes", or "no" and the reason code and its sentence.
std::string run_line(const vss::plugin::inspection& found);

/// Whether the licence covers the plugin, in words.
std::string licence_line(const vss::plugin::inspection& found,
                         const vss::plugin::inspect_context& context);

/// The lines "Signature:", "Trust:", "Key:", "May run:" and "Licence:", each
/// indented two spaces and ended with a line feed.
void add_verdict_lines(std::string& out,
                       const vss::plugin::inspection& found,
                       const vss::plugin::inspect_context& context);

/// The same as the members of a JSON object: `signature_state`, `trust`,
/// `trust_line`, `key_id`, `may_run`, `run_reason`, `run_detail` and
/// `licence`.
void add_verdict_json(boost::json::object& item,
                      const vss::plugin::inspection& found,
                      const vss::plugin::inspect_context& context);

} // namespace vss_cli::plugin_support
