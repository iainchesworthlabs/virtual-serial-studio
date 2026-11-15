//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss::plugin::host::control_relay` with no gateway running: what is
// checked before anyone is asked, what the owner is asked, and what a plugin
// is told when there is nothing to connect to.  The same relay against a real
// gateway is in test_system_plugin_control_relay_gateway.cpp.
//

#include <vss/plugin/host/control_relay.hpp>

#include "control_relay_rig.hpp"
#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using vss::plugin::host::clean_control_text;
using vss::plugin::host::control_close_code;
using vss::plugin::host::control_refusal;
using vss::plugin::host::control_relay;
using vss::plugin::host::control_relay_handlers;
using vss::plugin::host::control_relay_options;
using vss::plugin::host::control_session_answer;
using vss::plugin::host::control_session_refused;
using vss::plugin::host::control_session_request;
using vss_test_relay::approval_mode;
using vss_test_relay::relay_rig;
using namespace std::chrono_literals;

namespace
{

/// A gateway instance name that nothing listens on: up to 32 letters, digits,
/// `_` and `-`.
std::string unused_instance()
{
    std::random_device device;
    std::ostringstream text;
    text << "nogate" << std::hex << std::setw(8) << std::setfill('0')
         << device();
    return text.str();
}

control_relay_options options_for(const std::string& instance)
{
    control_relay_options options;
    options.plugin_id = "com.example.remote";
    options.plugin_name = "Remote control";
    options.instance = instance;
    return options;
}

control_session_request request_for(std::vector<std::string> scopes)
{
    control_session_request asking;
    asking.scopes = std::move(scopes);
    asking.peer = "lab computer";
    asking.reason = "to watch the rig";
    return asking;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

/// One character as UTF-8, written without the character in the source.
std::string cp(std::uint32_t point)
{
    std::string out;
    if (point < 0x800)
    {
        out.push_back(static_cast<char>(0xc0 | (point >> 6)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
    else if (point < 0x10000)
    {
        out.push_back(static_cast<char>(0xe0 | (point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
    else
    {
        out.push_back(static_cast<char>(0xf0 | (point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
    return out;
}

/// A string made of bytes, so that no source line holds a character that
/// changes how the text around it reads.
std::string raw(std::initializer_list<int> values)
{
    std::string out;
    for (const int value : values)
        out.push_back(static_cast<char>(value));
    return out;
}

const control_session_refused& refusal_of(const control_session_answer& answer)
{
    const auto* refused = std::get_if<control_session_refused>(&answer);
    if (refused == nullptr)
        throw vss_test::failure("the request was not refused");
    return *refused;
}

} // namespace

TEST_CASE("plugin control relay: text for a person loses what cannot be seen")
{
    REQUIRE_EQ(clean_control_text("lab computer", 64),
               std::string("lab computer"));
    // Control characters go, and the spaces at the ends.
    REQUIRE_EQ(clean_control_text("  a\tb\nc\x7f  ", 64), std::string("abc"));
    // The line separator, the zero width space, the right to left override
    // and the byte order mark.
    std::string unseen = "a";
    unseen.append(raw({0xe2, 0x80, 0xa8})).append("b");
    unseen.append(raw({0xe2, 0x80, 0x8b})).append("c");
    unseen.append(raw({0xe2, 0x80, 0xae})).append("d");
    unseen.append(raw({0xef, 0xbb, 0xbf})).append("e");
    REQUIRE_EQ(clean_control_text(unseen, 64), std::string("abcde"));
    // A C1 control character, as two bytes.
    REQUIRE_EQ(clean_control_text("a\xc2\x85"
                                  "b",
                                  64),
               std::string("ab"));
    // Bytes that are not UTF-8: a stray byte, an overlong form, a surrogate
    // and a sequence that ends too soon.
    REQUIRE_EQ(clean_control_text("a\xff"
                                  "b\xc0\xaf"
                                  "c\xed\xa0\x80"
                                  "d\xe2\x82",
                                  64),
               std::string("abcd"));
    // A letter with an accent stays whole, and is never cut in two.
    REQUIRE_EQ(clean_control_text("caf\xc3\xa9", 64),
               std::string("caf\xc3\xa9"));
    REQUIRE_EQ(clean_control_text("caf\xc3\xa9", 4), std::string("caf"));
    REQUIRE_EQ(clean_control_text("abcdef", 3), std::string("abc"));
    REQUIRE_EQ(clean_control_text(" \t ", 64), std::string());
    REQUIRE_EQ(clean_control_text("", 64), std::string());
}

TEST_CASE("plugin control relay: a close code has the name the message uses")
{
    using vss::plugin::host::to_string;
    REQUIRE_EQ(to_string(control_close_code::revoked),
               std::string_view("revoked"));
    REQUIRE_EQ(to_string(control_close_code::expired),
               std::string_view("expired"));
    REQUIRE_EQ(to_string(control_close_code::gateway_closed),
               std::string_view("gateway_closed"));
    REQUIRE_EQ(to_string(control_close_code::refused_frame),
               std::string_view("refused_frame"));
    REQUIRE_EQ(to_string(control_close_code::host_stopped),
               std::string_view("host_stopped"));
}

TEST_CASE("plugin control relay: a request is checked before anyone is asked")
{
    relay_rig rig(options_for(unused_instance()));

    struct bad_request
    {
        std::vector<std::string> scopes{};
        std::string peer{};
        std::string reason{};
        control_refusal expected = control_refusal::invalid_request;
    };
    std::vector<bad_request> bad;
    bad.push_back({{"admin"}, "p", "r", control_refusal::permission_denied});
    // `admin` among others, and among names that do not exist.
    bad.push_back(
        {{"monitor", "admin"}, "p", "r", control_refusal::permission_denied});
    bad.push_back(
        {{"nonsense", "admin"}, "p", "r", control_refusal::permission_denied});
    bad.push_back({{"monitor.everything"}, "p", "r"});
    bad.push_back({{"Monitor"}, "p", "r"});
    bad.push_back({{}, "p", "r"});
    bad.push_back({{"monitor"}, std::string(65, 'p'), "r"});
    bad.push_back({{"monitor"}, "p", std::string(257, 'r')});
    bad.push_back({std::vector<std::string>(33, "monitor"), "p", "r"});

    for (const bad_request& item : bad)
    {
        control_session_request asking;
        asking.scopes = item.scopes;
        asking.peer = item.peer;
        asking.reason = item.reason;
        const auto answer = rig.request(std::move(asking));
        const control_session_refused& refused = refusal_of(answer);
        REQUIRE(refused.why == item.expected);
        REQUIRE(!refused.text.empty());
    }
    REQUIRE(rig.approvals().empty());

    // The longest texts that are allowed are not refused for their length.
    rig.set_mode(approval_mode::deny);
    control_session_request longest = request_for({"monitor"});
    longest.peer = std::string(64, 'p');
    longest.reason = std::string(256, 'r');
    const auto answer = rig.request(std::move(longest));
    REQUIRE(refusal_of(answer).why == control_refusal::permission_denied);
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
}

TEST_CASE("plugin control relay: the owner is asked for what will be asked of "
          "the gateway")
{
    control_relay_options options = options_for(unused_instance());
    options.token_life = std::chrono::hours{2};
    relay_rig rig(options);

    control_session_request asking =
        request_for({"record", "monitor", "monitor", "control.ports"});
    asking.peer = "  lab\x01 PC ";
    asking.peer.append(raw({0xe2, 0x80, 0xae}));
    asking.reason = "to\nwatch";
    const auto answer = rig.request(std::move(asking));

    const auto asked = rig.approvals();
    REQUIRE_EQ(asked.size(), std::size_t{1});
    REQUIRE_EQ(asked[0].plugin_id, std::string("com.example.remote"));
    REQUIRE_EQ(asked[0].plugin_name, std::string("Remote control"));
    REQUIRE_EQ(asked[0].peer, std::string("lab PC"));
    REQUIRE_EQ(asked[0].reason, std::string("towatch"));
    // Each once, in the order of the scope list.
    REQUIRE_EQ(asked[0].scopes.size(), std::size_t{3});
    REQUIRE_EQ(asked[0].scopes[0], std::string("monitor"));
    REQUIRE_EQ(asked[0].scopes[1], std::string("control.ports"));
    REQUIRE_EQ(asked[0].scopes[2], std::string("record"));
    REQUIRE(asked[0].token_life == std::chrono::hours{2});

    // Nothing runs behind it, so the plugin is told that, in plain words, and
    // nothing is tried again.
    const control_session_refused& refused = refusal_of(answer);
    REQUIRE(refused.why == control_refusal::plugin_unavailable);
    REQUIRE_EQ(refused.text, std::string("No control gateway is running."));
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
}

TEST_CASE("plugin control relay: an owner who says no refuses the request")
{
    relay_rig rig(options_for(unused_instance()));
    rig.set_mode(approval_mode::deny);
    const auto answer = rig.request(request_for({"monitor"}));
    const control_session_refused& refused = refusal_of(answer);
    REQUIRE(refused.why == control_refusal::permission_denied);
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
}

TEST_CASE("plugin control relay: with no one to ask nothing is approved")
{
    boost::asio::io_context io;
    const auto relay =
        control_relay::create(io.get_executor(), options_for(unused_instance()),
                              control_relay_handlers{});
    std::optional<control_session_answer> answer;
    relay->request_session(request_for({"monitor"}),
                           [&answer](control_session_answer given)
                           { answer = std::move(given); });
    // The answer is never given before the call has returned.
    REQUIRE(!answer.has_value());
    io.run();
    REQUIRE(answer.has_value());
    REQUIRE(refusal_of(*answer).why == control_refusal::permission_denied);
}

TEST_CASE("plugin control relay: a second request waits for the first")
{
    relay_rig rig(options_for(unused_instance()));
    rig.set_mode(approval_mode::hold);

    const auto first = rig.request_async(request_for({"monitor"}));
    REQUIRE(rig.wait_approvals(1));

    const auto second = rig.request(request_for({"monitor"}));
    REQUIRE(refusal_of(second).why == control_refusal::busy);
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});

    // The owner approves; there is no gateway, so the first ends as well.
    rig.release(true);
    REQUIRE(first.wait_for(vss_test_relay::long_limit) ==
            std::future_status::ready);
    REQUIRE(refusal_of(first.get()).why == control_refusal::plugin_unavailable);

    // The request is over, so another one may be made.
    rig.set_mode(approval_mode::deny);
    const auto third = rig.request(request_for({"monitor"}));
    REQUIRE(refusal_of(third).why == control_refusal::permission_denied);
}

TEST_CASE("plugin control relay: closing everything ends a request that waits "
          "for the owner")
{
    relay_rig rig(options_for(unused_instance()));
    rig.set_mode(approval_mode::hold);

    const auto waiting = rig.request_async(request_for({"monitor"}));
    REQUIRE(rig.wait_approvals(1));
    rig.close_all();
    REQUIRE(waiting.wait_for(vss_test_relay::long_limit) ==
            std::future_status::ready);
    REQUIRE(refusal_of(waiting.get()).why ==
            control_refusal::plugin_unavailable);

    // An approval that comes late finds nothing to approve: it is ignored,
    // and a new request is asked afresh.
    rig.release(true);
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    rig.set_mode(approval_mode::deny);
    const auto again = rig.request(request_for({"monitor"}));
    REQUIRE(refusal_of(again).why == control_refusal::permission_denied);
    REQUIRE_EQ(rig.approvals().size(), std::size_t{2});
}

TEST_CASE("plugin control relay: a frame for a number that is no session is "
          "dropped")
{
    relay_rig rig(options_for(unused_instance()));
    rig.send(7, "{\"id\":1,\"op\":\"ping\"}");
    rig.close_session(7);
    // Nothing was said about a session the plugin made up.
    REQUIRE_EQ(rig.session_count(), std::size_t{0});
    REQUIRE(rig.closed().empty());
}

TEST_CASE("plugin control relay: invisible characters are cleaned and odd "
          "spaces become one space")
{
    const std::vector<std::uint32_t> invisible = {
        0x61c,  0xad,    0x34f,   0x115f,  0x1160,  0x3164, 0x180e, 0xfe00,
        0xfe0f, 0xe0100, 0xe01ef, 0xe0000, 0xe007f, 0x200b, 0x200f, 0x202a,
        0x202e, 0x2066,  0x2069,  0xfeff,  0x2028,  0x85};
    for (const std::uint32_t point : invisible)
    {
        std::string text = "a";
        text.append(cp(point)).append("b");
        REQUIRE_EQ(clean_control_text(text, 64), std::string("ab"));
    }
    const std::vector<std::uint32_t> spaces = {0xa0,   0x2000, 0x2003, 0x200a,
                                               0x202f, 0x205f, 0x3000};
    for (const std::uint32_t point : spaces)
    {
        std::string text = "a";
        text.append(cp(point)).append("b");
        REQUIRE_EQ(clean_control_text(text, 64), std::string("a b"));
    }
    std::string runs = cp(0x3000);
    runs.append("  a ").append(cp(0xa0)).append(cp(0x2003)).append(" b  ");
    runs.append(cp(0x202f));
    REQUIRE_EQ(clean_control_text(runs, 64), std::string("a b"));
    // The cap counts the single space.
    REQUIRE_EQ(clean_control_text("ab   cd", 3), std::string("ab"));
}

TEST_CASE("plugin control relay: the plugin's id and name are cleaned for the "
          "prompt")
{
    control_relay_options options = options_for(unused_instance());
    options.plugin_id = "id";
    options.plugin_id.append(cp(0x202e)).append(std::string(100, 'x'));
    options.plugin_name = "Name";
    options.plugin_name.append(cp(0x3000)).append(cp(0xa0)).append("here");
    relay_rig rig(options);
    rig.set_mode(approval_mode::deny);
    rig.request(request_for({"monitor"}));
    const auto asked = rig.approvals();
    REQUIRE_EQ(asked.size(), std::size_t{1});
    REQUIRE_EQ(asked[0].plugin_id.size(), std::size_t{64});
    REQUIRE_EQ(asked[0].plugin_id.substr(0, 4), std::string("idxx"));
    REQUIRE_EQ(asked[0].plugin_name, std::string("Name here"));
}

TEST_CASE("plugin control relay: after a refusal the relay refuses by itself "
          "for longer each time")
{
    vss_test_relay::fake_clock clock;
    control_relay_options options = options_for(unused_instance());
    options.clock = clock.function();
    relay_rig rig(options);
    rig.set_mode(approval_mode::deny);

    // Asks, and says what the plugin was told.
    const auto ask = [&]
    {
        const auto answer = rig.request(request_for({"monitor"}));
        REQUIRE(refusal_of(answer).why == control_refusal::permission_denied);
        return refusal_of(answer).text;
    };

    REQUIRE(contains(ask(), "Try again in 60 seconds."));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});
    // Within the minute nobody is asked.
    REQUIRE(contains(ask(), "Try again in 60 seconds."));
    clock.advance(30s);
    REQUIRE(contains(ask(), "Try again in 30 seconds."));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{1});

    // Then it asks, and the time doubles, up to 15 minutes.
    clock.advance(31s);
    REQUIRE(contains(ask(), "Try again in 2 minutes."));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{2});
    clock.advance(121s);
    REQUIRE(contains(ask(), "Try again in 4 minutes."));
    clock.advance(241s);
    REQUIRE(contains(ask(), "Try again in 8 minutes."));
    clock.advance(481s);
    REQUIRE(contains(ask(), "Try again in 15 minutes."));
    clock.advance(901s);
    REQUIRE(contains(ask(), "Try again in 15 minutes."));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{6});

    // An approval starts it again from a minute.  No gateway runs, so the
    // request ends there, but the owner said yes.
    clock.advance(901s);
    rig.set_mode(approval_mode::approve);
    const auto approved = rig.request(request_for({"monitor"}));
    REQUIRE(refusal_of(approved).why == control_refusal::plugin_unavailable);
    rig.set_mode(approval_mode::deny);
    REQUIRE(contains(ask(), "Try again in 60 seconds."));
    REQUIRE_EQ(rig.approvals().size(), std::size_t{8});
}

TEST_CASE("plugin control relay: a request's done is not called once the relay "
          "is gone, and the relay may be destroyed inside it")
{
    {
        boost::asio::io_context io;
        auto relay = control_relay::create(io.get_executor(),
                                           options_for(unused_instance()),
                                           control_relay_handlers{});
        bool called = false;
        relay->request_session(request_for({"monitor"}),
                               [&](const control_session_answer&)
                               {
                                   called = true;
                                   relay.reset();
                               });
        io.run();
        REQUIRE(called);
        REQUIRE(relay == nullptr);
    }
    {
        boost::asio::io_context io;
        auto relay = control_relay::create(io.get_executor(),
                                           options_for(unused_instance()),
                                           control_relay_handlers{});
        bool called = false;
        relay->request_session(request_for({"monitor"}),
                               [&](const control_session_answer&)
                               { called = true; });
        relay.reset();
        io.run();
        REQUIRE(!called);
    }
}

TEST_CASE("plugin control relay: close_all's done is not called once the relay "
          "is gone")
{
    boost::asio::io_context io;
    auto relay =
        control_relay::create(io.get_executor(), options_for(unused_instance()),
                              control_relay_handlers{});
    bool called = false;
    relay->close_all([&] { called = true; });
    io.run();
    REQUIRE(called);

    io.restart();
    bool late = false;
    relay->close_all([&] { late = true; });
    relay.reset();
    io.run();
    REQUIRE(!late);
}
