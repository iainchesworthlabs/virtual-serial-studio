//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `control.session.*` (`make_control_point`) on its own.
// A test makes the handler with a hand-made `extension_context`: the link
// keeps what is answered and what is sent, the executor is a strand on a
// thread of the test's own, and the owner's answer to an approval is kept
// until the test gives it, or given at once.  The gateway is the real one
// (`vss_agent` and `vss_gate`, started under an instance name of their own),
// or none at all when a test wants none.  What the relay does with a session
// is in test_system_plugin_control_relay_gateway.cpp; here is what the
// messages of the plugin make of it, and the other way round
// (docs/plugins.md, section 7.7).
//

#include <vss/plugin/host/control_point.hpp>
#include <vss/plugin/host/control_relay.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/protocol.hpp>

#include "control_gateway_world.hpp"
#include "control_relay_rig.hpp"
#include "test_framework.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace control = vss::control;
namespace json = boost::json;
using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::error_message;
using vss::plugin::other_message;
using vss::plugin::ping_message;
using vss::plugin::host::control_approval_request;
using vss::plugin::host::control_approver;
using vss::plugin::host::control_point_options;
using vss::plugin::host::extension_context;
using vss::plugin::host::extension_handler;
using vss::plugin::host::make_control_point;
using vss_test_gateway::frame_of;
using vss_test_gateway::plugin_tokens;
using vss_test_gateway::wait_until_revoked;
using vss_test_gateway::world;
using vss_test_relay::approval_mode;
using vss_test_relay::unused_instance;
using namespace std::chrono_literals;

namespace
{

constexpr std::chrono::milliseconds long_limit = vss_test_relay::long_limit;

/// How long a test waits for something that must not happen.
constexpr std::chrono::milliseconds quiet_time = 300ms;

constexpr std::string_view request_op = "control.session.request";
constexpr std::string_view opened_op = "control.session.opened";
constexpr std::string_view close_op = "control.session.close";
constexpr std::string_view closed_op = "control.session.closed";
constexpr std::string_view frame_op = "control.frame";

/// What the handler said to the plugin: an answer to a request (`answer`,
/// with the number of the request it answers in `re`), or a notification.
struct sent_message
{
    bool answer = false;
    std::uint64_t re = 0;
    api_message message{};
};

/// The handler with everything around it.  The plugin's messages are offered
/// from the test's thread and handled on the strand, as the service does it.
class point_rig
{
public:
    explicit point_rig(std::string instance, bool with_approver = true)
        : guard_(boost::asio::make_work_guard(io_)),
          strand_(boost::asio::make_strand(io_))
    {
        extension_context context;
        context.plugin_id = "com.example.remote";
        context.plugin_name = "Remote control";
        context.plugin_version = "1.0.0";
        context.executor = strand_;
        context.link.answer = [this](std::uint64_t re, api_message message)
        { record(true, re, std::move(message)); };
        context.link.notify = [this](api_message message)
        { record(false, 0, std::move(message)); };
        control_point_options options;
        options.instance = std::move(instance);
        control_approver approve;
        if (with_approver)
        {
            approve = [this](const control_approval_request& ask,
                             const std::function<void(bool)>& done)
            { asked(ask, done); };
        }
        handler_ =
            make_control_point(context, std::move(approve), std::move(options));
        thread_ = std::thread([this] { io_.run(); });
    }

    /// Destroys the handler on the strand, lets what is still to do finish
    /// and stops the thread.
    ~point_rig()
    {
        try
        {
            std::promise<void> destroyed;
            std::future<void> gone = destroyed.get_future();
            boost::asio::post(strand_,
                              [this, &destroyed]
                              {
                                  handler_.reset();
                                  destroyed.set_value();
                              });
            gone.wait();
            guard_.reset();
            thread_.join();
        }
        catch (...)
        {
            // best effort: a destructor must not throw, and nothing can act
            // on a failed stop.
        }
    }

    point_rig(const point_rig&) = delete;
    point_rig& operator=(const point_rig&) = delete;

    // -- the plugin -----------------------------------------------------------

    /// Offers the handler a message, on the strand, and says whether it took
    /// it.
    bool offer(const api_message& message)
    {
        bool taken = false;
        on_strand([&] { taken = handler_->take(message); });
        return taken;
    }

    /// The plugin has gone.
    void plugin_gone()
    {
        on_strand([this] { handler_->plugin_gone(); });
    }

    /// The handler is destroyed now, with the plugin still there.
    void destroy()
    {
        on_strand([this] { handler_.reset(); });
    }

    // -- the owner ------------------------------------------------------------

    void set_mode(approval_mode mode)
    {
        const std::lock_guard lock(mutex_);
        mode_ = mode;
    }

    /// Answers the oldest held approval, from the test's thread.
    void release(bool approved)
    {
        std::function<void(bool)> done;
        {
            const std::lock_guard lock(mutex_);
            REQUIRE(!held_.empty());
            done = std::move(held_.front());
            held_.erase(held_.begin());
        }
        done(approved);
    }

    std::vector<control_approval_request> approvals() const
    {
        const std::lock_guard lock(mutex_);
        return approvals_;
    }

    /// Waits until the owner has been asked `count` times.
    bool wait_approvals(std::size_t count)
    {
        return wait_until([&] { return approvals_.size() >= count; });
    }

    // -- what the handler said ------------------------------------------------

    std::size_t sent_count() const
    {
        const std::lock_guard lock(mutex_);
        return sent_.size();
    }

    std::vector<sent_message> sent() const
    {
        const std::lock_guard lock(mutex_);
        return sent_;
    }

    /// Waits until more than `before` messages have been sent.
    bool wait_more(std::size_t before,
                   std::chrono::milliseconds limit = long_limit)
    {
        return wait_until([&] { return sent_.size() > before; }, limit);
    }

    /// Waits for a message that `match` accepts, and gives it.  `match` runs
    /// with the rig's lock held.
    template <class Predicate>
    std::optional<sent_message>
    wait_sent(Predicate match, std::chrono::milliseconds limit = long_limit)
    {
        std::optional<sent_message> found;
        wait_until(
            [&]
            {
                for (const sent_message& item : sent_)
                {
                    if (match(item))
                    {
                        found = item;
                        return true;
                    }
                }
                return false;
            },
            limit);
        return found;
    }

    /// The answer to the request with this number.
    sent_message wait_answer(std::uint64_t id)
    {
        const auto found = wait_sent([id](const sent_message& item)
                                     { return item.answer && item.re == id; });
        if (!found)
            throw vss_test::failure("no answer to request " +
                                    std::to_string(id));
        return *found;
    }

    /// Runs `work` on the strand and waits for it.
    void on_strand(const std::function<void()>& work)
    {
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        boost::asio::post(strand_,
                          [&work, &finished]
                          {
                              work();
                              finished.set_value();
                          });
        done.get();
    }

private:
    template <class Condition>
    bool wait_until(Condition condition, std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [&] { return condition(); });
    }

    template <class Condition> bool wait_until(Condition condition)
    {
        return wait_until(std::move(condition), long_limit);
    }

    void record(bool answer, std::uint64_t re, api_message message)
    {
        {
            const std::lock_guard lock(mutex_);
            sent_.push_back(sent_message{answer, re, std::move(message)});
        }
        changed_.notify_all();
    }

    void asked(const control_approval_request& ask,
               const std::function<void(bool)>& done)
    {
        approval_mode mode = approval_mode::approve;
        {
            const std::lock_guard lock(mutex_);
            approvals_.push_back(ask);
            mode = mode_;
            if (mode == approval_mode::hold)
                held_.push_back(done);
        }
        changed_.notify_all();
        if (mode == approval_mode::approve)
            done(true);
        else if (mode == approval_mode::deny)
            done(false);
    }

    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    std::unique_ptr<extension_handler> handler_;
    std::thread thread_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    approval_mode mode_ = approval_mode::approve;
    std::vector<std::function<void(bool)>> held_;
    std::vector<control_approval_request> approvals_;
    std::vector<sent_message> sent_;
};

// -- messages from the plugin -----------------------------------------------

/// A message of the plugin as the session hands it over: the header as it
/// is, whatever it holds.
api_message plugin_message(std::string_view op, std::uint64_t id,
                           std::string header, vss::byte_buffer blob = {})
{
    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.body = other_message{std::move(header)};
    message.blob = std::move(blob);
    return message;
}

/// A `control.session.request` with these scopes.
api_message session_request(std::uint64_t id,
                            const std::vector<std::string>& scopes,
                            std::string peer = "lab computer")
{
    json::array list;
    for (const std::string& scope : scopes)
        list.emplace_back(scope);
    json::object header;
    header.emplace("op", request_op);
    header.emplace("id", id);
    header.emplace("scopes", std::move(list));
    header.emplace("peer", peer);
    header.emplace("reason", std::string_view("a test"));
    return plugin_message(request_op, id, json::serialize(header));
}

/// A request whose header is this text, whatever it holds.
api_message request_with(std::string_view members)
{
    std::string header = R"({"op":"control.session.request","id":7)";
    if (!members.empty())
    {
        header.push_back(',');
        header.append(members);
    }
    header.push_back('}');
    return plugin_message(request_op, 7, std::move(header));
}

/// A `control.frame` of the plugin: the gateway's header as text, which is
/// put in as the member `frame`.
api_message frame_from_plugin(std::uint64_t session,
                              const std::string& frame_text,
                              vss::byte_buffer blob = {})
{
    json::object header;
    header.emplace("op", frame_op);
    header.emplace("session", session);
    header.emplace("frame", json::parse(frame_text));
    return plugin_message(frame_op, 0, json::serialize(header),
                          std::move(blob));
}

/// A `control.frame` whose member `frame` is this JSON.
api_message frame_with(std::uint64_t session, std::string_view frame_json)
{
    json::object header;
    header.emplace("op", frame_op);
    header.emplace("session", session);
    header.emplace("frame", json::parse(frame_json));
    return plugin_message(frame_op, 0, json::serialize(header));
}

/// A `control.session.close` of the plugin.
api_message close_from_plugin(std::uint64_t session)
{
    json::object header;
    header.emplace("op", close_op);
    header.emplace("session", session);
    return plugin_message(close_op, 0, json::serialize(header));
}

/// A request for a status of the gateway, as a header of its protocol.
std::string status_frame(std::uint64_t id)
{
    return frame_of(id, control::status_request{}).header;
}

// -- what the handler says --------------------------------------------------

json::object header_of(const sent_message& sent)
{
    const auto* const other = std::get_if<other_message>(&sent.message.body);
    if (other == nullptr)
        throw vss_test::failure("a message that is not an other_message: " +
                                sent.message.op);
    json::value parsed = json::parse(other->header);
    if (!parsed.is_object())
        throw vss_test::failure("a header that is not an object");
    return std::move(parsed.get_object());
}

std::uint64_t number_at(const json::object& header, std::string_view key)
{
    const json::value* const found = header.if_contains(key);
    if (found != nullptr && found->is_uint64())
        return found->get_uint64();
    if (found != nullptr && found->is_int64() && found->get_int64() >= 0)
        return static_cast<std::uint64_t>(found->get_int64());
    throw vss_test::failure("no whole number in the member " +
                            std::string(key));
}

std::string text_at(const json::object& header, std::string_view key)
{
    const json::value* const found = header.if_contains(key);
    if (found == nullptr || !found->is_string())
        throw vss_test::failure("no text in the member " + std::string(key));
    const json::string& text = found->get_string();
    return std::string(text.data(), text.size());
}

/// The code an answer carries: `ok`, or the name of the `api_errc` of an
/// error.
std::string code_of(const sent_message& sent)
{
    if (const auto* const error =
            std::get_if<error_message>(&sent.message.body))
    {
        REQUIRE_EQ(error->category, std::string("vss.plugin.api"));
        return error->value;
    }
    return sent.message.op;
}

std::string code_name(api_errc code)
{
    return std::string(vss::plugin::to_string(code));
}

/// Whether the message is a `control.session.closed` for this session.
bool is_closed_of(const sent_message& sent, std::uint64_t session)
{
    return !sent.answer && sent.message.op == closed_op &&
           number_at(header_of(sent), "session") == session;
}

/// Whether the message is a `control.frame` for this session.
bool is_frame_of(const sent_message& sent, std::uint64_t session)
{
    return !sent.answer && sent.message.op == frame_op &&
           number_at(header_of(sent), "session") == session;
}

/// Waits for the `control.session.closed` of a session and gives its header.
json::object wait_closed(point_rig& rig, std::uint64_t session)
{
    const auto found = rig.wait_sent([session](const sent_message& item)
                                     { return is_closed_of(item, session); });
    if (!found)
        throw vss_test::failure("the session " + std::to_string(session) +
                                " was not said to be closed");
    return header_of(*found);
}

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// Asks for a session and gives its number; the owner says yes.
std::uint64_t open_session(point_rig& rig, std::uint64_t id,
                           const std::vector<std::string>& scopes,
                           std::string peer = "lab computer")
{
    REQUIRE(rig.offer(session_request(id, scopes, std::move(peer))));
    const sent_message answer = rig.wait_answer(id);
    if (answer.message.op != opened_op)
        throw vss_test::failure("the session was not opened: " +
                                code_of(answer));
    const std::uint64_t session = number_at(header_of(answer), "session");
    REQUIRE(session != 0);
    return session;
}

/// Says that no message came after the first `before`.
void expect_quiet(point_rig& rig, std::size_t before)
{
    REQUIRE(!rig.wait_more(before, quiet_time));
    REQUIRE_EQ(rig.sent_count(), before);
}

} // namespace

TEST_CASE("plugin control point: with no approver a request is not_granted and "
          "no gateway is asked")
{
    world gateway;
    point_rig rig(gateway.instance(), false);

    REQUIRE(rig.offer(session_request(7, {"monitor"})));
    const sent_message answer = rig.wait_answer(7);
    REQUIRE_EQ(code_of(answer), code_name(api_errc::not_granted));
    REQUIRE_EQ(rig.sent_count(), std::size_t{1});

    // The gateway has not been spoken to: no token was made.
    expect_quiet(rig, 1);
    REQUIRE(plugin_tokens(gateway).empty());
}

TEST_CASE("plugin control point: a request that is not as it must be is "
          "bad_request")
{
    point_rig rig(unused_instance());

    std::string many = R"("scopes":[)";
    for (int count = 0; count < 16; ++count)
        many.append(R"("monitor",)");
    many.append(R"("monitor"])");

    struct bad
    {
        std::string what;
        api_message message;
    };
    std::vector<bad> cases;
    const auto add = [&cases](std::string what, api_message message)
    { cases.push_back(bad{std::move(what), std::move(message)}); };
    const std::string too_long_peer(
        vss::plugin::host::max_control_peer_size + 1, 'p');
    const std::string too_long_reason(
        vss::plugin::host::max_control_reason_size + 1, 'r');
    add("no scopes", request_with(""));
    add("scopes is not a list", request_with(R"("scopes":"monitor")"));
    add("an empty list", request_with(R"("scopes":[])"));
    add("17 scopes", request_with(many));
    add("a scope that is not text", request_with(R"("scopes":[5])"));
    add("an empty scope name", request_with(R"("scopes":[""])"));
    add("a peer of 65 bytes", request_with(R"("scopes":["monitor"],"peer":")" +
                                           too_long_peer + R"(")"));
    add("a reason of 257 bytes",
        request_with(R"("scopes":["monitor"],"reason":")" + too_long_reason +
                     R"(")"));
    add("a peer that is not text",
        request_with(R"("scopes":["monitor"],"peer":5)"));
    add("a header that is not an object",
        plugin_message(request_op, 7, "[1,2]"));
    api_message not_other;
    not_other.op = std::string(request_op);
    not_other.id = 7;
    not_other.body = ping_message{};
    add("a body that is not an other_message", std::move(not_other));

    std::size_t answered = 0;
    for (const bad& item : cases)
    {
        REQUIRE(rig.offer(item.message));
        ++answered;
        REQUIRE_EQ(rig.sent_count(), answered);
        const sent_message answer = rig.sent().back();
        if (!answer.answer || answer.re != 7 ||
            code_of(answer) != code_name(api_errc::bad_request))
        {
            throw vss_test::failure("not a bad_request for " + item.what +
                                    ": " + code_of(answer));
        }
    }
    // Nobody was asked.
    REQUIRE(rig.approvals().empty());
}

TEST_CASE("plugin control point: a text of the longest size is not bad_request")
{
    point_rig rig(unused_instance());
    rig.set_mode(approval_mode::deny);

    const std::string peer(vss::plugin::host::max_control_peer_size, 'p');
    const std::string reason(vss::plugin::host::max_control_reason_size, 'r');
    std::string fields = R"("scopes":["monitor"],"peer":")";
    fields += peer;
    fields += R"(","reason":")";
    fields += reason;
    fields += '"';
    REQUIRE(rig.offer(request_with(fields)));
    REQUIRE_EQ(code_of(rig.wait_answer(7)), code_name(api_errc::not_granted));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
}

TEST_CASE("plugin control point: another op is not taken")
{
    point_rig rig(unused_instance());
    REQUIRE(!rig.offer(plugin_message("control.other", 7,
                                      R"({"op":"control.other","id":7})")));
    REQUIRE_EQ(rig.sent_count(), std::size_t{0});
}

TEST_CASE("plugin control point: a request that wants no answer gets nothing")
{
    world gateway;
    point_rig rig(gateway.instance());

    // Taken, and neither answered nor asked about: a request with id 0 is a
    // notification, and a session could not be told to its sender.
    REQUIRE(rig.offer(session_request(0, {"monitor"})));
    expect_quiet(rig, 0);
    REQUIRE(rig.approvals().empty());
    REQUIRE(plugin_tokens(gateway).empty());
}

TEST_CASE("plugin control point: when the owner says no the answer is "
          "not_granted")
{
    point_rig rig(unused_instance());
    rig.set_mode(approval_mode::deny);

    REQUIRE(rig.offer(session_request(7, {"monitor", "control.ports"},
                                      "  lab\a  computer ")));
    const sent_message answer = rig.wait_answer(7);
    REQUIRE(answer.answer);
    REQUIRE_EQ(code_of(answer), code_name(api_errc::not_granted));

    // The owner saw who the plugin is, the peer as it was cleaned and the
    // scopes that were asked for.
    const auto asked = rig.approvals();
    REQUIRE_EQ(asked.size(), std::size_t{1});
    REQUIRE_EQ(asked[0].plugin_id, std::string("com.example.remote"));
    REQUIRE_EQ(asked[0].plugin_name, std::string("Remote control"));
    REQUIRE_EQ(asked[0].peer, std::string("lab computer"));
    REQUIRE_EQ(asked[0].reason, std::string("a test"));
    REQUIRE_EQ(asked[0].scopes.size(), std::size_t{2});
    REQUIRE_EQ(asked[0].scopes[0], std::string("monitor"));
    REQUIRE_EQ(asked[0].scopes[1], std::string("control.ports"));
    expect_quiet(rig, 1);
}

TEST_CASE("plugin control point: admin is not_granted without asking the owner")
{
    world gateway;
    point_rig rig(gateway.instance());

    REQUIRE(rig.offer(session_request(7, {"monitor", "admin"})));
    REQUIRE_EQ(code_of(rig.wait_answer(7)), code_name(api_errc::not_granted));
    REQUIRE(rig.approvals().empty());
    REQUIRE(plugin_tokens(gateway).empty());
}

TEST_CASE("plugin control point: with no gateway running the answer is "
          "plugin_unavailable after the approval")
{
    point_rig rig(unused_instance());

    REQUIRE(rig.offer(session_request(7, {"monitor"})));
    const sent_message answer = rig.wait_answer(7);
    REQUIRE_EQ(code_of(answer), code_name(api_errc::plugin_unavailable));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
}

TEST_CASE("plugin control point: an approved request opens a session")
{
    world gateway;
    point_rig rig(gateway.instance());

    REQUIRE(rig.offer(session_request(7, {"monitor", "control.ports"})));
    const sent_message answer = rig.wait_answer(7);
    REQUIRE(answer.answer);
    REQUIRE_EQ(answer.message.op, std::string(opened_op));
    REQUIRE_EQ(answer.message.re, std::uint64_t{7});

    const json::object header = header_of(answer);
    REQUIRE_EQ(text_at(header, "op"), std::string(opened_op));
    REQUIRE_EQ(number_at(header, "re"), std::uint64_t{7});
    REQUIRE(number_at(header, "session") != 0);
    const json::value* const scopes = header.if_contains("scopes");
    REQUIRE(scopes != nullptr && scopes->is_array());
    REQUIRE_EQ(scopes->get_array().size(), std::size_t{2});
    REQUIRE(scopes->get_array()[0].is_string());
    REQUIRE_EQ(std::string(scopes->get_array()[0].get_string().c_str()),
               std::string("monitor"));
    REQUIRE_EQ(std::string(scopes->get_array()[1].get_string().c_str()),
               std::string("control.ports"));
    REQUIRE(number_at(header, "expires_ms") > now_ms());

    // The owner was asked once, and one token was made.
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{1});
}

TEST_CASE("plugin control point: a frame of the plugin is answered by the "
          "gateway in a frame")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t session = open_session(rig, 7, {"monitor"});

    REQUIRE(rig.offer(frame_from_plugin(session, status_frame(41))));
    const auto found = rig.wait_sent([session](const sent_message& item)
                                     { return is_frame_of(item, session); });
    REQUIRE(found.has_value());
    // A notification: nothing in it answers a request.
    REQUIRE(!found->answer);
    REQUIRE_EQ(found->message.id, std::uint64_t{0});
    REQUIRE_EQ(found->message.op, std::string(frame_op));
    const json::object header = header_of(*found);
    REQUIRE_EQ(text_at(header, "op"), std::string(frame_op));
    const json::value* const inner = header.if_contains("frame");
    REQUIRE(inner != nullptr && inner->is_object());
    REQUIRE_EQ(number_at(inner->get_object(), "id"), std::uint64_t{41});
    REQUIRE_EQ(text_at(inner->get_object(), "reply"), std::string("status"));
}

TEST_CASE("plugin control point: a frame that says hello ends the session")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t session = open_session(rig, 7, {"monitor"});

    REQUIRE(rig.offer(
        frame_with(session, R"({"id":2,"op":"hello","api":1,"client":"x"})")));
    const json::object header = wait_closed(rig, session);
    REQUIRE_EQ(text_at(header, "op"), std::string(closed_op));
    REQUIRE_EQ(text_at(header, "code"), std::string("refused_frame"));
    REQUIRE(!text_at(header, "reason").empty());

    // It was said once, as a notification.
    const sent_message last = rig.sent().back();
    REQUIRE(!last.answer);
    REQUIRE_EQ(last.message.op, std::string(closed_op));
}

TEST_CASE("plugin control point: a frame that is not an object ends the "
          "session")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t first = open_session(rig, 7, {"monitor"}, "lab one");
    const std::uint64_t second = open_session(rig, 8, {"monitor"}, "lab two");
    REQUIRE(first != second);

    REQUIRE(rig.offer(frame_with(first, "5")));
    const json::object closed = wait_closed(rig, first);
    REQUIRE_EQ(text_at(closed, "code"), std::string("host_stopped"));

    // A frame that is not there at all is the same.
    json::object header;
    header.emplace("op", frame_op);
    header.emplace("session", second);
    REQUIRE(
        rig.offer(plugin_message(frame_op, 0, json::serialize(header), {})));
    REQUIRE_EQ(text_at(wait_closed(rig, second), "code"),
               std::string("host_stopped"));
}

TEST_CASE("plugin control point: a frame for a session that is not open is "
          "ignored")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t session = open_session(rig, 7, {"monitor"});
    const std::size_t before = rig.sent_count();

    // A number that was never given, a number that is not one, and the number
    // before and after the one that is open.
    REQUIRE(rig.offer(frame_from_plugin(session + 100, status_frame(1))));
    REQUIRE(rig.offer(frame_from_plugin(session + 1, status_frame(2))));
    REQUIRE(rig.offer(frame_from_plugin(0, status_frame(3))));
    REQUIRE(rig.offer(plugin_message(frame_op, 0,
                                     R"({"op":"control.frame","session":"1",)"
                                     R"("frame":{"id":4,"op":"status"}})")));
    REQUIRE(rig.offer(plugin_message(frame_op, 0,
                                     R"({"op":"control.frame",)"
                                     R"("frame":{"id":5,"op":"status"}})")));
    expect_quiet(rig, before);

    // The session that is open goes on.
    REQUIRE(rig.offer(frame_from_plugin(session, status_frame(6))));
    const auto found = rig.wait_sent([session](const sent_message& item)
                                     { return is_frame_of(item, session); });
    REQUIRE(found.has_value());
    const json::object header = header_of(*found);
    REQUIRE_EQ(number_at(header.at("frame").as_object(), "id"),
               std::uint64_t{6});
}

TEST_CASE("plugin control point: a close ends the session, and a second close "
          "is ignored")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t session = open_session(rig, 7, {"monitor"});

    REQUIRE(rig.offer(close_from_plugin(session)));
    const json::object header = wait_closed(rig, session);
    REQUIRE_EQ(text_at(header, "op"), std::string(closed_op));
    REQUIRE_EQ(text_at(header, "code"), std::string("host_stopped"));
    REQUIRE(header.if_contains("reason") != nullptr);
    const std::size_t before = rig.sent_count();

    REQUIRE(rig.offer(close_from_plugin(session)));
    expect_quiet(rig, before);
    // A frame for it is dropped, too.
    REQUIRE(rig.offer(frame_from_plugin(session, status_frame(8))));
    expect_quiet(rig, before);
}

TEST_CASE("plugin control point: when the plugin goes no session is said to be "
          "closed and a later request works")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t old_session = open_session(rig, 7, {"monitor"});
    REQUIRE_EQ(rig.sent_count(), std::size_t{1});

    rig.plugin_gone();
    // The plugin has gone: there is nobody to tell.
    expect_quiet(rig, 1);
    wait_until_revoked(gateway, 1);
    expect_quiet(rig, 1);

    // The plugin that starts again knows of no session.
    REQUIRE(rig.offer(frame_from_plugin(old_session, status_frame(9))));
    REQUIRE(rig.offer(close_from_plugin(old_session)));
    expect_quiet(rig, 1);

    const std::uint64_t new_session = open_session(rig, 8, {"monitor"});
    REQUIRE(new_session != old_session);
    REQUIRE(rig.offer(frame_from_plugin(new_session, status_frame(10))));
    const auto found =
        rig.wait_sent([new_session](const sent_message& item)
                      { return is_frame_of(item, new_session); });
    REQUIRE(found.has_value());
}

TEST_CASE("plugin control point: an approval that comes after the plugin has "
          "gone is not answered")
{
    world gateway;
    point_rig rig(gateway.instance());
    rig.set_mode(approval_mode::hold);

    REQUIRE(rig.offer(session_request(7, {"monitor"})));
    REQUIRE(rig.wait_approvals(1));
    REQUIRE_EQ(rig.sent_count(), std::size_t{0});

    rig.plugin_gone();
    rig.release(true);
    expect_quiet(rig, 0);
    // And no token was made for a plugin that is not there.
    REQUIRE(plugin_tokens(gateway).empty());
}

TEST_CASE("plugin control point: a handler that is destroyed with a session "
          "open leaves nothing to do for what comes late")
{
    world gateway;
    point_rig rig(gateway.instance());
    const std::uint64_t session = open_session(rig, 7, {"monitor"});
    const std::size_t before = rig.sent_count();

    // A second request waits for the owner while the handler goes.
    rig.set_mode(approval_mode::hold);
    REQUIRE(rig.offer(session_request(8, {"monitor", "control.ports"})));
    REQUIRE(rig.wait_approvals(2));
    rig.destroy();

    // The owner answers after the handler is gone, and the strand has time to
    // run what is left.
    rig.release(true);
    expect_quiet(rig, before);
    REQUIRE(session != 0);

    // The token of the session was taken back, and none was made after.
    wait_until_revoked(gateway, 1);
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{1});
}

TEST_CASE("plugin control point: a second request while the first waits for "
          "the owner is busy")
{
    world gateway;
    point_rig rig(gateway.instance());
    rig.set_mode(approval_mode::hold);

    REQUIRE(rig.offer(session_request(7, {"monitor"})));
    REQUIRE(rig.wait_approvals(1));
    REQUIRE(rig.offer(
        session_request(8, {"monitor", "control.ports"}, "another lab")));
    const sent_message busy = rig.wait_answer(8);
    REQUIRE_EQ(code_of(busy), code_name(api_errc::busy));
    // The owner was not asked a second time, and the first is still open.
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
    REQUIRE_EQ(rig.sent_count(), std::size_t{1});

    rig.release(true);
    const sent_message opened = rig.wait_answer(7);
    REQUIRE_EQ(opened.message.op, std::string(opened_op));
}
