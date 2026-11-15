//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a running plugin may ask of the host beyond the common messages:
// the extension points.  docs/plugins.md, sections 7.4 to 7.7, 19 and 20,
// are the design; docs/plugin-api-reference.md lists the messages.
//
// The service starts and supervises plugins and knows nothing of ports.
// The program that owns the ports (Studio) gives the service a factory
// that makes, for each plugin that starts, the handlers of the extension
// points that program offers: recording, generators, decoders, the
// control relay.  A message from the plugin that is not one of the common
// ones is offered to each handler in turn; the first that takes it answers
// it.  A message nobody takes is answered as an "op" this host does not
// know, which is what a plugin sees on a Studio that lacks the extension
// point.
//
// A handler does only what the plugin was granted: the context names the
// permissions.  It never trusts a number or a name the plugin sends: an
// instance must be one the host made and the plugin holds (`instances`),
// a place one the plugin's instance was given.
//
// Threads.  Handlers are made, called and destroyed on the service's
// strand.  They must not block and must not wait for the plugin.  Work
// that takes time goes to another executor and comes back.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vss::plugin::host
{

/// The ways a handler speaks to its plugin.  Each returns at once.  After
/// the plugin has gone they do nothing, and a request's handler is called
/// with `api_errc::plugin_unavailable`.
struct plugin_link
{
    using answer_handler = std::function<void(error_code, const api_message&)>;

    /// Answers a request of the plugin; `re` is the request's `id`.
    std::function<void(std::uint64_t re, api_message message)> answer;
    /// Sends a message that wants no answer.
    std::function<void(api_message message)> notify;
    /// Sends a request; `handler` is called exactly once.
    std::function<void(api_message message, answer_handler handler,
                       std::chrono::milliseconds timeout)>
        request;
};

/// What a handler knows of the plugin it serves.
struct extension_context
{
    std::string plugin_id;
    /// The plugin's name from its manifest: untrusted text.
    std::string plugin_name;
    /// The permissions that were granted.  Never what was asked for.
    std::vector<permission> granted;
    /// The plugin's data directory.
    std::filesystem::path data_directory;
    /// The instances the host made for this plugin.  A handler asks
    /// `instances->running(n)` before it acts for instance `n`, and reads
    /// an instance's kind, type and parameters from it.
    std::shared_ptr<const plugin_instances> instances;
    /// The service's strand.
    boost::asio::any_io_executor executor;
    plugin_link link;

    bool may(permission wanted) const noexcept;
};

/// One extension point for one plugin.
class extension_handler
{
public:
    extension_handler() = default;
    virtual ~extension_handler() = default;
    extension_handler(const extension_handler&) = delete;
    extension_handler& operator=(const extension_handler&) = delete;

    /// A message from the plugin that is not one of the common ones.
    /// Returns true when this handler takes it: it then answers a request
    /// (`message.id` is not zero), now or later, exactly once, with `ok`,
    /// its own answer or an `error`.  Returns false when the "op" is not
    /// this handler's.
    virtual bool take(const api_message& message) = 0;

    /// The plugin said hello and is ready.  It has no instances yet.
    virtual void plugin_ready() {}

    /// The plugin stopped or failed.  Everything done for it is undone:
    /// recordings are closed, sessions ended, queues dropped.  A plugin
    /// that starts again starts from nothing.
    virtual void plugin_gone() {}
};

/// Makes the handlers for a plugin that is being started.  Called on the
/// service's strand.
using extension_factory =
    std::function<std::vector<std::unique_ptr<extension_handler>>(
        const extension_context&)>;

/// An `error` answer for a request, with one of the codes of the plugin
/// API and a plain line for the plugin's log.
api_message error_answer(api_errc code, std::string text);

/// The `ok` answer with nothing in it.
api_message ok_answer();

} // namespace vss::plugin::host
