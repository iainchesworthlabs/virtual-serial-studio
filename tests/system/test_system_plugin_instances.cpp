//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::plugin_instances`, the list of instances a plugin is
// asked to have.  The class does no I/O, so each test plays the plugin from
// memory: a fake records every request the class sends, decoded, and the
// test answers any of them later, in any order, with "ok", an "error" or a
// failure of the transport.  The last test is a property test: random lists
// and random answers, and a few rules that must hold after every step.
//

#include <vss/core/error.hpp>
#include <vss/plugin/api_instance.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::decode_as;
using vss::plugin::error_message;
using vss::plugin::extension_kind;
using vss::plugin::extension_point;
using vss::plugin::instance_create_message;
using vss::plugin::instance_stop_message;
using vss::plugin::make_error_code;
using vss::plugin::ok_message;
using vss::plugin::op_of;
using vss::plugin::params_changed_message;
using vss::plugin::plugin_errc;
using vss::plugin::host::instance_spec;
using vss::plugin::host::instance_state;
using vss::plugin::host::instance_view;
using vss::plugin::host::plugin_instances;

namespace
{

constexpr std::string_view create_op = instance_create_message::op();
constexpr std::string_view stop_op = instance_stop_message::op();
constexpr std::string_view change_op = params_changed_message::op();

using number_list = std::vector<std::uint64_t>;

// -- the manifest -----------------------------------------------------------

extension_point point_for(extension_kind kind, std::string_view id,
                          std::uint32_t most)
{
    extension_point point;
    point.kind = kind;
    point.id = std::string(id);
    point.max_instances = most;
    return point;
}

/// A generator `replay` (two at most), an adapter `recorder` (one) and a
/// decoder `modbus` (sixteen).
std::vector<extension_point> declared_points()
{
    return {point_for(extension_kind::generator, "replay", 2),
            point_for(extension_kind::adapter, "recorder", 1),
            point_for(extension_kind::decoder, "modbus", 16)};
}

instance_spec spec_for(std::uint64_t number, extension_kind kind,
                       std::string_view type, std::string_view params)
{
    instance_spec spec;
    spec.instance = number;
    spec.kind = kind;
    spec.type = std::string(type);
    spec.params = std::string(params);
    return spec;
}

instance_spec replay_spec(std::uint64_t number, std::string_view params = "{}")
{
    return spec_for(number, extension_kind::generator, "replay", params);
}

instance_spec recorder_spec(std::uint64_t number,
                            std::string_view params = "{}")
{
    return spec_for(number, extension_kind::adapter, "recorder", params);
}

instance_spec modbus_spec(std::uint64_t number, std::string_view params = "{}")
{
    return spec_for(number, extension_kind::decoder, "modbus", params);
}

// -- the plugin, played from memory ----------------------------------------

/// A request the class sent, with what the plugin could read from it.
struct sent_request
{
    api_message message;
    plugin_instances::answer_handler handler;
    std::string op;
    std::uint64_t instance = 0;
    extension_kind kind = extension_kind::generator;
    std::string type;
    std::string params;
    /// The message could be read as the message its "op" names.
    bool valid = false;
    bool answered = false;
    /// How many times the plugin had gone when this was sent.
    std::size_t life = 0;
};

api_message ok_reply()
{
    api_message reply;
    reply.body = ok_message{};
    reply.op = std::string(op_of(reply.body));
    return reply;
}

api_message error_reply(std::string text)
{
    error_message body;
    body.category = "vss.plugin.api";
    body.value = "refused";
    body.message = std::move(text);
    api_message reply;
    reply.body = std::move(body);
    reply.op = std::string(op_of(reply.body));
    return reply;
}

class fake_plugin
{
public:
    /// What the class under test is given to send with.
    plugin_instances::sender sender()
    {
        return [this](api_message message,
                      plugin_instances::answer_handler handler)
        { record(std::move(message), std::move(handler)); };
    }

    /// The plugin is about to be told it is ready.
    void up() { ready_ = true; }

    /// The plugin is about to be told it has gone: what it had is lost, and
    /// every request still waiting belongs to a plugin that is no more.
    void down()
    {
        ready_ = false;
        ++life_;
        open_.clear();
        alive_.clear();
    }

    std::size_t count() const noexcept { return requests_.size(); }

    const sent_request& at(std::size_t index) const
    {
        REQUIRE(index < requests_.size());
        return requests_[index];
    }

    /// How many requests of this op were sent, for any instance.
    std::size_t count_of(std::string_view op) const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(requests_, [op](const sent_request& item)
                                  { return item.op == op; }));
    }

    /// How many requests of this op were sent for this instance.
    std::size_t count_of(std::string_view op, std::uint64_t instance) const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            requests_, [op, instance](const sent_request& item)
            { return item.op == op && item.instance == instance; }));
    }

    bool has_pending(std::string_view op, std::uint64_t instance) const
    {
        return std::ranges::any_of(requests_,
                                   [op, instance](const sent_request& item)
                                   {
                                       return !item.answered && item.op == op &&
                                              item.instance == instance;
                                   });
    }

    /// The latest request of this op for this instance that has no answer.
    std::size_t pending(std::string_view op, std::uint64_t instance) const
    {
        std::size_t found = requests_.size();
        for (std::size_t index = requests_.size(); index > 0; --index)
        {
            const sent_request& item = requests_[index - 1];
            if (!item.answered && item.op == op && item.instance == instance)
            {
                found = index - 1;
                break;
            }
        }
        REQUIRE(found < requests_.size());
        return found;
    }

    /// Every request without an answer, of this life of the plugin and of
    /// the ones before.
    std::vector<std::size_t> unanswered() const
    {
        std::vector<std::size_t> out;
        for (std::size_t index = 0; index < requests_.size(); ++index)
        {
            if (!requests_[index].answered)
                out.push_back(index);
        }
        return out;
    }

    void answer_ok(std::size_t index)
    {
        finish(index, vss::error_code{}, ok_reply(), true);
    }

    void answer_error(std::size_t index, std::string text)
    {
        finish(index, vss::error_code{}, error_reply(std::move(text)), false);
    }

    /// The transport failed: an error code and an empty message.
    void answer_failure(std::size_t index, api_errc code)
    {
        finish(index, make_error_code(code), api_message{}, false);
    }

    std::size_t sent_while_down() const noexcept { return sent_while_down_; }
    std::size_t double_sends() const noexcept { return double_sends_; }
    std::size_t unreadable() const noexcept { return unreadable_; }

    /// Whether an instance has more than one request without an answer in
    /// this life of the plugin.
    bool any_open_twice() const
    {
        return std::ranges::any_of(open_, [](const auto& entry)
                                   { return entry.second > 1; });
    }

    /// How many instances of a type the plugin has now.
    std::size_t alive_of_type(std::string_view type) const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(alive_, [type](const auto& entry)
                                  { return entry.second == type; }));
    }

private:
    static bool read_into(sent_request& item, const api_message& message)
    {
        if (message.op == create_op)
        {
            const vss::result<instance_create_message> body =
                decode_as<instance_create_message>(message);
            if (!body.has_value())
                return false;
            item.instance = body.value().instance;
            item.kind = body.value().kind;
            item.type = body.value().type;
            item.params = body.value().params;
            return true;
        }
        if (message.op == stop_op)
        {
            const vss::result<instance_stop_message> body =
                decode_as<instance_stop_message>(message);
            if (!body.has_value())
                return false;
            item.instance = body.value().instance;
            return true;
        }
        if (message.op == change_op)
        {
            const vss::result<params_changed_message> body =
                decode_as<params_changed_message>(message);
            if (!body.has_value())
                return false;
            item.instance = body.value().instance;
            item.params = body.value().params;
            return true;
        }
        return false;
    }

    void record(api_message message, plugin_instances::answer_handler handler)
    {
        sent_request item;
        item.op = message.op;
        item.life = life_;
        item.valid = read_into(item, message);
        if (!item.valid)
            ++unreadable_;
        if (!ready_)
            ++sent_while_down_;
        std::size_t& opened = open_[item.instance];
        if (opened > 0)
            ++double_sends_;
        ++opened;
        if (item.op == stop_op)
            alive_.erase(item.instance);
        item.message = std::move(message);
        item.handler = std::move(handler);
        requests_.push_back(std::move(item));
    }

    void finish(std::size_t index, vss::error_code failed,
                const api_message& reply, bool accepted)
    {
        REQUIRE(index < requests_.size());
        REQUIRE(!requests_[index].answered);
        requests_[index].answered = true;
        if (requests_[index].life == life_)
        {
            const auto found = open_.find(requests_[index].instance);
            if (found != open_.end() && found->second > 0)
                --found->second;
            if (accepted && requests_[index].op == create_op)
                alive_[requests_[index].instance] = requests_[index].type;
        }
        // The handler may send again, which moves the list: call a copy.
        const plugin_instances::answer_handler handler =
            requests_[index].handler;
        handler(failed, reply);
    }

    std::vector<sent_request> requests_;
    bool ready_ = true;
    std::size_t life_ = 0;
    /// Requests without an answer in this life, by instance.
    std::map<std::uint64_t, std::size_t> open_;
    /// The instances the plugin has in this life, with their types.
    std::map<std::uint64_t, std::string> alive_;
    std::size_t sent_while_down_ = 0;
    std::size_t double_sends_ = 0;
    std::size_t unreadable_ = 0;
};

/// The class under test with the plugin it sends to.
struct rig
{
    fake_plugin plugin;
    plugin_instances instances;

    rig() : instances(declared_points(), plugin.sender()) {}
};

instance_view view_of(const plugin_instances& instances, std::uint64_t number)
{
    const std::optional<instance_view> found = instances.find(number);
    REQUIRE(found.has_value());
    return found.value();
}

bool has_state(const plugin_instances& instances, std::uint64_t number,
               instance_state wanted)
{
    const std::optional<instance_view> found = instances.find(number);
    return found.has_value() && found.value().state == wanted;
}

/// A request that is a "instance.create" of exactly this instance.
void require_create(const sent_request& item, const instance_spec& spec)
{
    REQUIRE(item.valid);
    REQUIRE_EQ(item.op, create_op);
    REQUIRE_EQ(item.instance, spec.instance);
    REQUIRE(item.kind == spec.kind);
    REQUIRE_EQ(item.type, spec.type);
    REQUIRE_EQ(item.params, spec.params);
}

void require_stop(const sent_request& item, std::uint64_t number)
{
    REQUIRE(item.valid);
    REQUIRE_EQ(item.op, stop_op);
    REQUIRE_EQ(item.instance, number);
}

void require_change(const sent_request& item, std::uint64_t number,
                    std::string_view params)
{
    REQUIRE(item.valid);
    REQUIRE_EQ(item.op, change_op);
    REQUIRE_EQ(item.instance, number);
    REQUIRE_EQ(item.params, params);
}

/// A text of this many bytes that does not repeat itself soon.
std::string text_of_size(std::size_t size)
{
    std::string text;
    for (std::size_t index = 0; index < size; ++index)
        text.push_back(static_cast<char>('a' + index % 26));
    return text;
}

} // namespace

// -- before the plugin is ready ---------------------------------------------

TEST_CASE("plugin instances: nothing is sent before the plugin is ready")
{
    rig run;
    run.instances.want({replay_spec(1), recorder_spec(2), modbus_spec(3)});

    REQUIRE_EQ(run.plugin.count(), std::size_t{0});
    const std::vector<instance_view> views = run.instances.views();
    REQUIRE_EQ(views.size(), std::size_t{3});
    for (const instance_view& view : views)
    {
        REQUIRE(view.state == instance_state::waiting);
        REQUIRE(!view.problem);
        REQUIRE(view.reason.empty());
        REQUIRE(!view.params_pending);
    }
    REQUIRE(!run.instances.running(1));
    REQUIRE(!run.instances.running(2));
    REQUIRE(!run.instances.running(3));
}

TEST_CASE("plugin instances: ready creates every wanted instance in order")
{
    rig run;
    const std::vector<instance_spec> wanted{replay_spec(5, R"({"speed":2})"),
                                            recorder_spec(6),
                                            modbus_spec(7, R"({"unit":3})")};
    run.instances.want(wanted);
    run.instances.plugin_ready();

    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    require_create(run.plugin.at(0), wanted[0]);
    require_create(run.plugin.at(1), wanted[1]);
    require_create(run.plugin.at(2), wanted[2]);
    for (const std::uint64_t number : number_list{5, 6, 7})
    {
        REQUIRE(has_state(run.instances, number, instance_state::creating));
        REQUIRE(!run.instances.running(number));
    }

    // The views follow the list, whatever order the answers come in.
    run.plugin.answer_ok(run.plugin.pending(create_op, 6));
    REQUIRE(has_state(run.instances, 6, instance_state::running));
    REQUIRE(run.instances.running(6));
    REQUIRE(!run.instances.running(5));
    REQUIRE(!run.instances.running(7));

    run.plugin.answer_ok(run.plugin.pending(create_op, 7));
    run.plugin.answer_ok(run.plugin.pending(create_op, 5));
    const std::vector<instance_view> views = run.instances.views();
    REQUIRE_EQ(views.size(), std::size_t{3});
    REQUIRE_EQ(views[0].spec.instance, std::uint64_t{5});
    REQUIRE_EQ(views[1].spec.instance, std::uint64_t{6});
    REQUIRE_EQ(views[2].spec.instance, std::uint64_t{7});
    for (const instance_view& view : views)
    {
        REQUIRE(view.state == instance_state::running);
        REQUIRE(!view.problem);
        REQUIRE(!view.params_pending);
    }
    REQUIRE(run.instances.running(5));
    REQUIRE(run.instances.running(7));
    REQUIRE(!run.instances.running(8));
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
}

// -- adding, removing and changing ------------------------------------------

TEST_CASE("plugin instances: an added instance is created and a removed one "
          "is stopped")
{
    rig run;
    run.instances.want({replay_spec(1)});
    run.instances.plugin_ready();
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});

    // Added while the plugin is ready: created at once.
    run.instances.want({replay_spec(1), recorder_spec(2)});
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    require_create(run.plugin.at(1), recorder_spec(2));
    REQUIRE(has_state(run.instances, 2, instance_state::creating));
    run.plugin.answer_ok(run.plugin.pending(create_op, 2));

    // Removed: stopped, and gone from the views at once.
    run.instances.want({recorder_spec(2)});
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    require_stop(run.plugin.at(2), 1);
    REQUIRE(!run.instances.find(1).has_value());
    REQUIRE_EQ(run.instances.views().size(), std::size_t{1});
    REQUIRE(!run.instances.running(1));
    REQUIRE(run.instances.running(2));

    // Once the plugin has let go there is nothing more to say.
    run.plugin.answer_ok(run.plugin.pending(stop_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    REQUIRE(!run.instances.find(1).has_value());
    REQUIRE(!run.instances.running(1));
    REQUIRE_EQ(run.instances.views().size(), std::size_t{1});
}

TEST_CASE("plugin instances: new parameters are sent after the answer, and "
          "only the latest")
{
    rig run;
    run.instances.want({replay_spec(1, R"({"a":1})")});
    run.instances.plugin_ready();
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    REQUIRE(!view_of(run.instances, 1).params_pending);

    run.instances.want({replay_spec(1, R"({"a":2})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    require_change(run.plugin.at(1), 1, R"({"a":2})");
    REQUIRE(view_of(run.instances, 1).params_pending);
    REQUIRE(run.instances.running(1));

    // Two more changes while the first is in flight: nothing is sent yet.
    run.instances.want({replay_spec(1, R"({"a":3})")});
    run.instances.want({replay_spec(1, R"({"a":4})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});

    // The first is answered: the latest follows, not the one between.
    run.plugin.answer_ok(run.plugin.pending(change_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    require_change(run.plugin.at(2), 1, R"({"a":4})");
    REQUIRE(view_of(run.instances, 1).params_pending);

    run.plugin.answer_ok(run.plugin.pending(change_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    const instance_view view = view_of(run.instances, 1);
    REQUIRE(!view.params_pending);
    REQUIRE(view.state == instance_state::running);
    REQUIRE_EQ(view.spec.params, R"({"a":4})");
}

TEST_CASE("plugin instances: parameters changed while the create is in "
          "flight follow its answer")
{
    rig run;
    run.instances.want({replay_spec(1, R"({"a":1})")});
    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});

    run.instances.want({replay_spec(1, R"({"a":2})")});
    run.instances.want({replay_spec(1, R"({"a":3})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});
    REQUIRE(has_state(run.instances, 1, instance_state::creating));

    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    require_change(run.plugin.at(1), 1, R"({"a":3})");
    REQUIRE(has_state(run.instances, 1, instance_state::running));
    REQUIRE(view_of(run.instances, 1).params_pending);
    REQUIRE(run.instances.running(1));

    run.plugin.answer_ok(run.plugin.pending(change_op, 1));
    REQUIRE(!view_of(run.instances, 1).params_pending);
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
}

// -- what the plugin refuses -------------------------------------------------

TEST_CASE("plugin instances: a refused create stays refused until the "
          "parameters change")
{
    rig run;
    run.instances.want({replay_spec(1, R"({"a":1})")});
    run.instances.plugin_ready();

    const std::string text = text_of_size(2000);
    run.plugin.answer_error(run.plugin.pending(create_op, 1), text);

    const instance_view view = view_of(run.instances, 1);
    REQUIRE(view.state == instance_state::refused);
    REQUIRE(view.problem == make_error_code(plugin_errc::misbehaving));
    REQUIRE_EQ(view.reason.size(), std::size_t{400});
    REQUIRE_EQ(view.reason, text.substr(0, 400));
    REQUIRE(!run.instances.running(1));
    REQUIRE(!view.params_pending);

    // The same list again: the same parameters are not sent again.
    run.instances.want({replay_spec(1, R"({"a":1})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});
    REQUIRE(has_state(run.instances, 1, instance_state::refused));

    // Other parameters: the plugin is asked again.
    run.instances.want({replay_spec(1, R"({"a":2})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    require_create(run.plugin.at(1), replay_spec(1, R"({"a":2})"));
    const instance_view trying = view_of(run.instances, 1);
    REQUIRE(trying.state == instance_state::creating);
    REQUIRE(!trying.problem);
    REQUIRE(trying.reason.empty());

    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    REQUIRE(run.instances.running(1));
    REQUIRE(!view_of(run.instances, 1).problem);
}

TEST_CASE("plugin instances: refused parameters leave the instance running")
{
    rig run;
    run.instances.want({replay_spec(1, R"({"a":1})")});
    run.instances.plugin_ready();
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));

    run.instances.want({replay_spec(1, R"({"a":2})")});
    require_change(run.plugin.at(1), 1, R"({"a":2})");
    run.plugin.answer_error(run.plugin.pending(change_op, 1),
                            "cannot take that value");

    const instance_view view = view_of(run.instances, 1);
    REQUIRE(view.state == instance_state::running);
    REQUIRE(run.instances.running(1));
    REQUIRE(view.problem == make_error_code(plugin_errc::misbehaving));
    REQUIRE_EQ(view.reason, "cannot take that value");
    REQUIRE(!view.params_pending);

    // The refused parameters are not sent a second time.
    run.instances.want({replay_spec(1, R"({"a":2})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    REQUIRE(!view_of(run.instances, 1).params_pending);

    // Newer parameters are.
    run.instances.want({replay_spec(1, R"({"a":3})")});
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    require_change(run.plugin.at(2), 1, R"({"a":3})");
    REQUIRE(view_of(run.instances, 1).params_pending);

    run.plugin.answer_ok(run.plugin.pending(change_op, 1));
    const instance_view settled = view_of(run.instances, 1);
    REQUIRE(settled.state == instance_state::running);
    REQUIRE(!settled.problem);
    REQUIRE(settled.reason.empty());
    REQUIRE(!settled.params_pending);
}

TEST_CASE("plugin instances: a transport failure on create is refused as "
          "unresponsive")
{
    rig run;
    run.instances.want({replay_spec(1), recorder_spec(2)});
    run.instances.plugin_ready();

    run.plugin.answer_failure(run.plugin.pending(create_op, 1),
                              api_errc::plugin_unavailable);
    run.plugin.answer_failure(run.plugin.pending(create_op, 2),
                              api_errc::timeout);

    for (const std::uint64_t number : number_list{1, 2})
    {
        const instance_view view = view_of(run.instances, number);
        REQUIRE(view.state == instance_state::refused);
        REQUIRE(view.problem == make_error_code(plugin_errc::unresponsive));
        REQUIRE(view.reason.empty());
        REQUIRE(!run.instances.running(number));
    }
}

// -- the manifest ------------------------------------------------------------

TEST_CASE("plugin instances: the manifest is checked before anything is sent")
{
    rig run;
    run.instances.want({spec_for(1, extension_kind::generator, "ghost", "{}"),
                        spec_for(2, extension_kind::adapter, "replay", "{}"),
                        replay_spec(0), replay_spec(3)});
    for (const std::uint64_t number : number_list{1, 2, 0})
    {
        const instance_view view = view_of(run.instances, number);
        REQUIRE(view.state == instance_state::refused);
        REQUIRE(view.problem ==
                make_error_code(plugin_errc::capability_missing));
        REQUIRE(view.reason.empty());
    }
    REQUIRE_EQ(run.plugin.count(), std::size_t{0});

    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});
    require_create(run.plugin.at(0), replay_spec(3));
    for (const std::uint64_t number : number_list{1, 2, 0})
    {
        REQUIRE(has_state(run.instances, number, instance_state::refused));
        REQUIRE(!run.instances.running(number));
    }

    // A refusal here is not told to the plugin, and is not asked again.
    run.instances.want({spec_for(1, extension_kind::generator, "ghost", "{}"),
                        spec_for(2, extension_kind::adapter, "replay", "{}"),
                        replay_spec(0), replay_spec(3)});
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});
}

TEST_CASE("plugin instances: no more of a type than the manifest allows")
{
    rig run;
    run.instances.plugin_ready();
    run.instances.want({replay_spec(10), replay_spec(11), replay_spec(12)});

    // Two are created; the third is refused here and never sent.
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    REQUIRE_EQ(run.plugin.count_of(create_op, 12), std::size_t{0});
    const instance_view third = view_of(run.instances, 12);
    REQUIRE(third.state == instance_state::refused);
    REQUIRE(third.problem == make_error_code(plugin_errc::too_many));
    REQUIRE(third.reason.empty());

    run.plugin.answer_ok(run.plugin.pending(create_op, 10));
    run.plugin.answer_ok(run.plugin.pending(create_op, 11));
    REQUIRE(run.instances.running(10));
    REQUIRE(run.instances.running(11));
    REQUIRE(!run.instances.running(12));
    REQUIRE(has_state(run.instances, 12, instance_state::refused));
    REQUIRE_EQ(run.plugin.count_of(create_op, 12), std::size_t{0});

    // One of the first two goes, and the list is given again: the third
    // has room now.
    run.instances.want({replay_spec(11), replay_spec(12)});
    require_stop(run.plugin.at(run.plugin.pending(stop_op, 10)), 10);
    run.plugin.answer_ok(run.plugin.pending(stop_op, 10));
    run.instances.want({replay_spec(11), replay_spec(12)});

    REQUIRE_EQ(run.plugin.count_of(create_op, 12), std::size_t{1});
    const instance_view created = view_of(run.instances, 12);
    REQUIRE(created.state == instance_state::creating);
    REQUIRE(!created.problem);
    REQUIRE(!run.instances.find(10).has_value());

    run.plugin.answer_ok(run.plugin.pending(create_op, 12));
    REQUIRE(run.instances.running(12));
    REQUIRE(run.instances.running(11));
    REQUIRE_EQ(run.plugin.count_of(create_op, 12), std::size_t{1});
}

TEST_CASE("plugin instances: a number given with another type is refused")
{
    // The instance runs, and the owner gives its number another type.
    {
        rig run;
        run.instances.want({replay_spec(1)});
        run.instances.plugin_ready();
        run.plugin.answer_ok(run.plugin.pending(create_op, 1));
        REQUIRE_EQ(run.plugin.count(), std::size_t{1});

        run.instances.want({recorder_spec(1)});
        const instance_view view = view_of(run.instances, 1);
        REQUIRE(view.state == instance_state::refused);
        REQUIRE(view.problem ==
                make_error_code(plugin_errc::capability_missing));

        // The original is not created again, and nothing is made of the
        // new type.
        REQUIRE_EQ(run.plugin.count(), std::size_t{1});
        for (std::size_t index = 0; index < run.plugin.count(); ++index)
            REQUIRE_NE(run.plugin.at(index).type, "recorder");
    }

    // The same before the plugin is ready: the refusal is not lost.
    {
        rig run;
        run.instances.want({replay_spec(1, R"({"a":1})")});
        run.instances.want({recorder_spec(1, R"({"a":2})")});
        const instance_view view = view_of(run.instances, 1);
        REQUIRE(view.state == instance_state::refused);
        REQUIRE(view.problem ==
                make_error_code(plugin_errc::capability_missing));

        run.instances.plugin_ready();
        REQUIRE_EQ(run.plugin.count(), std::size_t{0});
        REQUIRE(has_state(run.instances, 1, instance_state::refused));
    }
}

TEST_CASE("plugin instances: a number that comes twice counts the first time")
{
    rig run;
    run.instances.want({replay_spec(1, R"({"a":1})"),
                        recorder_spec(1, R"({"a":2})"), modbus_spec(2),
                        replay_spec(1, R"({"a":3})")});

    const std::vector<instance_view> views = run.instances.views();
    REQUIRE_EQ(views.size(), std::size_t{2});
    REQUIRE_EQ(views[0].spec.instance, std::uint64_t{1});
    REQUIRE_EQ(views[0].spec.type, "replay");
    REQUIRE_EQ(views[0].spec.params, R"({"a":1})");
    REQUIRE_EQ(views[1].spec.instance, std::uint64_t{2});

    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});
    require_create(run.plugin.at(0), replay_spec(1, R"({"a":1})"));
    require_create(run.plugin.at(1), modbus_spec(2));
}

// -- the plugin goes ---------------------------------------------------------

TEST_CASE("plugin instances: when the plugin goes every instance waits and "
          "is created again")
{
    rig run;
    run.instances.want({replay_spec(1), replay_spec(2), recorder_spec(3)});
    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});

    // One runs, one is refused, one is still being created.
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    run.plugin.answer_error(run.plugin.pending(create_op, 3), "not now");
    REQUIRE(has_state(run.instances, 3, instance_state::refused));
    const std::size_t old_request = run.plugin.pending(create_op, 2);

    run.instances.plugin_gone();
    for (const std::uint64_t number : number_list{1, 2, 3})
    {
        const instance_view view = view_of(run.instances, number);
        REQUIRE(view.state == instance_state::waiting);
        REQUIRE(!view.problem);
        REQUIRE(view.reason.empty());
        REQUIRE(!run.instances.running(number));
    }
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});

    // The answer of the plugin that is gone changes nothing.
    run.plugin.answer_ok(old_request);
    for (const std::uint64_t number : number_list{1, 2, 3})
    {
        REQUIRE(has_state(run.instances, number, instance_state::waiting));
        REQUIRE(!run.instances.running(number));
    }
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});

    // The plugin that starts again has none: every instance is created,
    // the one that was refused too.
    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{6});
    require_create(run.plugin.at(3), replay_spec(1));
    require_create(run.plugin.at(4), replay_spec(2));
    require_create(run.plugin.at(5), recorder_spec(3));
    for (const std::uint64_t number : number_list{1, 2, 3})
    {
        REQUIRE(has_state(run.instances, number, instance_state::creating));
        run.plugin.answer_ok(run.plugin.pending(create_op, number));
        REQUIRE(run.instances.running(number));
    }
}

TEST_CASE("plugin instances: an instance removed while the plugin is not "
          "ready is forgotten")
{
    rig run;
    run.instances.want({replay_spec(1), replay_spec(2)});
    run.instances.want({replay_spec(2)});
    REQUIRE(!run.instances.find(1).has_value());
    REQUIRE_EQ(run.instances.views().size(), std::size_t{1});

    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});
    require_create(run.plugin.at(0), replay_spec(2));
    run.plugin.answer_ok(run.plugin.pending(create_op, 2));

    // The same after the plugin has gone: it has nothing to stop.
    run.instances.plugin_gone();
    run.instances.want({});
    REQUIRE_EQ(run.instances.views().size(), std::size_t{0});
    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{1});
    REQUIRE(!run.instances.running(2));
}

TEST_CASE("plugin instances: an instance removed while its create is in "
          "flight is stopped only when it was created")
{
    rig run;
    run.instances.want({replay_spec(1), replay_spec(2)});
    run.instances.plugin_ready();
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});

    run.instances.want({});
    REQUIRE_EQ(run.instances.views().size(), std::size_t{0});
    REQUIRE(!run.instances.running(1));
    REQUIRE(!run.instances.running(2));
    // Nothing is said while the plugin is making up its mind.
    REQUIRE_EQ(run.plugin.count(), std::size_t{2});

    // The plugin made the first: it is stopped.
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    require_stop(run.plugin.at(2), 1);
    REQUIRE(!run.instances.running(1));
    REQUIRE(!run.instances.find(1).has_value());

    // The plugin refused the second: there is nothing to stop.
    run.plugin.answer_error(run.plugin.pending(create_op, 2), "no");
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    REQUIRE_EQ(run.plugin.count_of(stop_op, 2), std::size_t{0});
    REQUIRE(!run.instances.find(2).has_value());

    run.plugin.answer_ok(run.plugin.pending(stop_op, 1));
    REQUIRE_EQ(run.plugin.count(), std::size_t{3});
    REQUIRE_EQ(run.instances.views().size(), std::size_t{0});
}

// -- the change handler and the end of the object ---------------------------

TEST_CASE("plugin instances: the change handler hears of every kind of "
          "change")
{
    std::size_t changes = 0;
    rig run;
    run.instances.on_change([&changes]() { ++changes; });

    std::size_t before = changes;
    run.instances.want({replay_spec(1)});
    REQUIRE(changes > before);

    before = changes;
    run.instances.plugin_ready();
    REQUIRE(changes > before);

    before = changes;
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));
    REQUIRE(changes > before);

    run.instances.want({replay_spec(1, R"({"a":2})")});
    before = changes;
    run.plugin.answer_error(run.plugin.pending(change_op, 1), "no");
    REQUIRE(changes > before);

    before = changes;
    run.instances.plugin_gone();
    REQUIRE(changes > before);
}

TEST_CASE("plugin instances: answering after the object is gone is harmless")
{
    std::size_t changes = 0;
    fake_plugin plugin;
    {
        plugin_instances instances(declared_points(), plugin.sender());
        instances.on_change([&changes]() { ++changes; });
        instances.want({replay_spec(1), recorder_spec(2), modbus_spec(3)});
        instances.plugin_ready();
        REQUIRE_EQ(plugin.count(), std::size_t{3});
    }
    const std::size_t at_the_end = changes;

    plugin.answer_ok(plugin.pending(create_op, 1));
    plugin.answer_error(plugin.pending(create_op, 2), "no");
    plugin.answer_failure(plugin.pending(create_op, 3), api_errc::timeout);

    REQUIRE_EQ(changes, at_the_end);
    REQUIRE_EQ(plugin.count(), std::size_t{3});
}

TEST_CASE("plugin instances: an instance wanted again while its stop is in "
          "flight is kept")
{
    rig run;
    run.instances.want({replay_spec(1)});
    run.instances.plugin_ready();
    run.plugin.answer_ok(run.plugin.pending(create_op, 1));

    run.instances.want({});
    require_stop(run.plugin.at(run.plugin.pending(stop_op, 1)), 1);
    REQUIRE(!run.instances.find(1).has_value());

    // The owner changes its mind before the plugin has answered.
    run.instances.want({replay_spec(1)});
    REQUIRE(run.instances.find(1).has_value());
    run.plugin.answer_ok(run.plugin.pending(stop_op, 1));

    // It is wanted, so it is in the list and is created again.
    REQUIRE(run.instances.find(1).has_value());
    REQUIRE_EQ(run.instances.views().size(), std::size_t{1});
    REQUIRE_EQ(run.plugin.count_of(create_op, 1), std::size_t{2});
    REQUIRE(has_state(run.instances, 1, instance_state::creating));
}

// -- a property test --------------------------------------------------------

namespace
{

/// A small generator of numbers that is the same on every platform.
class generator
{
public:
    explicit generator(std::uint64_t seed)
        : state_((seed * std::uint64_t{0x9E3779B97F4A7C15}) | 1)
    {
    }

    std::uint64_t next()
    {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * std::uint64_t{0x2545F4914F6CDD1D};
    }

    /// A number from 0 to `limit` - 1.
    std::size_t below(std::size_t limit)
    {
        return static_cast<std::size_t>((next() >> 33) %
                                        static_cast<std::uint64_t>(limit));
    }

private:
    std::uint64_t state_ = 1;
};

/// The highest instance number a random list may name.
constexpr std::uint64_t highest_number = 6;

std::vector<instance_spec> random_list(generator& random)
{
    constexpr std::array<std::string_view, 3> params{"{}", R"({"a":1})",
                                                     R"({"a":2})"};
    std::vector<instance_spec> list;
    const std::size_t length = random.below(7);
    for (std::size_t index = 0; index < length; ++index)
    {
        // Number 0, a type the manifest does not have and a kind that does
        // not match the type are in the mix on purpose.
        const std::uint64_t number = random.below(highest_number + 1);
        const std::string_view text = params[random.below(params.size())];
        switch (random.below(5))
        {
        case 0:
            list.push_back(replay_spec(number, text));
            break;
        case 1:
            list.push_back(recorder_spec(number, text));
            break;
        case 2:
            list.push_back(modbus_spec(number, text));
            break;
        case 3:
            list.push_back(
                spec_for(number, extension_kind::generator, "ghost", text));
            break;
        default:
            list.push_back(
                spec_for(number, extension_kind::adapter, "replay", text));
            break;
        }
    }
    return list;
}

void answer_random(fake_plugin& plugin, generator& random)
{
    const std::vector<std::size_t> waiting = plugin.unanswered();
    if (waiting.empty())
        return;
    const std::size_t index = waiting[random.below(waiting.size())];
    const std::size_t outcome = random.below(5);
    if (outcome < 3)
        plugin.answer_ok(index);
    else if (outcome == 3)
        plugin.answer_error(index, "refused by the test");
    else if (random.below(2) == 0)
        plugin.answer_failure(index, api_errc::plugin_unavailable);
    else
        plugin.answer_failure(index, api_errc::timeout);
}

bool known_state(instance_state state) noexcept
{
    switch (state)
    {
    case instance_state::waiting:
    case instance_state::creating:
    case instance_state::running:
    case instance_state::refused:
    case instance_state::stopping:
        return true;
    }
    return false;
}

/// What is wrong after a step, or nothing.  The most of a type is counted
/// from the plugin's side: an instance is the plugin's until the stop for
/// it has been sent.
std::string violation_of(const rig& run)
{
    if (run.plugin.unreadable() != 0)
        return "a request could not be read";
    if (run.plugin.sent_while_down() != 0)
        return "a request was sent while the plugin was not ready";
    if (run.plugin.double_sends() != 0 || run.plugin.any_open_twice())
        return "an instance had two requests without an answer";
    for (const instance_view& view : run.instances.views())
    {
        if (!known_state(view.state))
            return "an instance is in no known state";
    }
    for (std::uint64_t number = 0; number <= highest_number; ++number)
    {
        if (run.instances.running(number) &&
            !run.instances.find(number).has_value())
            return "an instance runs and is not in the views";
    }
    for (const extension_point& point : declared_points())
    {
        if (run.plugin.alive_of_type(point.id) > point.max_instances)
            return "the plugin has more instances of a type than allowed";
    }
    return {};
}

} // namespace

TEST_CASE("plugin instances: random lists and answers keep the rules")
{
    constexpr std::uint64_t seeds = 100;
    constexpr std::size_t steps = 300;

    for (std::uint64_t seed = 1; seed <= seeds; ++seed)
    {
        generator random(seed);
        rig run;
        run.plugin.down();
        bool ready = false;

        for (std::size_t step = 0; step < steps; ++step)
        {
            const std::size_t choice = random.below(100);
            if (choice < 25)
            {
                run.instances.want(random_list(random));
            }
            else if (choice < 35)
            {
                if (!ready)
                {
                    run.plugin.up();
                    run.instances.plugin_ready();
                    ready = true;
                }
            }
            else if (choice < 43)
            {
                if (ready)
                {
                    run.plugin.down();
                    run.instances.plugin_gone();
                    ready = false;
                }
            }
            else
            {
                answer_random(run.plugin, random);
            }

            const std::string problem = violation_of(run);
            if (!problem.empty())
            {
                std::string where = "seed ";
                where += std::to_string(seed);
                where += ", step ";
                where += std::to_string(step);
                where += ": ";
                where += problem;
                REQUIRE_EQ(where, std::string());
            }
        }
    }
}
