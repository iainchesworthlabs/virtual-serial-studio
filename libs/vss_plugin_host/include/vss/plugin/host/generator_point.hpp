//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Generators for a plugin: the messages `generator.start`,
// `generator.write`, `generator.credit`, `generator.pattern`,
// `generator.stop` and `generator.done`.  docs/plugins.md, sections 6.4,
// 7.4 and 7.9, are the design, and generator_feed.hpp holds the rules of
// the credit, the pacing and the back-pressure.  This file joins one feed
// for each generator instance to a plugin and to a port of the hub.
//
// A generator belongs to an instance.  The host makes the instance
// (`instance.create`, see instances.hpp); when the plugin has it, the
// handler asks the owner for the port the instance's parameters name (the
// parameter `port`, a name as the hub names it), sends `generator.start`
// with that port's name and, when the plugin answers `ok`,
// gives it its first credit.  From then on:
//
// - `generator.write` goes to the instance's feed.  The feed decides: a
//   write beyond the credit, for a generator that is not running, or not
//   allowed by the instance's timing class is refused and counted, and
//   enough of them end the generator.  A write the feed takes is written to
//   the port's endpoint at once, or held by the feed while the endpoint has
//   as much waiting as the window allows.  Credit goes back to the plugin
//   only as bytes are taken by the endpoint, so the host never holds more
//   than the window for a generator, in the feed and in the endpoint
//   together.
// - `generator.done` lets what is held be written and then ends the
//   generator.
// - `generator.pattern` asks the engine to make a pattern, which needs an
//   instance that declared timing class "rules" (answered `not_declared`
//   otherwise).  This host cannot make patterns yet, so it is answered
//   `unsupported` for such an instance.
// - `generator.stop` is sent by the host, when an instance is no longer
//   running: the feed is stopped and what it holds is dropped.
// - The host's own messages (`start`, `credit`, `stop`) sent by a plugin are
//   answered `bad_request`, as a request, and counted.
//
// A message for an instance that the plugin does not hold, that is not
// running or that is not a generator is answered `refused`.  Without the
// permission `ports.write` every generator message is answered
// `not_granted`.  A message that is not a notification is always answered,
// exactly once; a notification never is, and what is wrong with one is
// counted (`generator_counts`).
//
// Nothing here blocks a strand.  A write to the port is queued on the
// endpoint, and its result comes back to the service's strand.
//

#pragma once

#include <vss/core/endpoint.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/generator_feed.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace vss::plugin::host
{

/// Finds the endpoint a generator writes to by its name, as the hub names
/// it, or returns nothing when the owner has no such port or does not let
/// plugins write to it.  Called on the service's strand.  The endpoint
/// must stay valid for as long as it is held: the handler holds the
/// pointer.
using generator_port_finder =
    std::function<std::shared_ptr<endpoint>(std::string_view name)>;

/// The timing class an instance of this type declared in the manifest:
/// `feed_timing::rules` or `feed_timing::async`.  Called on the service's
/// strand with the `id` of the extension point.  When it is not set, every
/// type is `async`.
using generator_timing_finder = std::function<feed_timing(std::string_view)>;

/// What the handler needs of the owner of the ports.
struct generator_owner
{
    generator_port_finder find_port;
    generator_timing_finder timing_of;
};

/// What the handler did not do, counted since it was made.
struct generator_counts
{
    /// Generators started, and ones that could not be: the parameters name
    /// no port, the owner has none of that name, or the plugin did not
    /// answer `generator.start` with `ok`.
    std::uint64_t started = 0;
    std::uint64_t start_failed = 0;
    /// Writes the feed refused, and writes that were dropped before they
    /// reached a feed: for an instance that is not running, not started
    /// or already ended, and ones that were not as the API says.
    std::uint64_t writes_refused = 0;
    std::uint64_t writes_dropped = 0;
    /// Messages of the host's own side that a plugin sent.
    std::uint64_t wrong_side = 0;
    /// Bytes the endpoints took, and writes the endpoints failed.
    std::uint64_t bytes_written = 0;
    std::uint64_t write_errors = 0;
    /// Generators the feed ended as `misbehaving`.
    std::uint64_t misbehaved = 0;
};

class generator_point : public extension_handler
{
public:
    /// Brings the generators in step with the instances: one is started
    /// for each running instance of kind generator that has none, and one
    /// whose instance is not running any more is stopped.  The owner calls
    /// it, on the service's strand, when the instances changed.  The handler
    /// also calls it when the plugin becomes ready and before it takes a
    /// message.
    virtual void sync() = 0;

    virtual generator_counts counts() const = 0;
};

/// The extension point for one plugin.
std::unique_ptr<generator_point>
make_generator_point(const extension_context& context, generator_owner owner);

} // namespace vss::plugin::host
