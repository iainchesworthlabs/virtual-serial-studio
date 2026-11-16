//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/control_point.hpp>

#include <vss/core/bytes.hpp>
#include <vss/plugin/host/control_relay.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
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

namespace json = boost::json;

constexpr std::string_view request_op = "control.session.request";
constexpr std::string_view opened_op = "control.session.opened";
constexpr std::string_view close_op = "control.session.close";
constexpr std::string_view closed_op = "control.session.closed";
constexpr std::string_view frame_op = "control.frame";

constexpr std::size_t most_scopes = 16;
constexpr std::size_t longest_scope = 64;

/// The header of a message from the plugin, or nothing when it is not one
/// of the extension points' messages.
std::optional<json::object> header_of(const api_message& message)
{
    const auto* const other = std::get_if<other_message>(&message.body);
    if (other == nullptr)
        return std::nullopt;
    boost::system::error_code failed;
    json::value parsed = json::parse(other->header, failed);
    if (failed || !parsed.is_object())
        return std::nullopt;
    return std::move(parsed.get_object());
}

/// A text member of at most `most` bytes; empty when it is not there.
/// Nothing when it is there and wrong.
std::optional<std::string> text_of(const json::object& header,
                                   std::string_view key, std::size_t most)
{
    const json::value* const found = header.if_contains(key);
    if (found == nullptr)
        return std::string{};
    if (!found->is_string() || found->get_string().size() > most)
        return std::nullopt;
    const json::string& text = found->get_string();
    return std::string(text.data(), text.size());
}

/// The member `session`: a whole number above 0.
std::optional<std::uint64_t> session_of(const json::object& header)
{
    const json::value* const found = header.if_contains("session");
    if (found == nullptr)
        return std::nullopt;
    std::uint64_t number = 0;
    if (found->is_uint64())
    {
        number = found->get_uint64();
    }
    else if (found->is_int64() && found->get_int64() > 0)
    {
        number = static_cast<std::uint64_t>(found->get_int64());
    }
    if (number == 0 || number > max_message_id)
        return std::nullopt;
    return number;
}

/// What the plugin asked for, read strictly.
std::optional<control_session_request> read_request(const api_message& message)
{
    const auto header = header_of(message);
    if (!header)
        return std::nullopt;
    const json::value* const scopes = header->if_contains("scopes");
    if (scopes == nullptr || !scopes->is_array())
        return std::nullopt;
    const json::array& list = scopes->get_array();
    if (list.empty() || list.size() > most_scopes)
        return std::nullopt;
    control_session_request request;
    for (const json::value& item : list)
    {
        if (!item.is_string() || item.get_string().empty() ||
            item.get_string().size() > longest_scope)
            return std::nullopt;
        const json::string& name = item.get_string();
        request.scopes.emplace_back(name.data(), name.size());
    }
    auto peer = text_of(*header, "peer", max_control_peer_size);
    auto reason = text_of(*header, "reason", max_control_reason_size);
    if (!peer || !reason)
        return std::nullopt;
    request.peer = std::move(*peer);
    request.reason = std::move(*reason);
    return request;
}

api_errc code_of(control_refusal why) noexcept
{
    switch (why)
    {
    case control_refusal::invalid_request:
        return api_errc::bad_request;
    case control_refusal::permission_denied:
        return api_errc::not_granted;
    case control_refusal::busy:
        return api_errc::busy;
    case control_refusal::plugin_unavailable:
        break;
    }
    return api_errc::plugin_unavailable;
}

/// A message of this extension point, from the host.
api_message message_of(std::string_view op, std::uint64_t re,
                       json::object members)
{
    json::object full;
    full.emplace("op", op);
    if (re != 0)
        full.emplace("re", re);
    for (auto& item : members)
        full.insert_or_assign(item.key(), std::move(item.value()));
    api_message out;
    out.op = std::string(op);
    out.re = re;
    out.body = other_message{json::serialize(full)};
    return out;
}

class control_point final : public extension_handler
{
public:
    control_point(extension_context context, control_approver approve,
                  control_point_options options)
        : context_(std::move(context)), approve_(std::move(approve)),
          options_(std::move(options))
    {
    }

    // The relay goes with this object, and calls nothing afterwards.
    ~control_point() override = default;

    bool take(const api_message& message) override
    {
        if (message.op == request_op)
        {
            request(message);
            return true;
        }
        if (message.op == frame_op)
        {
            frame(message);
            return true;
        }
        if (message.op == close_op)
        {
            close(message);
            return true;
        }
        return false;
    }

    void plugin_gone() override
    {
        // What comes later belongs to a plugin that has gone.
        ++epoch_;
        open_.clear();
        if (relay_)
            relay_->close_all();
    }

private:
    void refuse(std::uint64_t id, api_errc code, const std::string& text)
    {
        if (id != 0)
            context_.link.answer(id, error_answer(code, text));
    }

    bool is_open(std::uint64_t session) const
    {
        return std::ranges::find(open_, session) != open_.end();
    }

    control_relay& relay()
    {
        if (relay_)
            return *relay_;
        control_relay_options options;
        options.plugin_id = context_.plugin_id;
        options.plugin_name = context_.plugin_name;
        options.instance = options_.instance;
        options.control_instance = options_.control_instance;
        control_relay_handlers handlers;
        if (approve_)
        {
            handlers.approve = [this](const control_approval_request& asked,
                                      std::function<void(bool)> done)
            { approve_(asked, std::move(done)); };
        }
        handlers.to_plugin = [this](std::uint64_t session,
                                    std::string_view header, bytes_view blob)
        { return to_plugin(session, header, blob); };
        handlers.closed = [this](std::uint64_t session, control_close_code code,
                                 std::string_view reason)
        { closed(session, code, reason); };
        relay_ = control_relay::create(context_.executor, std::move(options),
                                       std::move(handlers));
        return *relay_;
    }

    void request(const api_message& message)
    {
        // A request that wants no answer cannot be given a session.
        if (message.id == 0)
            return;
        const auto asked = read_request(message);
        if (!asked)
        {
            refuse(message.id, api_errc::bad_request,
                   "the session was not asked for as it must be");
            return;
        }
        if (!approve_)
        {
            refuse(message.id, api_errc::not_granted,
                   "this program has nobody to ask");
            return;
        }
        relay().request_session(*asked,
                                [this, id = message.id, epoch = epoch_](
                                    const control_session_answer& answer)
                                {
                                    if (epoch != epoch_)
                                        return;
                                    answered(id, answer);
                                });
    }

    void answered(std::uint64_t id, const control_session_answer& answer)
    {
        if (const auto* const refused =
                std::get_if<control_session_refused>(&answer))
        {
            refuse(id, code_of(refused->why),
                   clean_control_text(refused->text, max_control_reason_size));
            return;
        }
        const auto& opened = std::get<control_session_opened>(answer);
        open_.push_back(opened.session);
        json::object members;
        members.emplace("session", opened.session);
        json::array scopes;
        for (const std::string& scope : opened.scopes)
            scopes.emplace_back(scope);
        members.emplace("scopes", std::move(scopes));
        members.emplace("expires_ms", opened.expires_ms);
        context_.link.answer(id, message_of(opened_op, id, std::move(members)));
    }

    void frame(const api_message& message)
    {
        const auto header = header_of(message);
        if (!header)
            return;
        const auto session = session_of(*header);
        if (!session || !is_open(*session) || !relay_)
            return;
        const json::value* const inner = header->if_contains("frame");
        if (inner == nullptr || !inner->is_object())
        {
            // A frame that cannot be one ends the session, as a frame the
            // relay refuses does.
            relay_->close_session(*session);
            return;
        }
        relay_->frame_from_plugin(*session, json::serialize(*inner),
                                  message.blob);
    }

    void close(const api_message& message)
    {
        const auto header = header_of(message);
        if (!header)
            return;
        const auto session = session_of(*header);
        if (!session || !is_open(*session) || !relay_)
            return;
        relay_->close_session(*session);
    }

    /// A frame of the gateway for the plugin.  Always taken: see the
    /// header of this file.
    bool to_plugin(std::uint64_t session, std::string_view header,
                   bytes_view blob)
    {
        if (!is_open(session))
            return true;
        boost::system::error_code failed;
        json::value inner = json::parse(header, failed);
        json::object members;
        members.emplace("session", session);
        const bool inner_was_object = !failed && inner.is_object();
        if (inner_was_object)
            members.emplace("frame", std::move(inner));
        api_message out = message_of(frame_op, 0, std::move(members));
        const std::size_t size =
            std::get<other_message>(out.body).header.size();
        const bool fits =
            size <= max_header_size && size + blob.size() <= most_frame_;
        if (failed || !inner_was_object || !fits)
        {
            // A frame that does not fit in a message to the plugin cannot
            // be passed on, and one that is left out would not be missed
            // by a plugin that counts on every reply.  The session ends.
            if (relay_)
                relay_->close_session(session);
            return true;
        }
        out.blob.assign(blob.begin(), blob.end());
        context_.link.notify(std::move(out));
        return true;
    }

    void closed(std::uint64_t session, control_close_code code,
                std::string_view reason)
    {
        const auto found = std::ranges::find(open_, session);
        if (found == open_.end())
            return;
        open_.erase(found);
        json::object members;
        members.emplace("session", session);
        members.emplace("code", to_string(code));
        members.emplace("reason",
                        clean_control_text(reason, max_control_reason_size));
        context_.link.notify(message_of(closed_op, 0, std::move(members)));
    }

    /// The most a frame to the plugin holds, header and blob together,
    /// with room for the frame's own fields.
    static constexpr std::size_t most_frame_ =
        (std::size_t{1} << 20) - std::size_t{64};

    extension_context context_;
    control_approver approve_;
    control_point_options options_;
    /// The sessions the plugin that runs now knows of.
    std::vector<std::uint64_t> open_;
    /// Goes up when the plugin goes, so that an answer for the plugin
    /// that went never reaches the one that came after it.
    std::uint64_t epoch_ = 0;
    /// Last, so that it goes first: its handlers use what is above.
    std::shared_ptr<control_relay> relay_;
};

} // namespace

std::unique_ptr<extension_handler>
make_control_point(const extension_context& context, control_approver approve,
                   control_point_options options)
{
    return std::make_unique<control_point>(context, std::move(approve),
                                           std::move(options));
}

} // namespace vss::plugin::host
