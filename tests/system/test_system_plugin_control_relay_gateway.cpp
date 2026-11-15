//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::control_relay` against the real thing: `vss_agent` and
// `vss_gate` are started as children under an instance name of their own, a
// relay makes tokens and opens sessions, and the test looks at what the
// gateway says about them through its administration endpoint
// (docs/remote-control.md, sections 3, 4 and 9, and docs/plugins.md, 7.7).
//
// The relay never shows its token, so the tests cannot try one.  What shows
// that a token is gone is the gateway's own record: the token is listed as
// revoked and no session of it is left.
//

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/client.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/plugin/host/control_relay.hpp>

#include "child_process.hpp"
#include "control_relay_rig.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace control = vss::control;
using vss::plugin::host::control_close_code;
using vss::plugin::host::control_refusal;
using vss::plugin::host::control_relay_options;
using vss::plugin::host::control_session_refused;
using vss::plugin::host::control_session_request;
using vss_test_relay::approval_mode;
using vss_test_relay::offered_frame;
using vss_test_relay::relay_rig;
using vss_test_support::child_process;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

constexpr std::chrono::milliseconds long_limit = vss_test_relay::long_limit;

// -- the gateway as processes ------------------------------------------------

/// An instance name no other test, run, agent or gateway uses.
std::string unique_instance()
{
    static const std::string prefix = []
    {
        std::random_device device;
        std::ostringstream text;
        text << "r" << std::hex << std::setw(8) << std::setfill('0')
             << device();
        return text.str();
    }();
    static unsigned counter = 0;
    return prefix + "-" + std::to_string(counter++);
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

/// Reads lines from `child` until one contains `marker`.
void wait_for_line(child_process& child, const std::string& marker,
                   const char* who)
{
    std::string seen;
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto line = child.read_line(long_limit);
        if (!line)
            break;
        seen.append(*line);
        seen.push_back('\n');
        if (contains(*line, marker))
            return;
    }
    throw vss_test::failure(std::string(who) + " did not say \"" + marker +
                            "\"; it said: " + seen);
}

class agent_process
{
public:
    explicit agent_process(std::string name)
        : instance_(std::move(name)),
          child_(VSS_AGENT_PROGRAM, {"--instance=" + instance_})
    {
        wait_for_line(child_, "listening on", "the agent");
    }

    const std::string& instance() const noexcept { return instance_; }

private:
    std::string instance_;
    child_process child_;
};

class gate_process
{
public:
    explicit gate_process(const std::string& instance)
        : audit_dir_(scratch_.path() / "audit"),
          child_(VSS_GATE_PROGRAM,
                 {"--instance=" + instance, "--agent-instance=" + instance,
                  "--audit-dir=" + audit_dir_.string()})
    {
        wait_for_line(child_, "administration on", "the gateway");
    }

    child_process& child() noexcept { return child_; }

private:
    scratch_directory scratch_;
    std::filesystem::path audit_dir_;
    child_process child_;
};

/// An agent and a gateway for one test, and what a test needs to look at the
/// gateway.
class world
{
public:
    world() : agent_(unique_instance()), gate_(agent_.instance()) {}

    const std::string& instance() const noexcept { return agent_.instance(); }
    child_process& gate_child() noexcept { return gate_.child(); }

    /// The options of a relay for the plugin that the tests use.
    control_relay_options options() const
    {
        control_relay_options out;
        out.plugin_id = "com.example.remote";
        out.plugin_name = "Remote control";
        out.instance = instance();
        return out;
    }

    /// A connection to the administration endpoint, as `vss_cli` has.  The
    /// gateway has room for two there, and the relay uses one at times, so
    /// a connection that is refused for room is tried again.
    std::unique_ptr<control::client> admin() const
    {
        control::client_options options;
        options.instance = instance();
        options.name = "relay-test";
        options.admin = true;
        const auto deadline = std::chrono::steady_clock::now() + long_limit;
        for (;;)
        {
            auto connected = control::client::connect(options);
            if (connected)
                return std::move(*connected);
            if (connected.error() != control::api_errc::busy ||
                std::chrono::steady_clock::now() >= deadline)
            {
                throw vss_test::failure("cannot connect to the "
                                        "administration endpoint: " +
                                        connected.error().message());
            }
            std::this_thread::sleep_for(20ms);
        }
    }

private:
    agent_process agent_;
    gate_process gate_;
};

/// The tokens that relays made, as the gateway lists them.
std::vector<control::token_entry> plugin_tokens(const world& where)
{
    const auto admin = where.admin();
    auto listed = admin->list_tokens();
    if (!listed)
        throw vss_test::failure("tokens.list failed");
    std::vector<control::token_entry> out;
    for (control::token_entry& entry : listed->tokens)
    {
        if (entry.label.rfind("plugin:", 0) == 0)
            out.push_back(std::move(entry));
    }
    return out;
}

/// The sessions of tokens, as the gateway lists them.
std::vector<control::session_entry> token_sessions(const world& where)
{
    const auto admin = where.admin();
    auto listed = admin->list_sessions();
    if (!listed)
        throw vss_test::failure("sessions.list failed");
    std::vector<control::session_entry> out;
    for (control::session_entry& entry : listed->sessions)
    {
        if (entry.principal.rfind("token:", 0) == 0)
            out.push_back(std::move(entry));
    }
    return out;
}

/// Waits until every token of a relay is revoked, and none has a session.
void wait_until_revoked(const world& where, std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    for (;;)
    {
        const auto tokens = plugin_tokens(where);
        const bool all =
            tokens.size() == count &&
            std::ranges::all_of(tokens, [](const control::token_entry& entry)
                                { return entry.revoked; });
        if (all && token_sessions(where).empty())
            return;
        if (std::chrono::steady_clock::now() >= deadline)
            throw vss_test::failure("the tokens were not all revoked");
        std::this_thread::sleep_for(20ms);
    }
}

// -- frames ------------------------------------------------------------------

struct plugin_frame
{
    std::string header{};
    vss::byte_buffer blob{};
};

/// A request as a plugin would send it: the gateway's own header.
plugin_frame frame_of(std::uint64_t id, control::request_body body)
{
    control::request asked;
    asked.id = id;
    asked.body = std::move(body);
    control::frame encoded = control::encode(asked);
    return {std::move(encoded.header), std::move(encoded.blob)};
}

void send_request(relay_rig& rig, std::uint64_t session, std::uint64_t id,
                  control::request_body body)
{
    plugin_frame sending = frame_of(id, std::move(body));
    rig.send(session, std::move(sending.header), std::move(sending.blob));
}

/// The reply with this id that the relay handed to the plugin.
control::reply wait_reply(relay_rig& rig, std::uint64_t id)
{
    const auto found = rig.wait_frame(
        [id](const offered_frame& item)
        {
            const auto decoded = control::decode_server_message(
                control::frame{item.header, item.blob});
            if (!decoded)
                return false;
            const auto* answer = std::get_if<control::reply>(&*decoded);
            return answer != nullptr && answer->id == id;
        });
    if (!found)
        throw vss_test::failure("no reply with id " + std::to_string(id));
    auto decoded = control::decode_server_message(
        control::frame{found->header, found->blob});
    if (!decoded)
        throw vss_test::failure("a reply that cannot be read");
    return std::get<control::reply>(std::move(*decoded));
}

template <class Body> Body reply_as(const control::reply& answer)
{
    const auto* body = std::get_if<Body>(&answer.body);
    if (body == nullptr)
        throw vss_test::failure("the reply is not what was asked for");
    return *body;
}

const control_session_refused&
refusal_of(const vss::plugin::host::control_session_answer& answer)
{
    const auto* refused = std::get_if<control_session_refused>(&answer);
    if (refused == nullptr)
        throw vss_test::failure("the request was not refused");
    return *refused;
}

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

/// True for text that holds a token or the secret part of one: the start of
/// the format, or a run of base32 characters as long as a secret.
bool holds_token(const std::string& text)
{
    if (contains(text, std::string(control::token_prefix)))
        return true;
    std::size_t run = 0;
    for (const char letter : text)
    {
        const bool base32 = (letter >= 'a' && letter <= 'z') ||
                            (letter >= '2' && letter <= '7');
        run = base32 ? run + 1 : 0;
        if (run >= control::token_secret_size)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("plugin control relay: an approved request opens a session with the "
          "approved scopes")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened = rig.open({"monitor", "control.ports"}, "lab computer");

    REQUIRE(opened.session != 0);
    REQUIRE_EQ(opened.scopes.size(), std::size_t{2});
    REQUIRE_EQ(opened.scopes[0], std::string("monitor"));
    REQUIRE_EQ(opened.scopes[1], std::string("control.ports"));
    // The token lives 24 hours.
    const std::uint64_t day = std::uint64_t{24} * 60 * 60 * 1000;
    REQUIRE(opened.expires_ms > now_ms() + day - 120000);
    REQUIRE(opened.expires_ms < now_ms() + day + 120000);

    const auto asked = rig.approvals();
    REQUIRE_EQ(asked.size(), std::size_t{1});
    REQUIRE_EQ(asked[0].peer, std::string("lab computer"));
    REQUIRE_EQ(asked[0].scopes.size(), std::size_t{2});
    REQUIRE(asked[0].token_life == std::chrono::hours{24});

    // What the gateway made: one token with those scopes for the local
    // endpoint alone, and one session of it.
    const auto tokens = plugin_tokens(gateway);
    REQUIRE_EQ(tokens.size(), std::size_t{1});
    REQUIRE_EQ(tokens[0].label,
               std::string("plugin:com.example.remote lab computer"));
    REQUIRE_EQ(tokens[0].scopes.size(), std::size_t{2});
    REQUIRE_EQ(tokens[0].scopes[0], std::string("monitor"));
    REQUIRE_EQ(tokens[0].scopes[1], std::string("control.ports"));
    REQUIRE_EQ(tokens[0].transports.size(), std::size_t{1});
    REQUIRE_EQ(tokens[0].transports[0], std::string("local"));
    REQUIRE(!tokens[0].revoked);
    REQUIRE_EQ(tokens[0].expires_ms, opened.expires_ms);

    const auto sessions = token_sessions(gateway);
    REQUIRE_EQ(sessions.size(), std::size_t{1});
    REQUIRE_EQ(sessions[0].principal, "token:" + tokens[0].id);
    REQUIRE_EQ(sessions[0].client, std::string("plugin:com.example.remote"));
    REQUIRE_EQ(sessions[0].transport, std::string("local"));
    REQUIRE_EQ(rig.session_count(), std::size_t{1});
}

TEST_CASE("plugin control relay: a relayed request gets its reply with the "
          "same id")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened = rig.open({"monitor"});

    send_request(rig, opened.session, 7, control::status_request{});
    const auto status = reply_as<control::status_reply>(wait_reply(rig, 7));
    REQUIRE(status.agent_running);
    // This session and the administration session of the test are not
    // counted the same, but the plugin's is.
    REQUIRE(status.sessions >= 1);

    send_request(rig, opened.session, 8, control::ports_list_request{});
    const auto ports = reply_as<control::ports_reply>(wait_reply(rig, 8));
    REQUIRE(ports.ports.empty());

    // The gateway's refusal comes back as it is: a monitor session may not
    // make a port.
    send_request(rig, opened.session, 9,
                 control::ports_create_request{"alpha", ""});
    const auto refused = wait_reply(rig, 9);
    const auto* error = std::get_if<control::error_reply>(&refused.body);
    REQUIRE(error != nullptr);
    REQUIRE_EQ(error->error.code, std::string("forbidden"));
    // And the session goes on.
    REQUIRE_EQ(rig.session_count(), std::size_t{1});
}

TEST_CASE("plugin control relay: an event stream arrives with its bytes in the "
          "blob")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened =
        rig.open({"monitor", "monitor.data", "control.ports", "control.data"});
    const std::uint64_t session = opened.session;

    send_request(rig, session, 1, control::ports_create_request{"alpha", ""});
    const std::string alpha =
        reply_as<control::created_reply>(wait_reply(rig, 1)).id;
    send_request(rig, session, 2, control::ports_create_request{"beta", ""});
    const std::string beta =
        reply_as<control::created_reply>(wait_reply(rig, 2)).id;
    send_request(rig, session, 3,
                 control::links_connect_request{alpha, beta, true});
    reply_as<control::created_reply>(wait_reply(rig, 3));

    control::traffic_subscribe_request subscribing;
    subscribing.options.interval_ms = 20;
    subscribing.options.ports = {beta};
    send_request(rig, session, 4, subscribing);
    reply_as<control::subscribed_reply>(wait_reply(rig, 4));

    control::ports_send_request writing;
    writing.port = alpha;
    writing.data = {0x00, 0xff, 'h', 'i', 0x0d, 0x0a};
    send_request(rig, session, 5, writing);
    reply_as<control::done_reply>(wait_reply(rig, 5));

    const auto arrived = rig.wait_frame(
        [&](const offered_frame& item)
        {
            if (item.blob.empty())
                return false;
            const auto decoded = control::decode_server_message(
                control::frame{item.header, item.blob});
            const auto* batch =
                decoded ? std::get_if<control::event_batch>(&*decoded)
                        : nullptr;
            if (batch == nullptr)
                return false;
            return std::ranges::any_of(batch->events,
                                       [&](const control::event_entry& event)
                                       {
                                           return event.kind == "data" &&
                                                  event.port == beta &&
                                                  event.data == writing.data;
                                       });
        });
    if (!arrived)
        throw vss_test::failure("no batch with the bytes arrived");
    // The bytes are in the blob and not in the header.
    REQUIRE(!contains(arrived->header, "hi"));
    // A loopback port hears the bytes and gives them back, so the batch may
    // hold them twice.
    REQUIRE(arrived->blob.size() >= writing.data.size());
    REQUIRE(!std::ranges::search(arrived->blob, writing.data).empty());
    REQUIRE(arrived->session == session);
}

TEST_CASE("plugin control relay: a frame that says hello is refused and the "
          "session closed")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto first = rig.open({"monitor"});
    rig.send(first.session,
             "{\"id\":2,\"op\":\"hello\",\"api\":1,\"client\":\"x\"}");
    const std::string reason = rig.wait_closed(first.session);
    REQUIRE(contains(reason, "hello"));
    REQUIRE(rig.wait_code(first.session) == control_close_code::refused_frame);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    wait_until_revoked(gateway, 1);

    // The same word written with an escape is the same word.
    const auto second = rig.open({"monitor"});
    rig.send(second.session, "{\"id\":2,\"op\":\"\\u0068ello\"}");
    REQUIRE(contains(rig.wait_closed(second.session), "hello"));
    wait_until_revoked(gateway, 2);

    // The session is gone for good: a later frame is dropped, and nothing is
    // said about it.
    rig.send(first.session, "{\"id\":3,\"op\":\"ping\"}");
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    REQUIRE(!rig.wait_closed_count(first.session, 2, 200ms));
}

TEST_CASE("plugin control relay: a frame the gateway could not read closes the "
          "session")
{
    world gateway;
    relay_rig rig(gateway.options());

    struct bad_frame
    {
        std::string header{};
        vss::byte_buffer blob{};
    };
    std::vector<bad_frame> bad;
    bad.push_back({"not json", {}});
    bad.push_back({"[1,2]", {}});
    bad.push_back({"\"text\"", {}});
    bad.push_back({"", {}});
    // Nested deeper than the gateway reads.
    bad.push_back(
        {"{\"id\":1,\"op\":\"ping\",\"x\":[[[[[[[[[[1]]]]]]]]]]}", {}});
    // A frame longer than a frame may be.
    bad.push_back({"{\"id\":1,\"op\":\"ping\"}",
                   vss::byte_buffer(vss::agent::max_frame_size, 0)});

    std::size_t made = 0;
    for (const bad_frame& item : bad)
    {
        const auto opened = rig.open({"monitor"});
        ++made;
        rig.send(opened.session, item.header, item.blob);
        const std::string reason = rig.wait_closed(opened.session);
        REQUIRE(!reason.empty());
        REQUIRE(rig.wait_code(opened.session) ==
                control_close_code::refused_frame);
        REQUIRE_EQ(rig.session_count(), std::size_t{0});
    }
    wait_until_revoked(gateway, made);
}

TEST_CASE("plugin control relay: admin is refused before anyone is asked")
{
    world gateway;
    relay_rig rig(gateway.options());
    control_session_request asking;
    asking.scopes = {"monitor", "admin"};
    asking.peer = "lab computer";
    asking.reason = "a test";
    const auto answer = rig.request(std::move(asking));
    REQUIRE(refusal_of(answer).why == control_refusal::permission_denied);
    REQUIRE(rig.approvals().empty());
    REQUIRE(plugin_tokens(gateway).empty());
}

TEST_CASE("plugin control relay: a refused approval makes no token")
{
    world gateway;
    vss_test_relay::fake_clock clock;
    control_relay_options options = gateway.options();
    options.clock = clock.function();
    relay_rig rig(options);
    rig.set_mode(approval_mode::deny);
    control_session_request asking;
    asking.scopes = {"monitor"};
    asking.peer = "lab computer";
    asking.reason = "a test";
    const auto answer = rig.request(std::move(asking));
    REQUIRE(refusal_of(answer).why == control_refusal::permission_denied);
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
    REQUIRE(plugin_tokens(gateway).empty());
    REQUIRE(token_sessions(gateway).empty());
    clock.advance(std::chrono::seconds{61});

    // Nothing is made while the owner has not answered either.
    rig.set_mode(approval_mode::hold);
    control_session_request waiting;
    waiting.scopes = {"monitor"};
    waiting.peer = "lab computer";
    waiting.reason = "a test";
    const auto pending = rig.request_async(std::move(waiting));
    REQUIRE(rig.wait_approvals(2));
    REQUIRE(plugin_tokens(gateway).empty());
    rig.release(false);
    REQUIRE(pending.wait_for(long_limit) == std::future_status::ready);
    REQUIRE(plugin_tokens(gateway).empty());
}

TEST_CASE("plugin control relay: closing everything revokes every token")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto first = rig.open({"monitor"}, "first");
    const auto second = rig.open({"monitor", "monitor.data"}, "second");
    REQUIRE(first.session != second.session);
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{2});
    REQUIRE_EQ(token_sessions(gateway).size(), std::size_t{2});

    rig.close_all();
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    // `close_all` waited for the gateway, so it is all in the record now.
    const auto tokens = plugin_tokens(gateway);
    REQUIRE_EQ(tokens.size(), std::size_t{2});
    for (const control::token_entry& entry : tokens)
    {
        REQUIRE(entry.revoked);
    }
    REQUIRE(token_sessions(gateway).empty());

    // The host closed both, and says so.
    REQUIRE(rig.wait_code(first.session) == control_close_code::host_stopped);
    REQUIRE(rig.wait_code(second.session) == control_close_code::host_stopped);
    REQUIRE_EQ(rig.closed().size(), std::size_t{2});

    // The relay is used again after that, with a token of its own.
    const auto third = rig.open({"monitor"}, "third");
    REQUIRE(third.session != first.session);
    REQUIRE(third.session != second.session);
    send_request(rig, third.session, 1, control::ping_request{});
    reply_as<control::done_reply>(wait_reply(rig, 1));
}

TEST_CASE("plugin control relay: closing one session revokes its token")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto keep = rig.open({"monitor"}, "keep");
    const auto drop = rig.open({"monitor"}, "drop");
    rig.close_session(drop.session);
    // The gateway has the one token revoked and the other in use.
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    for (;;)
    {
        const auto tokens = plugin_tokens(gateway);
        const auto revoked =
            std::ranges::count_if(tokens, [](const control::token_entry& entry)
                                  { return entry.revoked; });
        if (revoked == 1 && token_sessions(gateway).size() == 1)
            break;
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(20ms);
    }
    for (const control::token_entry& entry : plugin_tokens(gateway))
    {
        REQUIRE_EQ(entry.revoked,
                   entry.label == "plugin:com.example.remote drop");
    }
    // The one that is left works.
    send_request(rig, keep.session, 1, control::ping_request{});
    reply_as<control::done_reply>(wait_reply(rig, 1));
    // The host closed it and said so; a frame for it is then dropped.
    REQUIRE(rig.wait_code(drop.session) == control_close_code::host_stopped);
    rig.send(drop.session, "{\"id\":2,\"op\":\"ping\"}");
    REQUIRE_EQ(rig.session_count(), std::size_t{1});
    REQUIRE(!rig.wait_closed_count(drop.session, 2, 200ms));
}

TEST_CASE("plugin control relay: a token revoked at the gateway ends the "
          "session")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened = rig.open({"monitor"});
    const auto tokens = plugin_tokens(gateway);
    REQUIRE_EQ(tokens.size(), std::size_t{1});

    {
        const auto admin = gateway.admin();
        const auto revoked = admin->revoke_token(tokens[0].id);
        REQUIRE(revoked.has_value());
    }

    const std::string reason = rig.wait_closed(opened.session);
    REQUIRE_EQ(reason,
               std::string("The control gateway closed the connection."));
    // The gateway ended the connection without a word, long before the
    // token's time: that is not `revoked`, which is the gateway saying
    // `unauthenticated`.
    REQUIRE(rig.wait_code(opened.session) ==
            control_close_code::gateway_closed);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    wait_until_revoked(gateway, 1);
}

TEST_CASE("plugin control relay: a token that has ended is told to the plugin "
          "and closes the session")
{
    world gateway;
    control_relay_options options = gateway.options();
    options.token_life = std::chrono::seconds{1};
    relay_rig rig(options);
    const auto opened = rig.open({"monitor"});

    // The token ends one second after it was made; the next request of the
    // session finds it so.
    while (now_ms() < opened.expires_ms + 300)
        std::this_thread::sleep_for(50ms);
    send_request(rig, opened.session, 3, control::ping_request{});

    const auto answer = wait_reply(rig, 3);
    const auto* error = std::get_if<control::error_reply>(&answer.body);
    REQUIRE(error != nullptr);
    REQUIRE_EQ(error->error.code, std::string("unauthenticated"));
    const std::string reason = rig.wait_closed(opened.session);
    REQUIRE(contains(reason, "token has ended"));
    REQUIRE(rig.wait_code(opened.session) == control_close_code::expired);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
}

TEST_CASE("plugin control relay: a gateway that stops ends the sessions and "
          "the next request finds none")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened = rig.open({"monitor"});
    {
        const auto admin = gateway.admin();
        REQUIRE(admin->stop().has_value());
    }
    REQUIRE(gateway.gate_child().wait(long_limit).has_value());
    REQUIRE_EQ(rig.wait_closed(opened.session),
               std::string("The control gateway closed the connection."));
    REQUIRE(rig.wait_code(opened.session) ==
            control_close_code::gateway_closed);

    // Revoking has nobody to ask; it does not take long and does not hang.
    rig.close_all();

    control_session_request asking;
    asking.scopes = {"monitor"};
    asking.peer = "lab computer";
    asking.reason = "a test";
    const auto answer = rig.request(std::move(asking));
    REQUIRE(refusal_of(answer).why == control_refusal::plugin_unavailable);
    REQUIRE_EQ(refusal_of(answer).text,
               std::string("No control gateway is running."));
}

TEST_CASE("plugin control relay: a slow plugin holds the connection and loses "
          "nothing")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened = rig.open({"monitor"});

    // The plugin takes no frame for now.
    rig.refuse_offers(1);
    send_request(rig, opened.session, 2, control::ping_request{});
    REQUIRE(
        rig.wait_frame([](const offered_frame& item) { return !item.taken; })
            .has_value());
    send_request(rig, opened.session, 3, control::ping_request{});
    // The second reply is not offered while the first is held.
    std::this_thread::sleep_for(300ms);
    REQUIRE_EQ(rig.frames().size(), std::size_t{1});

    // Room again: the held frame is offered once more and the next follows,
    // in order.
    rig.has_room(opened.session);
    reply_as<control::done_reply>(wait_reply(rig, 3));
    const auto frames = rig.frames();
    REQUIRE_EQ(frames.size(), std::size_t{3});
    REQUIRE(!frames[0].taken);
    REQUIRE(frames[1].taken);
    REQUIRE(frames[2].taken);
    REQUIRE_EQ(frames[0].header, frames[1].header);
    REQUIRE_NE(frames[1].header, frames[2].header);
}

TEST_CASE("plugin control relay: frames that wait too long to be written close "
          "the session")
{
    world gateway;
    control_relay_options options = gateway.options();
    options.max_waiting_bytes = 1024;
    relay_rig rig(options);
    const auto opened = rig.open({"monitor"});

    rig.send(opened.session, "{\"id\":1,\"op\":\"ping\"}",
             vss::byte_buffer(4096, 0));
    const std::string reason = rig.wait_closed(opened.session);
    REQUIRE(contains(reason, "as fast as"));
    REQUIRE(rig.wait_code(opened.session) == control_close_code::refused_frame);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    wait_until_revoked(gateway, 1);
}

TEST_CASE("plugin control relay: a plugin has four sessions at most")
{
    world gateway;
    relay_rig rig(gateway.options());
    std::vector<std::uint64_t> sessions;
    for (int count = 0; count < 4; ++count)
    {
        std::string peer = "peer ";
        peer.append(std::to_string(count));
        sessions.push_back(rig.open({"monitor"}, std::move(peer)).session);
    }

    control_session_request asking;
    asking.scopes = {"monitor"};
    asking.peer = "one too many";
    asking.reason = "a test";
    const auto answer = rig.request(std::move(asking));
    REQUIRE(refusal_of(answer).why == control_refusal::busy);
    // The fifth was not even asked about.
    REQUIRE_EQ(rig.approvals().size(), std::size_t{4});
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{4});

    // When one is closed there is room for another.
    rig.close_session(sessions[0]);
    const auto again = rig.open({"monitor"}, "after a close");
    REQUIRE(again.session != 0);
}

TEST_CASE("plugin control relay: the token is in no text the relay gives out")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto first =
        rig.open({"monitor", "monitor.data", "control.ports", "control.data",
                  "control.faults", "control.apply", "record"},
                 "lab computer");
    send_request(rig, first.session, 1, control::status_request{});
    wait_reply(rig, 1);
    send_request(rig, first.session, 2, control::ports_list_request{});
    wait_reply(rig, 2);
    // A failed session, an ended token and a refused frame say things too.
    const auto second = rig.open({"monitor"}, "second");
    rig.send(second.session, "{\"id\":1,\"op\":\"hello\"}");
    rig.wait_closed(second.session);
    rig.set_mode(approval_mode::deny);
    control_session_request denied;
    denied.scopes = {"monitor"};
    denied.peer = "denied";
    denied.reason = "a test";
    rig.request(std::move(denied));
    rig.close_session(first.session);
    rig.close_all();

    const std::string said = rig.everything_said();
    REQUIRE(!said.empty());
    REQUIRE(!holds_token(said));
    // The log does say what was done, with the token's public id.
    const auto lines = rig.lines();
    REQUIRE(!lines.empty());
    REQUIRE(contains(said, "revoked"));
}

// -- what the relay may spend
// ---------------------------------------------------

namespace
{

control_session_request asking_for(std::vector<std::string> scopes,
                                   std::string peer)
{
    control_session_request asking;
    asking.scopes = std::move(scopes);
    asking.peer = std::move(peer);
    asking.reason = "a test";
    return asking;
}

} // namespace

TEST_CASE("plugin control relay: a computer that has a session gets no second "
          "token")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto first = rig.open({"monitor", "monitor.data"}, "lab computer");

    // The same peer and the same scopes, in another order.
    const auto again =
        rig.request(asking_for({"monitor.data", "monitor"}, "lab computer"));
    REQUIRE(refusal_of(again).why == control_refusal::busy);
    REQUIRE_EQ(refusal_of(again).text,
               std::string("A session for this computer is already open."));
    // No one was asked, and no token was made.
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{1});

    // Another peer, or other scopes, are another session.
    rig.open({"monitor", "monitor.data"}, "other computer");
    rig.open({"monitor"}, "lab computer");
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{3});

    // When the session is closed the computer may have another.
    rig.close_session(first.session);
    const auto reopened = rig.open({"monitor", "monitor.data"}, "lab computer");
    REQUIRE(reopened.session != first.session);
}

TEST_CASE("plugin control relay: at most so many tokens are made in a day")
{
    world gateway;
    vss_test_relay::fake_clock clock;
    control_relay_options options = gateway.options();
    options.clock = clock.function();
    options.max_tokens_per_day = 2;
    relay_rig rig(options);

    const auto first = rig.open({"monitor"}, "a");
    const auto second = rig.open({"monitor"}, "b");
    // Closing a session gives the token back, but the gateway keeps its
    // record, so the count stays.
    rig.close_session(first.session);
    rig.close_session(second.session);

    const auto over = rig.request(asking_for({"monitor"}, "c"));
    REQUIRE(refusal_of(over).why == control_refusal::busy);
    REQUIRE(contains(refusal_of(over).text, "24 hours"));
    // Refused before anyone was asked, and nothing was made.
    REQUIRE_EQ(rig.approvals().size(), std::size_t{2});
    REQUIRE_EQ(plugin_tokens(gateway).size(), std::size_t{2});

    clock.advance(std::chrono::hours{24});
    rig.open({"monitor"}, "c");
    REQUIRE_EQ(rig.approvals().size(), std::size_t{3});
}

TEST_CASE("plugin control relay: the label of a token always fits the "
          "gateway's limit")
{
    world gateway;
    control_relay_options options = gateway.options();
    options.plugin_id = std::string(200, 'a');
    relay_rig rig(options);
    // The longest peer there is: 32 letters of two bytes each.
    std::string peer;
    for (int count = 0; count < 32; ++count)
        peer.append("\xc3\xa9");
    rig.open({"monitor"}, peer);
    const auto tokens = plugin_tokens(gateway);
    REQUIRE_EQ(tokens.size(), std::size_t{1});
    REQUIRE_EQ(tokens[0].label.size(), std::size_t{128});
    REQUIRE_EQ(tokens[0].label.substr(0, 7), std::string("plugin:"));
    REQUIRE_EQ(tokens[0].label.substr(tokens[0].label.size() - peer.size()),
               peer);
}

// -- the relay going away
// --------------------------------------------------------

TEST_CASE("plugin control relay: the relay may be destroyed inside a handler")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened = rig.open({"monitor"});
    rig.destroy_in_closed();
    rig.send(opened.session, "{\"id\":2,\"op\":\"hello\"}");
    REQUIRE(rig.wait_code(opened.session) == control_close_code::refused_frame);
    // The token is revoked without the relay.
    wait_until_revoked(gateway, 1);
}

TEST_CASE("plugin control relay: closing everything while a token is being "
          "made revokes it")
{
    world gateway;
    relay_rig rig(gateway.options());
    rig.set_mode(approval_mode::hold);
    const auto waiting = rig.request_async(asking_for({"monitor"}, "lab"));
    REQUIRE(rig.wait_approvals(1));
    // The approval is on the strand before the close, so the gateway is
    // asked for the token first.
    rig.release(true);
    rig.close_all();
    REQUIRE(waiting.wait_for(long_limit) == std::future_status::ready);
    REQUIRE(refusal_of(waiting.get()).why ==
            control_refusal::plugin_unavailable);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    wait_until_revoked(gateway, 1);
}

// -- what a plugin cannot do through the relay
// -------------------------------------

TEST_CASE("plugin control relay: administration is forbidden on any session")
{
    world gateway;
    relay_rig rig(gateway.options());
    const auto opened =
        rig.open({"monitor", "monitor.data", "control.ports", "control.data",
                  "control.faults", "control.apply", "record"});
    std::vector<control::request_body> bodies;
    bodies.emplace_back(
        control::tokens_create_request{"x", {"monitor"}, 0, {}});
    bodies.emplace_back(control::tokens_list_request{});
    bodies.emplace_back(control::sessions_kick_request{1});
    bodies.emplace_back(control::endpoint_stop_request{});
    bodies.emplace_back(control::sessions_list_request{});
    bodies.emplace_back(control::tokens_revoke_request{"aaaaaaaaaaaaa"});
    std::uint64_t id = 10;
    for (const control::request_body& body : bodies)
    {
        send_request(rig, opened.session, id, body);
        const auto answer = wait_reply(rig, id);
        const auto* error = std::get_if<control::error_reply>(&answer.body);
        REQUIRE(error != nullptr);
        REQUIRE_EQ(error->error.code, std::string("forbidden"));
        ++id;
    }
    // Nothing happened: the gateway runs, the session works, and no token
    // was made or revoked.
    send_request(rig, opened.session, id, control::ping_request{});
    reply_as<control::done_reply>(wait_reply(rig, id));
    const auto tokens = plugin_tokens(gateway);
    REQUIRE_EQ(tokens.size(), std::size_t{1});
    REQUIRE(!tokens[0].revoked);
}

TEST_CASE("plugin control relay: an operation named twice is never taken for "
          "hello by the gateway")
{
    world gateway;
    relay_rig rig(gateway.options());
    // What the relay and the gateway take as the operation is the same
    // (both read the header with the same JSON reader), so one of two
    // things happens: the relay refuses the frame, or the gateway answers
    // the other operation.
    const std::vector<std::string> headers = {
        "{\"id\":5,\"op\":\"hello\",\"op\":\"ports.list\"}",
        "{\"id\":6,\"op\":\"ports.list\",\"op\":\"hello\"}"};
    std::uint64_t id = 5;
    for (const std::string& header : headers)
    {
        std::string peer = "dup ";
        peer.append(std::to_string(id));
        const auto opened = rig.open({"monitor"}, peer);
        rig.send(opened.session, header);

        bool refused = false;
        bool answered = false;
        const auto deadline = std::chrono::steady_clock::now() + long_limit;
        while (!refused && !answered)
        {
            for (const auto& note : rig.closed())
                refused = refused || note.session == opened.session;
            for (const offered_frame& item : rig.frames())
            {
                const auto decoded = control::decode_server_message(
                    control::frame{item.header, item.blob});
                const auto* reply =
                    decoded ? std::get_if<control::reply>(&*decoded) : nullptr;
                answered = answered || (reply != nullptr && reply->id == id);
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(10ms);
        }
        if (refused)
        {
            // The relay read `hello` and refused it.
            REQUIRE(rig.wait_code(opened.session) ==
                    control_close_code::refused_frame);
            REQUIRE(!answered);
        }
        else
        {
            // The gateway read the other operation and answered it; it did
            // not take the frame for a second hello, which would have ended
            // the session.
            reply_as<control::ports_reply>(wait_reply(rig, id));
            send_request(rig, opened.session, 90 + id, control::ping_request{});
            reply_as<control::done_reply>(wait_reply(rig, 90 + id));
            REQUIRE_EQ(rig.session_count(), std::size_t{1});
            rig.close_session(opened.session);
        }
        ++id;
    }
}

TEST_CASE("plugin control relay: a gateway that is full ends the request with "
          "the token revoked")
{
    world gateway;
    // The gateway takes eight sessions on its local endpoint; two relays hold
    // four each.
    control_relay_options options_a = gateway.options();
    options_a.plugin_id = "com.example.a";
    control_relay_options options_b = gateway.options();
    options_b.plugin_id = "com.example.b";
    control_relay_options options_c = gateway.options();
    options_c.plugin_id = "com.example.c";
    relay_rig rig_a(options_a);
    relay_rig rig_b(options_b);
    relay_rig rig_c(options_c);
    for (int count = 0; count < 4; ++count)
    {
        std::string peer = "peer ";
        peer.append(std::to_string(count));
        rig_a.open({"monitor"}, peer);
        rig_b.open({"monitor"}, peer);
    }

    const auto answer = rig_c.request(asking_for({"monitor"}, "ninth"));
    REQUIRE(refusal_of(answer).why == control_refusal::plugin_unavailable);
    REQUIRE(contains(refusal_of(answer).text, "did not take"));
    REQUIRE_EQ(rig_c.session_count(), std::size_t{0});

    // The token that was made for it is revoked, and the others are not.
    const auto deadline = std::chrono::steady_clock::now() + long_limit;
    for (;;)
    {
        std::size_t revoked = 0;
        bool its_own = false;
        for (const control::token_entry& entry : plugin_tokens(gateway))
        {
            if (entry.revoked)
            {
                ++revoked;
                its_own = entry.label.rfind("plugin:com.example.c", 0) == 0;
            }
        }
        if (revoked == 1 && its_own && token_sessions(gateway).size() == 8)
            break;
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(20ms);
    }
}

// -- a gateway that is not the gateway
// -------------------------------------------

namespace
{

/// Something that listens where the gateway's local endpoint would be, under
/// a name of its own, and records every frame it is sent.  It answers
/// `hello` as another principal, or not at all.
class fake_control_endpoint
{
public:
    enum class behaviour
    {
        wrong_principal,
        silent
    };

    fake_control_endpoint(std::string instance, behaviour how)
        : instance_(std::move(instance)), how_(how),
          guard_(boost::asio::make_work_guard(io_))
    {
        auto made = vss::agent::listen_local(
            io_.get_executor(), vss::agent::local_endpoint::control, instance_);
        if (!made)
            throw vss_test::failure("the fake endpoint cannot listen");
        listener_ = std::move(*made);
        accept();
        thread_ = std::thread([this] { io_.run(); });
    }

    ~fake_control_endpoint()
    {
        try
        {
            boost::asio::post(io_,
                              [this]
                              {
                                  if (stream_)
                                      stream_->close();
                                  listener_->close();
                              });
            guard_.reset();
            thread_.join();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // Nothing to be done in a destructor of a test.
        }
    }

    fake_control_endpoint(const fake_control_endpoint&) = delete;
    fake_control_endpoint& operator=(const fake_control_endpoint&) = delete;

    const std::string& instance() const noexcept { return instance_; }

    std::vector<std::string> received() const
    {
        const std::lock_guard lock(mutex_);
        return headers_;
    }

    bool wait_for_frames(std::size_t count) const
    {
        const auto deadline = std::chrono::steady_clock::now() + long_limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (received().size() >= count)
                return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

private:
    void accept()
    {
        listener_->async_accept(
            [this](boost::system::error_code failed,
                   std::unique_ptr<vss::agent::local_stream> connected)
            {
                if (failed)
                    return;
                stream_ = std::move(connected);
                read();
            });
    }

    void read()
    {
        stream_->async_read_some(
            boost::asio::buffer(buffer_),
            [this](boost::system::error_code failed, std::size_t size)
            {
                if (failed)
                    return;
                reader_.feed(vss::bytes_view(buffer_.data(), size));
                for (;;)
                {
                    auto next = reader_.next();
                    if (!next || !*next)
                        break;
                    handle(**next);
                }
                read();
            });
    }

    void handle(const vss::agent::frame& received)
    {
        std::size_t count = 0;
        {
            const std::lock_guard lock(mutex_);
            headers_.push_back(received.header);
            count = headers_.size();
        }
        if (count != 1 || how_ != behaviour::wrong_principal)
            return;
        // The first frame is the hello: answer it as the user, with the local
        // role, as a gateway that did not take the token would.
        const auto asked = control::decode_request(received);
        if (!asked)
            return;
        control::hello_reply welcome;
        welcome.version = "fake";
        welcome.principal = "local:someone";
        welcome.scopes = {"monitor"};
        control::reply answer;
        answer.id = asked->id;
        answer.body = welcome;
        auto bytes = vss::agent::encode_frame(control::encode(answer));
        if (!bytes)
            return;
        out_ = std::make_shared<vss::byte_buffer>(std::move(*bytes));
        write(0);
    }

    void write(std::size_t offset)
    {
        stream_->async_write_some(
            boost::asio::buffer(out_->data() + offset, out_->size() - offset),
            [this, offset](boost::system::error_code failed, std::size_t size)
            {
                if (!failed && offset + size < out_->size())
                    write(offset + size);
            });
    }

    std::string instance_;
    behaviour how_;
    boost::asio::io_context io_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
        guard_;
    std::unique_ptr<vss::agent::local_listener> listener_;
    std::unique_ptr<vss::agent::local_stream> stream_;
    vss::byte_buffer buffer_ = vss::byte_buffer(4096);
    vss::agent::frame_reader reader_;
    std::shared_ptr<vss::byte_buffer> out_;
    mutable std::mutex mutex_;
    std::vector<std::string> headers_;
    std::thread thread_;
};

} // namespace

TEST_CASE("plugin control relay: a gateway that answers as another principal "
          "gets no frame of the plugin")
{
    world gateway;
    fake_control_endpoint fake(
        unique_instance(), fake_control_endpoint::behaviour::wrong_principal);
    control_relay_options options = gateway.options();
    options.control_instance = fake.instance();
    relay_rig rig(options);

    const auto answer = rig.request(asking_for({"monitor"}, "lab"));
    REQUIRE(refusal_of(answer).why == control_refusal::plugin_unavailable);
    REQUIRE(contains(refusal_of(answer).text, "did not take the connection"));
    REQUIRE_EQ(rig.session_count(), std::size_t{0});

    // The plugin tries with the number the session would have had.
    rig.send(1, "{\"id\":2,\"op\":\"ports.list\"}");
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    wait_until_revoked(gateway, 1);

    // The fake was sent the hello and nothing else.
    REQUIRE(fake.wait_for_frames(1));
    const auto seen = fake.received();
    REQUIRE_EQ(seen.size(), std::size_t{1});
    REQUIRE(contains(seen[0], "\"op\":\"hello\""));
    REQUIRE(contains(seen[0], "vsst1_"));
    REQUIRE(!contains(rig.everything_said(), "vsst1"));
}

TEST_CASE("plugin control relay: a gateway that does not answer hello gets no "
          "frame of the plugin")
{
    world gateway;
    fake_control_endpoint fake(unique_instance(),
                               fake_control_endpoint::behaviour::silent);
    control_relay_options options = gateway.options();
    options.control_instance = fake.instance();
    relay_rig rig(options);

    // The plugin sends a frame while the relay waits for the answer.
    const auto waiting = rig.request_async(asking_for({"monitor"}, "lab"));
    REQUIRE(fake.wait_for_frames(1));
    rig.send(1, "{\"id\":2,\"op\":\"ports.list\"}");
    REQUIRE(waiting.wait_for(long_limit) == std::future_status::ready);
    REQUIRE(refusal_of(waiting.get()).why ==
            control_refusal::plugin_unavailable);
    REQUIRE(rig.wait_code(1) == control_close_code::refused_frame);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    wait_until_revoked(gateway, 1);
    REQUIRE_EQ(fake.received().size(), std::size_t{1});
}

TEST_CASE("plugin control relay: a gateway that never answers hello is given "
          "up on and the token revoked")
{
    world gateway;
    fake_control_endpoint fake(unique_instance(),
                               fake_control_endpoint::behaviour::silent);
    control_relay_options options = gateway.options();
    options.control_instance = fake.instance();
    options.step_timeout = std::chrono::milliseconds{3000};
    relay_rig rig(options);

    const auto answer = rig.request(asking_for({"monitor"}, "lab"));
    REQUIRE(refusal_of(answer).why == control_refusal::plugin_unavailable);
    REQUIRE(contains(refusal_of(answer).text, "did not answer hello"));
    wait_until_revoked(gateway, 1);
    REQUIRE_EQ(fake.received().size(), std::size_t{1});
}
