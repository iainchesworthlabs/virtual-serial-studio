//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sections a plugin adds to a support bundle: the messages
// `bundle.prepare` and `bundle.result`.  docs/plugins.md, section 19.5, is the
// design, and bundle_sections.hpp is the host's check of what the plugin
// lists.
//
// The host asks and the plugin answers, so the plugin sends nothing of its
// own here: `take` takes no message.  The bundle dialog (in Studio) calls
// `prepare` for a row that the user ticked.  The handler:
//
// - refuses at once when the plugin was not granted `bundle.section`, or the
//   section's id or title are not as `check_sections` wants;
// - sends `bundle.prepare` and waits for `bundle.result` for at most 30
//   seconds;
// - reads the files the plugin lists, from `bundles/<section id>/` in the
//   plugin's data directory, on the executor it was given for slow work and
//   never on the service's strand: it takes each file's size and SHA-512 from
//   one open handle (`measure_file`) and checks the whole list again with
//   `verify_sections`;
// - calls `done` once, on the service's strand, with the list as it was
//   read.  What the dialog previews and saves is read from those files and
//   checked against those hashes, which is what makes the preview and the
//   saved bytes the same.
//
// A plugin that stops before it answers makes `done` say so.  `done` is called
// when the handler goes too, so that nothing waits for it for ever.  What the
// plugin wrote stays where it is.
//
// The progress and the cancelling of `bundle.prepare` are not built (the API
// reference lists them).
//

#pragma once

#include <vss/plugin/host/bundle_sections.hpp>
#include <vss/plugin/host/extension.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vss::plugin::host
{

/// What the dialog asks for one section.
struct bundle_ask
{
    /// The section's id: the plugin's own or a reserved one.
    std::string section;
    /// The title the dialog shows, from the manifest.  Untrusted text.
    std::string title;
    bool sensitive = false;
    /// What the user chose for the section, as one JSON object.
    std::string settings = "{}";
};

/// A section whose files were read.
struct bundle_prepared
{
    /// The list with the size and the hash of every file, as read.  A reader
    /// later passes it to `verify_sections` before it trusts a file.
    offered_section offered;
    verified_section verified;
};

/// How a request ended.  Either `prepared` or a reason, which is plain text
/// made here and never the plugin's own words.
struct bundle_outcome
{
    std::optional<bundle_prepared> prepared;
    std::string reason;
    /// The check that failed, when a check did.
    std::optional<bundle_failure> failure;
};

using bundle_done = std::function<void(bundle_outcome)>;

class bundle_point : public extension_handler
{
public:
    /// Asks the plugin for a section.  Called on the service's strand.
    virtual void prepare(bundle_ask ask, bundle_done done) = 0;
};

/// The extension point for one plugin.  `slow` is where the files are read.
/// `reserved_ids` are the ids besides its own that the plugin may use.
std::unique_ptr<bundle_point>
make_bundle_point(const extension_context& context,
                  boost::asio::any_io_executor slow,
                  std::vector<std::string> reserved_ids = {});

} // namespace vss::plugin::host
