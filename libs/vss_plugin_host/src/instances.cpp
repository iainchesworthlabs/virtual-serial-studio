//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/instances.hpp>

#include <vss/core/error.hpp>
#include <vss/plugin/api_instance.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin::host
{

namespace
{

/// The longest reason of a plugin that is kept.
constexpr std::size_t longest_reason = 400;

/// The number a request is written with.  The session gives every request
/// its real number when it is sent; a message cannot be written with none.
constexpr std::uint64_t placeholder_id = 1;

/// Parameters as a message carries them: the same JSON with no spaces
/// between its parts.  A message is only written when it reads back as it
/// was given, so parameters that were typed with spaces would never be
/// sent.  Text that is not JSON is returned as it is, and the message is
/// then refused where it is written.
std::string as_sent(const std::string& params)
{
    boost::system::error_code failed;
    const boost::json::value parsed = boost::json::parse(params, failed);
    if (failed || !parsed.is_object())
        return params;
    return boost::json::serialize(parsed);
}

/// One instance, wanted or on its way out.
struct entry
{
    /// What the owner wants.  `kind` and `type` are those the instance
    /// was first given with.
    instance_spec spec;
    instance_state state = instance_state::waiting;
    error_code problem;
    std::string reason;
    /// The plugin has it.
    bool created = false;
    /// The parameters the plugin has, when it has the instance.
    std::string sent_params;
    /// Parameters the plugin refused; they are not sent a second time.
    std::optional<std::string> refused_params;
    /// A request for this instance waits for its answer.
    bool in_flight = false;
    /// The owner no longer wants it; it goes once the plugin has let go.
    bool unwanted = false;
    /// The owner gave the number again as another kind or type.
    bool mismatched = false;
    /// Refused here, against the manifest; nothing is sent to make it.
    bool never = false;
};

/// The reason a plugin gave, cut to a length that is kept, at the end of
/// a character.
std::string reason_of(const api_message& answer)
{
    const auto* const failed = std::get_if<error_message>(&answer.body);
    if (failed == nullptr)
        return {};
    std::string text = failed->message;
    if (text.size() > longest_reason)
    {
        std::size_t end = longest_reason;
        // Not in the middle of a UTF-8 character.
        while (end > 0 &&
               (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
            --end;
        text.resize(end);
    }
    return text;
}

bool is_error(const api_message& answer) noexcept
{
    return std::holds_alternative<error_message>(answer.body);
}

} // namespace

std::string_view to_string(instance_state value) noexcept
{
    switch (value)
    {
    case instance_state::waiting:
        return "waiting";
    case instance_state::creating:
        return "creating";
    case instance_state::running:
        return "running";
    case instance_state::refused:
        return "refused";
    case instance_state::stopping:
        return "stopping";
    }
    return "unknown";
}

struct plugin_instances::state : std::enable_shared_from_this<state>
{
    std::vector<extension_point> declared;
    sender send;
    change_handler changed;
    bool ready = false;
    /// Goes up when the plugin goes, so that an answer that comes late is
    /// known to belong to a plugin that is no longer there.
    std::uint64_t epoch = 0;
    /// The instances by number, and their order: the wanted ones in the
    /// order of the owner's list, then those on their way out.
    std::map<std::uint64_t, entry> entries;
    std::vector<std::uint64_t> order;

    void tell()
    {
        if (changed)
            changed();
    }

    const extension_point* point_of(const instance_spec& spec) const
    {
        const auto found = std::ranges::find_if(
            declared, [&](const extension_point& point)
            { return point.kind == spec.kind && point.id == spec.type; });
        return found == declared.end() ? nullptr : &*found;
    }

    bool same_type(const entry& left, const instance_spec& right) const
    {
        return left.spec.kind == right.kind && left.spec.type == right.type;
    }

    /// How many wanted instances of a type come before `number` in the
    /// list.
    std::size_t earlier_of_type(const instance_spec& spec,
                                std::uint64_t number) const
    {
        std::size_t count = 0;
        for (const std::uint64_t other : order)
        {
            if (other == number)
                break;
            const entry& there = entries.at(other);
            if (!there.unwanted && !there.mismatched && same_type(there, spec))
                ++count;
        }
        return count;
    }

    /// How many instances of a type the plugin holds now, or is being
    /// asked to make, other than `number`.
    std::size_t held_of_type(const instance_spec& spec,
                             std::uint64_t number) const
    {
        std::size_t count = 0;
        for (const auto& [other, there] : entries)
        {
            if (other == number || !same_type(there, spec))
                continue;
            const bool making =
                there.in_flight && there.state == instance_state::creating;
            if (there.created || making)
                ++count;
        }
        return count;
    }

    void refuse_here(entry& item, plugin_errc why)
    {
        item.never = true;
        item.state = instance_state::refused;
        item.problem = make_error_code(why);
        item.reason.clear();
    }

    /// Checks a wanted instance against the manifest.
    void judge(entry& item)
    {
        item.never = false;
        const extension_point* const point = point_of(item.spec);
        if (item.mismatched || item.spec.instance == 0 || point == nullptr)
        {
            refuse_here(item, plugin_errc::capability_missing);
            return;
        }
        if (earlier_of_type(item.spec, item.spec.instance) >=
            point->max_instances)
        {
            refuse_here(item, plugin_errc::too_many);
            return;
        }
        if (item.state == instance_state::refused && !item.created &&
            !item.refused_params.has_value())
        {
            // It was refused here before and no longer is.
            item.state = instance_state::waiting;
            item.problem = error_code{};
        }
    }

    /// Sends the next message an instance needs, when it needs one and
    /// none is on its way.
    void drive(std::uint64_t number)
    {
        const auto found = entries.find(number);
        if (found == entries.end())
            return;
        entry& item = found->second;
        if (item.in_flight)
            return;
        if (!ready)
        {
            if (item.unwanted)
            {
                forget(number);
                return;
            }
            if (!item.never)
            {
                item.state = instance_state::waiting;
                item.problem = error_code{};
                item.reason.clear();
            }
            return;
        }
        // The plugin lets go of what is no longer wanted, and of what the
        // owner gave again as another kind of thing.
        if ((item.unwanted || item.mismatched) && item.created)
        {
            if (item.unwanted)
                item.state = instance_state::stopping;
            ask(number, instance_stop_message::op(),
                encode_as(instance_stop_message{number}, placeholder_id));
            return;
        }
        if (item.unwanted)
        {
            forget(number);
            return;
        }
        if (item.never)
            return;
        if (!item.created)
        {
            if (item.refused_params == item.spec.params)
                return;
            // The plugin never holds more of a type than its manifest
            // allows: an instance that takes the place of one on its way
            // out waits until that one has gone.
            const extension_point* const point = point_of(item.spec);
            if (point == nullptr ||
                held_of_type(item.spec, number) >= point->max_instances)
            {
                item.state = instance_state::waiting;
                return;
            }
            item.state = instance_state::creating;
            item.problem = error_code{};
            item.reason.clear();
            instance_create_message create;
            create.instance = number;
            create.kind = item.spec.kind;
            create.type = item.spec.type;
            create.params = as_sent(item.spec.params);
            ask(number, instance_create_message::op(),
                encode_as(create, placeholder_id));
            return;
        }
        if (item.sent_params != item.spec.params &&
            item.refused_params != item.spec.params)
        {
            params_changed_message change;
            change.instance = number;
            change.params = as_sent(item.spec.params);
            ask(number, params_changed_message::op(),
                encode_as(change, placeholder_id));
        }
    }

    /// Every instance again: one that went may have made room for
    /// another.
    void drive_all()
    {
        const std::vector<std::uint64_t> numbers = order;
        for (const std::uint64_t number : numbers)
            drive(number);
    }

    /// Sends a request for an instance and takes its answer.
    void ask(std::uint64_t number, std::string_view op,
             result<api_message> message)
    {
        entry& item = entries.at(number);
        const std::string params = item.spec.params;
        if (!message)
        {
            // The message cannot be written.  Nothing was sent, so this is
            // the answer.
            took_answer(number, std::string(op), params, message.error(),
                        api_message{});
            return;
        }
        item.in_flight = true;
        send(std::move(*message),
             [weak = weak_from_this(), number, op = std::string(op), params,
              sent_in = epoch](error_code failed, const api_message& answer)
             {
                 const auto self = weak.lock();
                 if (!self || sent_in != self->epoch)
                     return;
                 self->took_answer(number, op, params, failed, answer);
                 self->drive_all();
                 self->tell();
             });
    }

    void took_answer(std::uint64_t number, const std::string& op,
                     const std::string& params, error_code failed,
                     const api_message& answer)
    {
        const auto found = entries.find(number);
        if (found == entries.end())
            return;
        entry& item = found->second;
        item.in_flight = false;
        const bool refused = failed || is_error(answer);
        const error_code why = make_error_code(
            failed ? plugin_errc::unresponsive : plugin_errc::misbehaving);

        if (op == instance_stop_message::op())
        {
            // Stopped, or the plugin would not say so: either way the
            // host no longer counts on it.
            item.created = false;
            item.sent_params.clear();
            if (item.unwanted)
                forget(number);
            return;
        }
        if (op == instance_create_message::op())
        {
            if (refused)
            {
                item.state = instance_state::refused;
                item.problem = why;
                item.reason = reason_of(answer);
                item.refused_params = params;
                return;
            }
            item.created = true;
            item.sent_params = params;
            item.refused_params.reset();
            item.state = instance_state::running;
            item.problem = error_code{};
            item.reason.clear();
            return;
        }
        // "params.changed": refused parameters leave the instance running
        // with the ones it had.
        if (refused)
        {
            item.problem = why;
            item.reason = reason_of(answer);
            item.refused_params = params;
            return;
        }
        item.sent_params = params;
        item.refused_params.reset();
        item.problem = error_code{};
        item.reason.clear();
    }

    void forget(std::uint64_t number)
    {
        entries.erase(number);
        std::erase(order, number);
    }
};

plugin_instances::plugin_instances(std::vector<extension_point> declared,
                                   sender send)
    : state_(std::make_shared<state>())
{
    state_->declared = std::move(declared);
    state_->send = std::move(send);
}

plugin_instances::~plugin_instances() = default;

void plugin_instances::on_change(change_handler handler)
{
    state_->changed = std::move(handler);
}

void plugin_instances::want(std::vector<instance_spec> wanted)
{
    state& self = *state_;
    std::vector<std::uint64_t> order;
    for (instance_spec& spec : wanted)
    {
        const std::uint64_t number = spec.instance;
        if (std::ranges::find(order, number) != order.end())
            continue;
        order.push_back(number);
        const auto found = self.entries.find(number);
        if (found == self.entries.end())
        {
            entry item;
            item.spec = std::move(spec);
            self.entries.emplace(number, std::move(item));
            continue;
        }
        entry& item = found->second;
        item.unwanted = false;
        // The same number as another kind of thing is not the same
        // instance: the owner must use a new number.  The instance is
        // refused, and the plugin lets go of the one it has.
        item.mismatched = !self.same_type(item, spec);
        if (!item.mismatched)
            item.spec.params = std::move(spec.params);
    }
    // What is no longer in the list goes, after the plugin has let go.
    for (auto& [number, item] : self.entries)
    {
        if (std::ranges::find(order, number) == order.end())
        {
            item.unwanted = true;
            order.push_back(number);
        }
    }
    self.order = std::move(order);
    for (const std::uint64_t number : self.order)
    {
        entry& item = self.entries.at(number);
        if (!item.unwanted)
            self.judge(item);
    }
    self.drive_all();
    self.tell();
}

void plugin_instances::plugin_ready()
{
    state& self = *state_;
    self.ready = true;
    self.drive_all();
    self.tell();
}

void plugin_instances::plugin_gone()
{
    state& self = *state_;
    self.ready = false;
    ++self.epoch;
    const std::vector<std::uint64_t> numbers = self.order;
    for (const std::uint64_t number : numbers)
    {
        entry& item = self.entries.at(number);
        item.in_flight = false;
        item.created = false;
        item.sent_params.clear();
        // A plugin that starts again is asked again.
        item.refused_params.reset();
        self.drive(number);
    }
    self.tell();
}

std::vector<instance_view> plugin_instances::views() const
{
    std::vector<instance_view> out;
    for (const std::uint64_t number : state_->order)
    {
        const entry& item = state_->entries.at(number);
        if (item.unwanted)
            continue;
        instance_view view;
        view.spec = item.spec;
        view.state = item.state;
        view.problem = item.problem;
        view.reason = item.reason;
        view.params_pending = item.created && !item.mismatched &&
                              item.sent_params != item.spec.params &&
                              item.refused_params != item.spec.params;
        out.push_back(std::move(view));
    }
    return out;
}

std::optional<instance_view>
plugin_instances::find(std::uint64_t instance) const
{
    for (instance_view& view : views())
    {
        if (view.spec.instance == instance)
            return std::move(view);
    }
    return std::nullopt;
}

bool plugin_instances::running(std::uint64_t instance) const noexcept
{
    const auto found = state_->entries.find(instance);
    return found != state_->entries.end() && found->second.created &&
           !found->second.unwanted && !found->second.mismatched;
}

} // namespace vss::plugin::host
