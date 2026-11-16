//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoding for a plugin: the messages `decoder.create`, `decoder.data`,
// `decoder.ack`, `decoder.stop`, `decoder.frame` and `trigger.publish`.
// docs/plugins.md, sections 20.4 to 20.7, are the design, and
// docs/plugin-api-reference.md, "Decoders, the decode stream and triggers",
// lists the messages.
//
// A decoder is an instance of the extension point of kind `decoder`.  The
// owner of the ports says which places it reads (`decoder_targets`, which is
// the binding of the setup, section 7.8) and where what it finds is kept
// (a `decode_store`, one for each instance).  This handler then:
//
// - sends `decoder.create` once the instance is running (`instances`), and
//   `decoder.stop` when it is not any more;
// - taps the hub for the places (traffic_tap.hpp) and sends what the tap
//   holds as `decoder.data`: the items in the order the hub reported them,
//   the bytes of all `data` items in one blob;
// - keeps the window of the page: at most 256 KiB of bytes sent and not yet
//   acknowledged by `decoder.ack`.  While the window is full nothing is
//   taken from the tap, so a slow plugin makes the tap drop.  A drop is not
//   silent: the tap leaves a `loss` item where the bytes were dropped
//   (`tap_limits::mark_losses`), and the stream says it as a `lost` item
//   with the place, the direction, the offset and the count of the bytes;
// - puts `decoder.frame` and `trigger.publish` into the instance's store,
//   with the limits the store has, and counts what it refuses.  The text of
//   a plugin stays untrusted plain text.
//
// What is not in the stream.  A `bus_fault` item needs the offset and the
// length of the sender's chunk, which the hub's event does not carry (the
// capability `events_bus_fault_detail` is absent), a `line_error` item needs
// the kind of the error, which a hub error does not say, and a `rule` item
// needs the rule events of the engine.  None of the three is sent.  A `time`
// item is not sent while nothing happens.  What is sent is `data`, `line` and
// `lost`.
//
// A place is a port, or a link, which is taken for a bus (`bus` is true in
// `decoder.create`).  Its rate and format are read from the `settings` of the
// place, "115200 8N1", when they read so, and left out otherwise.
//
// Who may do what.  `traffic.tap` is needed to see traffic, and so for
// `decoder.create` (without it the instance is not started) and for what the
// plugin sends back: `decoder.ack`, `decoder.frame` and `trigger.publish`
// from a plugin without it are refused.  Every message names an `instance`,
// which must be a running instance of kind `decoder` of this plugin, and a
// `place` that is one of its places.  These messages are notifications, so
// a refusal is counted (`decoder_state::refused`) and nothing is answered.
// Sent as a request, a message is answered with `bad_request`, never with
// `unknown_op`.
//
// The store is read from the GUI thread, or any thread, through
// `decode_store::snapshot`: a pointer to an immutable window, taken under a
// lock that is held for no more than a pointer swap.  Nothing the GUI reads
// is state of this handler's strand, so nothing needs the strand to be read.
// What the store keeps after the plugin has gone is the store's rule: it keeps
// it, and the handler adds one `decoder_lost` trigger for each place.
//
// Threads.  As for every handler: made, called and destroyed on the service's
// strand, and nothing here blocks.  What is posted checks a flag that the
// destructor clears.  The owner calls `refresh` on the strand when the state
// of an instance changes (`plugin_instances::on_change`), and the handler
// also does it when it is called for any other reason.
//

#pragma once

#include <vss/core/hub.hpp>
#include <vss/plugin/host/capture_point.hpp>
#include <vss/plugin/host/decode_store.hpp>
#include <vss/plugin/host/extension.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vss::plugin::host
{

/// What one decoder instance reads and where its frames and triggers go.
struct decoder_target
{
    /// The places, named as `place_finder` knows them: 1 to 64, no name
    /// twice.
    std::vector<std::string> places;
    /// Kept by the owner, who reads it from the GUI thread.
    std::shared_ptr<decode_store> store;
};

/// The binding of an instance, or nothing when the owner has none for it
/// (the instance is then not started, and asked about again on the next
/// `refresh`).  Called on the service's strand.
using decoder_targets =
    std::function<std::optional<decoder_target>(std::uint64_t instance)>;

/// One decoder instance as it stands.
struct decoder_state
{
    /// The handler knows the instance and has not let it go.
    bool known = false;
    /// `decoder.create` was answered with `ok` and the instance reads.
    bool running = false;
    /// The last `seq` sent, and the last that the plugin acknowledged.
    std::uint64_t sent_seq = 0;
    std::uint64_t acked_seq = 0;
    /// The bytes sent and not acknowledged.  At most 262144.
    std::size_t unacknowledged_bytes = 0;
    /// What was sent: messages, items and the bytes of `data` items.
    std::uint64_t messages = 0;
    std::uint64_t items = 0;
    std::uint64_t bytes = 0;
    /// The bytes told to the plugin as lost, and the number of `lost` items.
    std::uint64_t lost_bytes = 0;
    std::uint64_t lost_items = 0;
    /// Messages of the plugin that were refused.
    std::uint64_t refused = 0;
    /// Why the instance does not run, in plain words made here; empty when
    /// it runs or has not been tried.
    std::string reason;
};

class decoder_point : public extension_handler
{
public:
    /// Starts the decoders that are running and not started, and lets go of
    /// those that are not running now.  On the service's strand.
    virtual void refresh() = 0;

    /// How an instance stands.  On the service's strand.
    virtual decoder_state state_of(std::uint64_t instance) const = 0;
};

/// The extension point for one plugin.  `source` must outlive it.
std::unique_ptr<decoder_point>
make_decoder_point(const extension_context& context, hub& source,
                   place_finder find, decoder_targets targets);

} // namespace vss::plugin::host
