//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The instances of one plugin: what the user wants of it (a recorder on
// these ports, a decoder on that link), kept in step with what the running
// plugin has been told.  docs/plugins.md, section 7.8, and the messages
// `instance.create`, `params.changed` and `instance.stop` of
// docs/plugin-api-reference.md are the design.
//
// A plugin does nothing until the host makes an instance of one of the
// extension points its manifest declares.  This object holds the list the
// owner wants and sends the plugin what is needed to get there:
//
// - when the plugin becomes ready, every wanted instance is created, in
//   the order of the list (a plugin that was started again has none);
// - an instance added to the list is created, one taken off is stopped,
//   and one whose parameters changed is told the new ones;
// - one message at a time for each instance: a change made while an
//   answer is awaited is sent after it;
// - what the plugin answers is kept: an instance it refuses stays refused,
//   with the plugin's reason as untrusted text, until its parameters
//   change or the plugin starts again.
//
// It checks an instance against the manifest before anything is sent: the
// kind and type must be an extension point the plugin declares, and no
// type may have more instances than the manifest allows.  It does not
// check parameter values: that is the parameter model's work, done by the
// owner before a list is given.
//
// It does no I/O of its own.  The owner gives it a way to send a request
// to the plugin (`plugin_runner::request`) and tells it when the plugin
// becomes ready and when it has gone.  Every member is called on one
// strand, the runner's.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

/// One instance the owner wants.
struct instance_spec
{
    /// The owner's number for it, not zero, the same for as long as the
    /// instance is wanted.  The plugin is told this number.
    std::uint64_t instance = 0;
    extension_kind kind = extension_kind::generator;
    /// The `id` of the extension point in the manifest.
    std::string type;
    /// The parameters, as the text of a JSON object, already checked
    /// against the manifest.
    std::string params = "{}";

    friend bool operator==(const instance_spec&,
                           const instance_spec&) = default;
};

enum class instance_state
{
    /// Wanted, and the plugin is not ready.
    waiting,
    /// "instance.create" was sent and not answered yet.
    creating,
    /// The plugin has it.
    running,
    /// The plugin, or this object, refused it.  `problem` and `reason`
    /// say why.
    refused,
    /// "instance.stop" was sent and not answered yet.
    stopping
};

std::string_view to_string(instance_state value) noexcept;

/// One instance as it stands.
struct instance_view
{
    instance_spec spec;
    instance_state state = instance_state::waiting;
    /// Why it is refused: `plugin_errc::capability_missing` when the
    /// manifest declares no such extension point, `too_many` when the
    /// manifest allows no more of the type, `misbehaving` when the plugin
    /// answered with an error, `unresponsive` when it did not answer.
    error_code problem;
    /// The plugin's own words for a refusal: untrusted plain text, cut to
    /// a few hundred bytes.  Empty when this object refused.
    std::string reason;
    /// True while the plugin has parameters older than `spec.params` and
    /// the newer ones are on their way.
    bool params_pending = false;
};

class plugin_instances
{
public:
    /// The answer to a request: the plugin's answer, or an error and an
    /// empty message.  As `plugin_runner::answer_handler`.
    using answer_handler = std::function<void(error_code, const api_message&)>;
    /// Sends a request to the plugin.  As `plugin_runner::request`: the
    /// handler is called exactly once, later, on the same strand.
    using sender = std::function<void(api_message, answer_handler)>;
    using change_handler = std::function<void()>;

    /// `declared` is what the plugin's manifest offers.
    plugin_instances(std::vector<extension_point> declared, sender send);
    ~plugin_instances();
    plugin_instances(const plugin_instances&) = delete;
    plugin_instances& operator=(const plugin_instances&) = delete;

    /// Called whenever an instance's state changes.  Set once.
    void on_change(change_handler handler);

    /// The list the owner wants, replacing the one before.  An entry with
    /// the number of an earlier entry is that instance: its parameters
    /// may have changed, its kind and type may not: such an entry is
    /// refused, and the plugin is told to stop the instance it has under
    /// that number.  A number that comes twice counts once, the first time.
    void want(std::vector<instance_spec> wanted);

    /// The plugin said hello and is ready: it has no instances yet.
    void plugin_ready();

    /// The plugin stopped, failed or is being stopped: it has none any
    /// more, and nothing is sent until it is ready again.
    void plugin_gone();

    /// Every wanted instance as it stands, in the order of the list.
    std::vector<instance_view> views() const;

    /// The instance with this number, when it is wanted.
    std::optional<instance_view> find(std::uint64_t instance) const;

    /// Whether the plugin has this instance now (`running`, or `running`
    /// with newer parameters on their way).  What an extension point asks
    /// before it lets a plugin act for an instance.
    bool running(std::uint64_t instance) const noexcept;

private:
    struct state;
    std::shared_ptr<state> state_;
};

} // namespace vss::plugin::host
