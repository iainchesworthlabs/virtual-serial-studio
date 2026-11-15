//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control API without a connection: every message there and back, what a
// decoder refuses, the names of the errors, and the policy as a table.  The
// table of who may do what is written out here a second time, by hand, so
// that a change to the one in the library that was not meant shows up as a
// failure (docs/remote-control.md, sections 5, 7 and 15).
//

#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/proto/error.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/control/proto/policy.hpp>

#include "test_framework.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace control = vss::control;
namespace agent = vss::agent;

namespace
{

vss::error_code code(control::api_errc value)
{
    return control::make_error_code(value);
}

control::frame header_frame(std::string header)
{
    control::frame out;
    out.header = std::move(header);
    return out;
}

control::request round_trip(const control::request& value)
{
    const auto decoded = control::decode_request(control::encode(value));
    REQUIRE(decoded);
    return *decoded;
}

control::reply round_trip(const control::reply& value)
{
    const auto decoded = control::decode_reply(control::encode(value));
    REQUIRE(decoded);
    return *decoded;
}

constexpr std::array all_transports{
    control::transport::local, control::transport::local_admin,
    control::transport::loopback, control::transport::tls};

/// Every set of scopes there is: 2^8 of them.
std::vector<control::scope_set> every_scope_set()
{
    std::vector<control::scope_set> out;
    const std::size_t count = std::size(control::all_scope_values);
    for (std::uint32_t bits = 0; bits < (1u << count); ++bits)
    {
        control::scope_set set;
        for (std::size_t index = 0; index < count; ++index)
        {
            if ((bits & (1u << index)) != 0)
                set =
                    set | control::scope_set{control::all_scope_values[index]};
        }
        out.push_back(set);
    }
    return out;
}

/// The scope each operation needs, written out by hand.  Nothing here is
/// computed from the library's table.
struct expected_row
{
    control::operation id;
    std::string_view name;
    std::optional<control::scope> required;
};

constexpr std::array expected_operations{
    expected_row{control::operation::hello, "hello", std::nullopt},
    expected_row{control::operation::status, "status", control::scope::monitor},
    expected_row{control::operation::ports_list, "ports.list",
                 control::scope::monitor},
    expected_row{control::operation::endpoint_stop, "endpoint.stop",
                 control::scope::admin},
    expected_row{control::operation::tokens_create, "tokens.create",
                 control::scope::admin},
    expected_row{control::operation::tokens_list, "tokens.list",
                 control::scope::admin},
    expected_row{control::operation::tokens_revoke, "tokens.revoke",
                 control::scope::admin},
    expected_row{control::operation::events_subscribe, "events.subscribe",
                 control::scope::monitor},
    expected_row{control::operation::events_unsubscribe, "events.unsubscribe",
                 control::scope::monitor},
    expected_row{control::operation::traffic_subscribe, "traffic.subscribe",
                 control::scope::monitor_data},
    expected_row{control::operation::traffic_unsubscribe, "traffic.unsubscribe",
                 control::scope::monitor_data},
    expected_row{control::operation::sessions_list, "sessions.list",
                 control::scope::admin},
    expected_row{control::operation::sessions_kick, "sessions.kick",
                 control::scope::admin},
    expected_row{control::operation::ports_create, "ports.create",
                 control::scope::control_ports},
    expected_row{control::operation::ports_remove, "ports.remove",
                 control::scope::control_ports},
    expected_row{control::operation::links_connect, "links.connect",
                 control::scope::control_ports},
    expected_row{control::operation::ports_send, "ports.send",
                 control::scope::control_data},
    expected_row{control::operation::expect_bytes, "expect.bytes",
                 control::scope::monitor_data},
    expected_row{control::operation::runs_begin, "runs.begin",
                 control::scope::monitor},
    expected_row{control::operation::runs_end, "runs.end",
                 control::scope::monitor},
    expected_row{control::operation::runs_record, "runs.record",
                 control::scope::monitor},
    expected_row{control::operation::report_junit, "report.junit",
                 control::scope::monitor},
    expected_row{control::operation::ports_get, "ports.get",
                 control::scope::monitor},
    expected_row{control::operation::links_list, "links.list",
                 control::scope::monitor},
    expected_row{control::operation::links_get, "links.get",
                 control::scope::monitor},
    expected_row{control::operation::ping, "ping", std::nullopt},
    expected_row{control::operation::session_close, "session.close",
                 std::nullopt},
    expected_row{control::operation::links_disconnect, "links.disconnect",
                 control::scope::control_ports},
    expected_row{control::operation::ports_rename, "ports.rename",
                 control::scope::control_ports}};

} // namespace

// -- messages ----------------------------------------------------------------

TEST_CASE("control proto: every request survives the wire")
{
    const control::request hello{7, control::hello_request{1, "pytest 8.3"}};
    REQUIRE(round_trip(hello) == hello);
    const control::request status{8, control::status_request{}};
    REQUIRE(round_trip(status) == status);
    const control::request list{9, control::ports_list_request{}};
    REQUIRE(round_trip(list) == list);
    const control::request stop{10, control::endpoint_stop_request{}};
    REQUIRE(round_trip(stop) == stop);

    control::hello_request with_token{1, "rig"};
    with_token.token = control::secret_string(std::string_view(
        "vsst1_aaaaaaaaaaaaa_"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    const control::request tokened{11, with_token};
    REQUIRE(round_trip(tokened) == tokened);
    const control::request create{
        12, control::tokens_create_request{
                "Lab rig 3", {"monitor", "monitor.data"}, 86400, {"loopback"}}};
    REQUIRE(round_trip(create) == create);
    const control::request listing{13, control::tokens_list_request{}};
    REQUIRE(round_trip(listing) == listing);
    const control::request revoke{
        14, control::tokens_revoke_request{"aaaaaaaaaaaaa"}};
    REQUIRE(round_trip(revoke) == revoke);
}

TEST_CASE("control proto: a hello's token is text of a token's size at most")
{
    const auto with = [](const std::string& token)
    {
        return control::decode_request(header_frame(
            R"({"id":1,"op":"hello","api":1,"token":)" + token + "}"));
    };
    const auto good = with("\"" + std::string(72, 'a') + "\"");
    REQUIRE(good);
    const auto* hello = std::get_if<control::hello_request>(&good->body);
    REQUIRE(hello != nullptr);
    REQUIRE_EQ(hello->token.size(), std::size_t{72});
    // No token and an empty one are the same: none.
    const auto none = with("\"\"");
    REQUIRE(none);
    REQUIRE(std::get<control::hello_request>(none->body).token.empty());

    REQUIRE(!with("\"" + std::string(control::max_token_text_size + 1, 'a') +
                  "\""));
    REQUIRE(!with("12345"));
    REQUIRE(!with("null"));
    REQUIRE(!with("[\"a\"]"));
}

TEST_CASE("control proto: the token replies survive the wire")
{
    control::token_entry entry;
    entry.id = "aaaaaaaaaaaaa";
    entry.label = "Lab rig 3";
    entry.scopes = {"monitor"};
    entry.transports = {"local"};
    entry.created_ms = 1'800'000'000'000;
    entry.expires_ms = 1'802'592'000'000;
    entry.last_used_ms = 0;

    control::token_created_reply created;
    created.token = control::secret_string(std::string_view("vsst1_x_y"));
    created.entry = entry;
    const control::reply made{5, created};
    REQUIRE(round_trip(made) == made);

    control::token_entry used = entry;
    used.id = "bbbbbbbbbbbbb";
    used.last_used_ms = 1'800'000'100'000;
    used.revoked = true;
    control::tokens_reply listed;
    listed.tokens = {entry, used};
    const control::reply list{6, listed};
    REQUIRE(round_trip(list) == list);

    // A token reply without its entry, or a list entry short of a field.
    REQUIRE(!control::decode_reply(
        header_frame(R"({"id":1,"reply":"token","token":"t"})")));
    REQUIRE(!control::decode_reply(header_frame(
        R"({"id":1,"reply":"tokens","tokens":[{"id":"a","label":"b"}]})")));
}

TEST_CASE("control proto: every reply survives the wire")
{
    const control::reply done{3, control::done_reply{}};
    REQUIRE(round_trip(done) == done);

    control::hello_reply welcome;
    welcome.version = "0.1.0";
    welcome.principal = "local:1000";
    welcome.scopes = {"monitor", "monitor.data"};
    welcome.features = {"ports.read"};
    const control::reply hello{4, welcome};
    REQUIRE(round_trip(hello) == hello);

    control::status_reply state;
    state.uptime_ms = 123456;
    state.sessions = 2;
    state.agent_running = true;
    state.agent_version = "0.1.0";
    const control::reply status{5, state};
    REQUIRE(round_trip(status) == status);
    state.edition = "enterprise";
    state.licence_state = "grace";
    const control::reply licensed{5, state};
    REQUIRE(round_trip(licensed) == licensed);

    control::ports_reply listed;
    listed.revision = 12;
    listed.ports.push_back(
        {"p_1", "alpha", "/dev/pts/3", "virtual", "open", true, 10, 20});
    listed.ports.push_back({"p_2", "beta", "", "mock", "created", false, 0, 0});
    listed.links.push_back({"l_1", "duplex", {"p_1"}, {"p_2"}, 30});
    const control::reply ports{6, listed};
    REQUIRE(round_trip(ports) == ports);

    const control::reply refused{0, control::error_reply{control::to_api_error(
                                        control::api_errc::forbidden)}};
    REQUIRE(round_trip(refused) == refused);
}

TEST_CASE("control proto: a field this version does not know is ignored")
{
    const auto request = control::decode_request(header_frame(
        R"({"id":1,"op":"hello","api":1,"client":"x","token":"later",)"
        R"("future":{"a":[1,2]}})"));
    REQUIRE(request);
    REQUIRE(std::holds_alternative<control::hello_request>(request->body));

    const auto reply = control::decode_reply(
        header_frame(R"({"id":1,"reply":"done","next_pin":"abc"})"));
    REQUIRE(reply);
    REQUIRE(std::holds_alternative<control::done_reply>(reply->body));
}

TEST_CASE("control proto: hello may leave the client's name out")
{
    const auto request = control::decode_request(
        header_frame(R"({"id":1,"op":"hello","api":1})"));
    REQUIRE(request);
    const auto* hello = std::get_if<control::hello_request>(&request->body);
    REQUIRE(hello != nullptr);
    REQUIRE(hello->client.empty());
}

TEST_CASE("control proto: a request that is not one is refused")
{
    const std::array bad{
        std::string("not json"), std::string("[1,2,3]"),
        std::string(R"({"op":"status"})"),
        std::string(R"({"id":0,"op":"status"})"),
        std::string(R"({"id":-1,"op":"status"})"),
        std::string(R"({"id":"1","op":"status"})"),
        std::string(R"({"id":1.5,"op":"status"})"), std::string(R"({"id":1})"),
        std::string(R"({"id":1,"op":7})"),
        std::string(R"({"id":1,"op":"hello"})"),
        std::string(R"({"id":1,"op":"hello","api":"1"})"),
        std::string(R"({"id":1,"op":"hello","api":4294967296})"),
        std::string(R"({"id":1,"op":"hello","api":1,"client":5})"),
        std::string(R"({"id":1,"op":"hello","api":1,"client":")") +
            std::string(control::max_text_size + 1, 'x') + R"("})",
        // Nine levels of nesting: one more than a header may have.
        std::string(R"({"id":1,"op":"status","x":[[[[[[[[1]]]]]]]]})"),
        // A header that is not UTF-8.
        std::string("{\"id\":1,\"op\":\"status\",\"x\":\"\xff\xfe\"}")};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_request(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
}

TEST_CASE("control proto: an operation that does not exist is unsupported")
{
    for (const char* name : {"ports.settings", "tokens.forge", "HELLO", "",
                             "status ", "endpoint.enable"})
    {
        const auto decoded = control::decode_request(
            header_frame(std::string(R"({"id":4,"op":")") + name + R"("})"));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::unsupported));
    }
}

TEST_CASE("control proto: no request of this version carries bytes")
{
    control::frame carried =
        control::encode(control::request{1, control::ports_list_request{}});
    carried.blob = {vss::byte{1}, vss::byte{2}};
    const auto decoded = control::decode_request(carried);
    REQUIRE(!decoded);
    REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
}

TEST_CASE("control proto: a reply that is not one is refused")
{
    const std::array bad{
        std::string("{}"),
        std::string(R"({"id":1})"),
        std::string(R"({"id":1,"reply":"surprise"})"),
        std::string(R"({"id":1,"reply":"hello"})"),
        std::string(R"({"id":1,"reply":"status","uptime_ms":1,"sessions":1})"),
        // No licence part, an edition or a state the protocol has no word
        // for, and an edition with a licence that is not in force.
        std::string(R"({"id":1,"reply":"status","uptime_ms":1,"sessions":1,)"
                    R"("agent":{"running":false,"version":""}})"),
        std::string(R"({"id":1,"reply":"status","uptime_ms":1,"sessions":1,)"
                    R"("agent":{"running":false,"version":""},)"
                    R"("licence":{"edition":"gold","state":"valid"}})"),
        std::string(R"({"id":1,"reply":"status","uptime_ms":1,"sessions":1,)"
                    R"("agent":{"running":false,"version":""},)"
                    R"("licence":{"edition":"pro","state":"fine"}})"),
        std::string(R"({"id":1,"reply":"status","uptime_ms":1,"sessions":1,)"
                    R"("agent":{"running":false,"version":""},)"
                    R"("licence":{"edition":"pro","state":"expired"}})"),
        std::string(R"({"id":1,"reply":"status","uptime_ms":1,"sessions":1,)"
                    R"("agent":{"running":false,"version":""},)"
                    R"("licence":{"edition":"pro","state":"none"}})"),
        std::string(R"({"id":1,"reply":"ports","revision":1,"ports":{}})"),
        std::string(R"({"id":1,"reply":"ports","revision":1,"ports":[7],)"
                    R"("links":[]})"),
        std::string(R"({"id":1,"reply":"error","error":"forbidden"})")};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_reply(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
}

TEST_CASE("control proto: text that is not ours is made safe to send")
{
    control::ports_reply listed;
    control::port_entry port;
    port.id = "p_1";
    // A name with bytes that are not UTF-8, and a path that is too long.
    port.name = std::string("bad\xff\xfename");
    port.path = std::string(control::max_text_size * 2, 'p');
    port.kind = "virtual";
    port.state = "open";
    listed.ports.push_back(port);

    const auto decoded =
        control::decode_reply(control::encode(control::reply{1, listed}));
    REQUIRE(decoded);
    const auto* back = std::get_if<control::ports_reply>(&decoded->body);
    REQUIRE(back != nullptr);
    REQUIRE_EQ(back->ports.at(0).name, std::string("bad??name"));
    REQUIRE_EQ(back->ports.at(0).path.size(), control::max_text_size);
}

TEST_CASE("control proto: the id of a request that cannot be read")
{
    REQUIRE_EQ(
        control::peek_request_id(header_frame(R"({"id":42,"op":"no.such"})")),
        std::uint64_t{42});
    REQUIRE_EQ(control::peek_request_id(header_frame(R"({"op":"status"})")),
               std::uint64_t{0});
    REQUIRE_EQ(control::peek_request_id(header_frame("garbage")),
               std::uint64_t{0});
}

TEST_CASE("control proto: public ids are opaque text")
{
    REQUIRE_EQ(control::port_id_text(3), std::string("p_3"));
    REQUIRE_EQ(control::link_id_text(12), std::string("l_12"));
}

TEST_CASE("control proto: the reader holds a first frame to the hello limit")
{
    // A frame of one byte more than a hello may have is refused when its
    // length arrives, before any of its body is buffered.
    agent::frame_reader reader(control::max_hello_frame_size);
    const std::uint32_t size = control::max_hello_frame_size + 1;
    const vss::byte_buffer prefix{static_cast<vss::byte>(size & 0xffu),
                                  static_cast<vss::byte>((size >> 8) & 0xffu),
                                  vss::byte{0}, vss::byte{0}};
    reader.feed(vss::bytes_view(prefix.data(), prefix.size()));
    const auto next = reader.next();
    REQUIRE(!next);
    REQUIRE_EQ(next.error(),
               agent::make_error_code(agent::protocol_errc::oversized_frame));

    // The same length is fine once the limit has been raised.
    agent::frame_reader raised(control::max_hello_frame_size);
    raised.set_limit(agent::max_frame_size);
    raised.feed(vss::bytes_view(prefix.data(), prefix.size()));
    const auto waiting = raised.next();
    REQUIRE(waiting);
    REQUIRE(!waiting->has_value());

    // A limit above the largest frame is brought down to it.
    agent::frame_reader greedy(agent::max_frame_size * 2);
    const std::uint32_t huge =
        static_cast<std::uint32_t>(agent::max_frame_size) + 1;
    const vss::byte_buffer huge_prefix{
        static_cast<vss::byte>(huge & 0xffu),
        static_cast<vss::byte>((huge >> 8) & 0xffu),
        static_cast<vss::byte>((huge >> 16) & 0xffu),
        static_cast<vss::byte>((huge >> 24) & 0xffu)};
    greedy.feed(vss::bytes_view(huge_prefix.data(), huge_prefix.size()));
    REQUIRE(!greedy.next());
}

// -- errors ------------------------------------------------------------------

TEST_CASE("control proto: every error has a name that reads back")
{
    constexpr std::array all{control::api_errc::unauthenticated,
                             control::api_errc::forbidden,
                             control::api_errc::invalid_argument,
                             control::api_errc::not_found,
                             control::api_errc::conflict,
                             control::api_errc::unsupported,
                             control::api_errc::busy,
                             control::api_errc::timeout,
                             control::api_errc::limit_exceeded,
                             control::api_errc::cancelled,
                             control::api_errc::unavailable,
                             control::api_errc::internal};
    std::set<std::string_view> names;
    for (const control::api_errc value : all)
    {
        const std::string_view name = control::code_name(value);
        REQUIRE(names.insert(name).second);
        REQUIRE(control::parse_code(name) == value);
        const control::api_error wire = control::to_api_error(value);
        REQUIRE_EQ(wire.code, std::string(name));
        REQUIRE(!wire.message.empty());
        REQUIRE_EQ(wire.retryable, control::retryable(value));
        REQUIRE_EQ(control::from_api_error(wire), code(value));
        REQUIRE(!code(value).message().empty());
    }
    REQUIRE(control::retryable(control::api_errc::busy));
    REQUIRE(control::retryable(control::api_errc::unavailable));
    REQUIRE(!control::retryable(control::api_errc::forbidden));
    REQUIRE(!control::retryable(control::api_errc::unauthenticated));

    // A name from a later version is an internal error here, never a success.
    REQUIRE(!control::parse_code("brand_new").has_value());
    const control::api_error later{"brand_new", "x", false};
    REQUIRE_EQ(control::from_api_error(later),
               code(control::api_errc::internal));
    REQUIRE_EQ(std::string(control::api_category().name()),
               std::string("vss.control"));
}

// -- policy ------------------------------------------------------------------

TEST_CASE("control policy: scopes have names and no scope implies another")
{
    std::set<std::string_view> names;
    for (const control::scope value : control::all_scope_values)
    {
        const std::string_view name = control::scope_name(value);
        REQUIRE(names.insert(name).second);
        REQUIRE(control::parse_scope(name) == value);
        // A set of one holds that one and nothing else.
        const control::scope_set alone{value};
        for (const control::scope other : control::all_scope_values)
        {
            REQUIRE_EQ(alone.contains(other), other == value);
        }
    }
    REQUIRE(!control::parse_scope("root").has_value());
    REQUIRE(!control::parse_scope("").has_value());
    REQUIRE_EQ(control::scope_name(control::scope::monitor_data),
               std::string_view("monitor.data"));
    REQUIRE(control::scope_names(control::role_monitor()) ==
            std::vector<std::string>{"monitor"});
}

TEST_CASE("control policy: the roles are what the design says")
{
    using control::scope;
    REQUIRE(control::role_monitor() == control::scope_set{scope::monitor});
    REQUIRE(!control::role_monitor().contains(scope::monitor_data));
    REQUIRE(control::role_control() ==
            (control::scope_set{scope::monitor, scope::monitor_data,
                                scope::control_ports, scope::control_data,
                                scope::control_faults, scope::record}));
    REQUIRE(!control::role_control().contains(scope::control_apply));
    REQUIRE(!control::role_control().contains(scope::admin));
    REQUIRE(control::role_control_apply().contains(scope::control_apply));
    REQUIRE(!control::role_control_apply().contains(scope::admin));
    for (const scope value : control::all_scope_values)
    {
        REQUIRE(control::every_scope().contains(value));
    }
}

TEST_CASE("control policy: admin survives only on the administration "
          "endpoint")
{
    using control::scope;
    for (const control::scope_set granted : every_scope_set())
    {
        for (const control::transport via : all_transports)
        {
            const control::scope_set got =
                control::effective_scopes(granted, via, control::every_scope());
            // Never more than was granted.
            REQUIRE(granted.contains_all(got));
            if (via == control::transport::local_admin)
            {
                REQUIRE(got == granted);
            }
            else
            {
                REQUIRE(!got.contains(scope::admin));
                REQUIRE(got == granted.without(scope::admin));
            }
            // A ceiling only ever takes away.
            const control::scope_set capped = control::effective_scopes(
                granted, via, control::role_monitor());
            REQUIRE(control::role_monitor().contains_all(capped));
            REQUIRE(got.contains_all(capped));
        }
    }
}

TEST_CASE("control policy: every operation has a row, a name and a scope")
{
    const auto table = control::operations();
    REQUIRE_EQ(table.size(), expected_operations.size());
    std::set<std::string_view> names;
    for (const expected_row& expected : expected_operations)
    {
        const control::operation_info& row = control::info_of(expected.id);
        REQUIRE(row.id == expected.id);
        REQUIRE_EQ(row.name, expected.name);
        REQUIRE(row.required == expected.required);
        REQUIRE(names.insert(row.name).second);
        REQUIRE(control::find_operation(expected.name) == &row);
    }
    // Only `hello` (how a caller becomes a principal) and the two that show
    // and change nothing of the ports, `ping` and `session.close`, name no
    // scope.
    for (const control::operation_info& row : table)
    {
        if (row.id != control::operation::hello &&
            row.id != control::operation::ping &&
            row.id != control::operation::session_close)
        {
            REQUIRE(row.required.has_value());
        }
    }
    // Neither of the two is audited as a change: `session.close` is audited
    // as the end of a session.
    REQUIRE(!control::info_of(control::operation::ping).mutating);
    REQUIRE(!control::info_of(control::operation::session_close).mutating);
    for (const control::operation reading :
         {control::operation::ports_get, control::operation::links_list,
          control::operation::links_get})
    {
        REQUIRE(!control::info_of(reading).mutating);
    }
    // What stops the gateway or changes who may connect is recorded each
    // time.
    REQUIRE(control::info_of(control::operation::endpoint_stop).mutating);
    REQUIRE(control::info_of(control::operation::tokens_create).mutating);
    REQUIRE(control::info_of(control::operation::tokens_revoke).mutating);
    REQUIRE(!control::info_of(control::operation::tokens_list).mutating);
    REQUIRE(control::info_of(control::operation::sessions_kick).mutating);
    REQUIRE(!control::info_of(control::operation::sessions_list).mutating);
    for (const control::operation changing :
         {control::operation::ports_create, control::operation::ports_remove,
          control::operation::links_connect, control::operation::ports_send,
          control::operation::links_disconnect,
          control::operation::ports_rename})
    {
        REQUIRE(control::info_of(changing).mutating);
    }
    REQUIRE(control::find_operation("ports.settings") == nullptr);
    REQUIRE(control::find_operation("") == nullptr);
}

TEST_CASE("control policy: the whole matrix of caller and operation")
{
    // Every operation, every set of scopes, every transport: 29 * 256 * 4
    // decisions, each compared with the rule written here by hand.
    std::size_t allowed_count = 0;
    for (const expected_row& expected : expected_operations)
    {
        const control::operation_info& wanted = control::info_of(expected.id);
        for (const control::scope_set held : every_scope_set())
        {
            for (const control::transport via : all_transports)
            {
                control::principal caller;
                caller.id = "test";
                caller.via = via;
                caller.scopes = held;
                const control::decision got =
                    control::authorise(caller, wanted);

                const std::optional<control::scope> needed = expected.required;
                bool should = false;
                if (!needed.has_value())
                    should = true;
                else if (std::to_underlying(needed.value()) ==
                         std::to_underlying(control::scope::admin))
                    should = via == control::transport::local_admin &&
                             held.contains(control::scope::admin);
                else
                    should = held.contains(needed.value());

                REQUIRE_EQ(got.allowed, should);
                if (should)
                {
                    REQUIRE(!got.missing.has_value());
                    ++allowed_count;
                }
                else
                {
                    REQUIRE(got.missing == expected.required);
                }
            }
        }
    }
    REQUIRE(allowed_count > 0);
}

TEST_CASE("control policy: the cases a person would ask about")
{
    using control::scope;
    const auto may = [](control::transport via, control::scope_set scopes,
                        control::operation wanted)
    {
        control::principal caller;
        caller.id = "local:test";
        caller.via = via;
        caller.scopes = scopes;
        return control::authorise(caller, control::info_of(wanted)).allowed;
    };
    const auto local = control::transport::local;
    const auto admin = control::transport::local_admin;

    // A monitor reads and cannot stop the gateway.
    REQUIRE(may(local, control::role_monitor(), control::operation::status));
    REQUIRE(
        may(local, control::role_monitor(), control::operation::ports_list));
    REQUIRE(!may(local, control::role_monitor(),
                 control::operation::endpoint_stop));
    // Control is not admin.
    REQUIRE(!may(local, control::role_control_apply(),
                 control::operation::endpoint_stop));
    // A caller that somehow carries the admin scope on the public endpoint,
    // or over a network, is still refused.
    REQUIRE(
        !may(local, control::every_scope(), control::operation::endpoint_stop));
    REQUIRE(!may(control::transport::loopback, control::every_scope(),
                 control::operation::endpoint_stop));
    REQUIRE(!may(control::transport::tls, control::every_scope(),
                 control::operation::endpoint_stop));
    // The administration endpoint may.
    REQUIRE(
        may(admin, control::every_scope(), control::operation::endpoint_stop));
    // Tokens are administration: no scope a token or the public endpoint
    // can hold reaches them.
    for (const control::operation wanted :
         {control::operation::tokens_create, control::operation::tokens_list,
          control::operation::tokens_revoke})
    {
        REQUIRE(!may(local, control::every_scope(), wanted));
        REQUIRE(!may(control::transport::tls, control::every_scope(), wanted));
        REQUIRE(may(admin, control::every_scope(), wanted));
    }
    // No scopes at all: nothing but hello, ping and the end of its own
    // session.
    REQUIRE(!may(local, {}, control::operation::status));
    REQUIRE(!may(local, {}, control::operation::ports_list));
    REQUIRE(!may(local, {}, control::operation::ports_get));
    REQUIRE(!may(local, {}, control::operation::links_list));
    REQUIRE(!may(local, {}, control::operation::links_get));
    REQUIRE(may(local, {}, control::operation::hello));
    REQUIRE(may(local, {}, control::operation::ping));
    REQUIRE(may(local, {}, control::operation::session_close));
    // A monitor reads one port and the links, and writing without reading
    // does not.
    REQUIRE(may(local, control::role_monitor(), control::operation::ports_get));
    REQUIRE(
        may(local, control::role_monitor(), control::operation::links_list));
    REQUIRE(may(local, control::role_monitor(), control::operation::links_get));
    REQUIRE(!may(local, control::scope_set{scope::control_data},
                 control::operation::ports_get));
    // Writing without reading is a set like any other: it reads nothing.
    REQUIRE(!may(local, control::scope_set{scope::control_data},
                 control::operation::ports_list));
}

// -- streams and sessions ----------------------------------------------------

namespace
{

control::server_message round_trip(const control::event_batch& value)
{
    const auto decoded = control::decode_server_message(control::encode(value));
    REQUIRE(decoded);
    return *decoded;
}

control::event_batch sample_batch()
{
    control::event_batch batch;
    batch.seq = 41;
    batch.revision = 12;
    batch.dropped = 3;
    batch.mono_ns = 81234567890ull;
    batch.wall_ms = 1794562200123ull;

    control::event_entry started;
    started.kind = "started";
    started.port = "p_1";
    started.name = "alpha";
    started.mono_ns = 81234000000ull;
    batch.events.push_back(started);

    control::event_entry tapped;
    tapped.kind = "data";
    tapped.port = "p_2";
    tapped.name = "beta";
    tapped.direction = "rx";
    tapped.size = 4;
    tapped.data = vss::to_bytes(std::string_view("\x00\xff\r\n", 4));
    tapped.mono_ns = 81234000001ull;
    batch.events.push_back(tapped);

    control::event_entry counted;
    counted.kind = "data";
    counted.port = "p_2";
    counted.direction = "tx";
    counted.size = 1000000;
    batch.events.push_back(counted);

    control::event_entry more;
    more.kind = "data";
    more.port = "p_3";
    more.direction = "rx";
    more.size = 2;
    more.data = vss::to_bytes("ok");
    batch.events.push_back(more);

    control::event_entry lines;
    lines.kind = "lines";
    lines.port = "p_3";
    lines.lines = 0x2d;
    batch.events.push_back(lines);

    control::event_entry failed;
    failed.kind = "error";
    failed.port = "p_4";
    failed.message = "connection refused";
    failed.error = "engine.io_error";
    batch.events.push_back(failed);

    // About no port, and in more than ASCII.
    control::event_entry logged;
    logged.kind = "log_message";
    logged.message = "d\xc3\xa9j\xc3\xa0 vu";
    batch.events.push_back(logged);
    return batch;
}

} // namespace

TEST_CASE("control proto: the stream and session requests survive the wire")
{
    const control::request events{
        20, control::events_subscribe_request{{50, {"p_1", "p_22"}}}};
    REQUIRE(round_trip(events) == events);
    const control::request traffic{
        21, control::traffic_subscribe_request{{10000, {}}}};
    REQUIRE(round_trip(traffic) == traffic);
    const control::request no_events{22, control::events_unsubscribe_request{}};
    REQUIRE(round_trip(no_events) == no_events);
    const control::request no_traffic{23,
                                      control::traffic_unsubscribe_request{}};
    REQUIRE(round_trip(no_traffic) == no_traffic);
    const control::request listing{24, control::sessions_list_request{}};
    REQUIRE(round_trip(listing) == listing);
    const control::request kick{25,
                                control::sessions_kick_request{
                                    std::numeric_limits<std::uint64_t>::max()}};
    REQUIRE(round_trip(kick) == kick);

    // The four are different operations on the wire.
    std::set<std::string> names;
    for (const control::request& asked :
         {events, traffic, no_events, no_traffic})
    {
        const std::string header = control::encode(asked).header;
        REQUIRE(names.insert(header.substr(header.find("\"op\""), 28)).second);
    }
}

TEST_CASE("control proto: a subscription may leave out its time and its ports")
{
    const auto bare = control::decode_request(
        header_frame(R"({"id":1,"op":"events.subscribe"})"));
    REQUIRE(bare);
    const auto& options =
        std::get<control::events_subscribe_request>(bare->body).options;
    REQUIRE_EQ(options.interval_ms, control::default_stream_interval_ms);
    REQUIRE(options.ports.empty());

    const auto timed = control::decode_request(
        header_frame(R"({"id":1,"op":"traffic.subscribe","interval_ms":20})"));
    REQUIRE(timed);
    REQUIRE_EQ(std::get<control::traffic_subscribe_request>(timed->body)
                   .options.interval_ms,
               std::uint32_t{20});

    // Whether a text names a port is the gateway's to say: the decoder keeps
    // it, so the caller gets an error it can read and keeps its connection.
    const auto odd = control::decode_request(header_frame(
        R"({"id":1,"op":"events.subscribe","ports":["COM3","p_1"]})"));
    REQUIRE(odd);
    REQUIRE_EQ(
        std::get<control::events_subscribe_request>(odd->body).options.ports,
        (std::vector<std::string>{"COM3", "p_1"}));
}

TEST_CASE("control proto: a subscription that is not one is refused")
{
    std::string many = R"({"id":1,"op":"traffic.subscribe","ports":[)";
    for (std::size_t index = 0; index <= control::max_port_filter_size; ++index)
        many += (index == 0 ? "\"p_" : ",\"p_") + std::to_string(index) + "\"";
    many += "]}";
    std::string most = R"({"id":1,"op":"traffic.subscribe","ports":[)";
    for (std::size_t index = 0; index < control::max_port_filter_size; ++index)
        most += (index == 0 ? "\"p_" : ",\"p_") + std::to_string(index) + "\"";
    most += "]}";
    REQUIRE(control::decode_request(header_frame(most)));

    const std::array bad{
        many,
        std::string(R"({"id":1,"op":"events.subscribe","interval_ms":-1})"),
        std::string(R"({"id":1,"op":"events.subscribe","interval_ms":"20"})"),
        std::string(
            R"({"id":1,"op":"events.subscribe","interval_ms":4294967296})"),
        std::string(R"({"id":1,"op":"events.subscribe","ports":"p_1"})"),
        std::string(R"({"id":1,"op":"events.subscribe","ports":[1]})"),
        std::string(R"({"id":1,"op":"events.subscribe","ports":null})"),
        std::string(R"({"id":1,"op":"sessions.kick"})"),
        std::string(R"({"id":1,"op":"sessions.kick","session":"3"})"),
        std::string(R"({"id":1,"op":"sessions.kick","session":-3})")};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_request(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
}

TEST_CASE("control proto: the stream and session replies survive the wire")
{
    const control::reply subscribed{
        9, control::subscribed_reply{
               10, std::numeric_limits<std::uint64_t>::max()}};
    REQUIRE(round_trip(subscribed) == subscribed);

    control::sessions_reply listed;
    control::session_entry watcher;
    watcher.number = 7;
    watcher.principal = "token:aaaaaaaaaaaaa";
    watcher.transport = "local";
    watcher.client = "pytest 8.3";
    watcher.program = "/usr/bin/python3";
    watcher.process = 4242;
    watcher.opened_ms = 1794562200123ull;
    watcher.requests = 12;
    watcher.events = true;
    watcher.traffic = true;
    watcher.events_sent = 1000;
    watcher.bytes_sent = 123456;
    watcher.events_dropped = 3;
    listed.sessions.push_back(watcher);
    listed.sessions.push_back(control::session_entry{});
    const control::reply sessions{10, listed};
    REQUIRE(round_trip(sessions) == sessions);
    const control::reply nobody{11, control::sessions_reply{}};
    REQUIRE(round_trip(nobody) == nobody);

    for (const char* const bad :
         {R"({"id":1,"reply":"subscribed"})",
          R"({"id":1,"reply":"subscribed","interval_ms":10})",
          R"({"id":1,"reply":"subscribed","interval_ms":"10","next_seq":1})",
          R"({"id":1,"reply":"sessions"})",
          R"({"id":1,"reply":"sessions","sessions":[{}]})",
          R"({"id":1,"reply":"sessions","sessions":[7]})"})
    {
        REQUIRE(!control::decode_reply(header_frame(bad)));
    }
}

TEST_CASE("control proto: a batch of events survives the wire, bytes and all")
{
    const control::event_batch batch = sample_batch();
    const control::frame wire = control::encode(batch);
    // The bytes are in the blob, in order, and nowhere in the header.
    REQUIRE_EQ(wire.blob.size(), std::size_t{6});
    REQUIRE_EQ(wire.header.find("ok"), std::string::npos);
    const control::server_message got = round_trip(batch);
    REQUIRE(std::holds_alternative<control::event_batch>(got));
    REQUIRE(std::get<control::event_batch>(got) == batch);

    const control::event_batch empty{1, 0, 0, 5, 6, {}};
    REQUIRE(std::get<control::event_batch>(round_trip(empty)) == empty);

    // A batch is not an answer, and an answer is still read as one.
    REQUIRE(!control::decode_reply(wire));
    REQUIRE(!control::decode_request(wire));
    const control::reply done{3, control::done_reply{}};
    const auto answer = control::decode_server_message(control::encode(done));
    REQUIRE(answer);
    REQUIRE(std::get<control::reply>(*answer) == done);
}

TEST_CASE("control proto: a field of an event with nothing to say is left out")
{
    control::event_batch batch;
    batch.seq = 1;
    control::event_entry bare;
    bare.kind = "stopped";
    batch.events.push_back(bare);
    const std::string header = control::encode(batch).header;
    for (const char* const field :
         {"\"port\"", "\"name\"", "\"direction\"", "\"size\"", "\"stored\"",
          "\"lines\"", "\"message\"", "\"error\""})
    {
        REQUIRE_EQ(header.find(field), std::string::npos);
    }
    REQUIRE(header.find("\"kind\":\"stopped\"") != std::string::npos);
    REQUIRE(std::get<control::event_batch>(round_trip(batch)) == batch);
}

TEST_CASE("control proto: a batch that is not one is refused")
{
    const std::string time = R"("time":{"mono_ns":1,"wall_ms":2})";
    const std::string start =
        R"({"event":"batch","seq":1,"revision":0,"dropped":0,)" + time;
    REQUIRE(control::decode_server_message(
        header_frame(start + R"(,"events":[]})")));
    REQUIRE(control::decode_server_message(
        header_frame(start + R"(,"events":[{"kind":"x","new_field":[1]}]})")));

    const std::array bad{
        std::string(R"({"event":"other","seq":1})"),
        std::string(R"({"event":7})"), std::string(R"({"event":"batch"})"),
        // No time, a time that is not an object, half a time.
        std::string(R"({"event":"batch","seq":1,"revision":0,"dropped":0,)"
                    R"("events":[]})"),
        std::string(R"({"event":"batch","seq":1,"revision":0,"dropped":0,)"
                    R"("time":7,"events":[]})"),
        std::string(R"({"event":"batch","seq":1,"revision":0,"dropped":0,)"
                    R"("time":{"mono_ns":1},"events":[]})"),
        start + R"(})", start + R"(,"events":{}})", start + R"(,"events":[7]})",
        start + R"(,"events":[{}]})", start + R"(,"events":[{"kind":7}]})",
        start + R"(,"events":[{"kind":"data","size":-1}]})",
        start + R"(,"events":[{"kind":"data","port":7}]})",
        start + R"(,"events":[{"kind":"data","lines":4294967296}]})",
        start + R"(,"events":[{"kind":"data","mono_ns":"1"}]})",
        // More bytes than were sent.
        start + R"(,"events":[{"kind":"data","stored":1}]})"};
    for (const std::string& header : bad)
    {
        const auto decoded =
            control::decode_server_message(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }

    // The events use the blob in order and all of it: bytes nobody claims,
    // and the same bytes claimed twice, are both a mistake.
    control::frame extra =
        header_frame(start + R"(,"events":[{"kind":"data","stored":1}]})");
    extra.blob = {vss::byte{1}, vss::byte{2}};
    REQUIRE(!control::decode_server_message(extra));
    control::frame twice =
        header_frame(start + R"(,"events":[{"kind":"data","stored":2},)"
                             R"({"kind":"data","stored":2}]})");
    twice.blob = {vss::byte{1}, vss::byte{2}};
    REQUIRE(!control::decode_server_message(twice));
    control::frame exact =
        header_frame(start + R"(,"events":[{"kind":"data","stored":1},)"
                             R"({"kind":"data","stored":1}]})");
    exact.blob = {vss::byte{1}, vss::byte{2}};
    const auto read = control::decode_server_message(exact);
    REQUIRE(read);
    const auto& events = std::get<control::event_batch>(*read).events;
    REQUIRE_EQ(events.at(0).data, (vss::byte_buffer{vss::byte{1}}));
    REQUIRE_EQ(events.at(1).data, (vss::byte_buffer{vss::byte{2}}));
}

TEST_CASE("control proto: a port id is read back only in the form it is "
          "written")
{
    for (const std::uint64_t id :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{42},
          std::numeric_limits<std::uint64_t>::max()})
    {
        const auto read = control::parse_port_id(control::port_id_text(id));
        REQUIRE(read == id);
    }
    for (const char* const bad : {"",
                                  "p",
                                  "p_",
                                  "p_-1",
                                  "p_+1",
                                  "p_ 1",
                                  "p_1 ",
                                  " p_1",
                                  "P_1",
                                  "l_1",
                                  "p_01",
                                  "p_00",
                                  "p_1x",
                                  "p_0x1",
                                  "p_1.0",
                                  "p_18446744073709551616",
                                  "p_99999999999999999999",
                                  "p_184467440737095516150",
                                  "1",
                                  "COM3",
                                  "p__1",
                                  "p_\xd9\xa1"})
    {
        REQUIRE(!control::parse_port_id(bad).has_value());
    }
    using namespace std::string_view_literals;
    REQUIRE(!control::parse_port_id("p_1\0"sv).has_value());
}

TEST_CASE("control proto: an engine error has a public name and no number")
{
    REQUIRE(control::engine_error_name({}).empty());
    std::set<std::string> names;
    for (int value = 1; value <= static_cast<int>(vss::errc::internal); ++value)
    {
        const std::string name = control::engine_error_name(
            vss::make_error_code(static_cast<vss::errc>(value)));
        REQUIRE_EQ(name.rfind("engine.", 0), std::size_t{0});
        REQUIRE(name.size() > 7);
        REQUIRE(names.insert(name).second);
    }
    REQUIRE_EQ(
        control::engine_error_name(vss::make_error_code(vss::errc::not_open)),
        std::string("engine.not_open"));
    // What the operating system said does not cross: not its number, not
    // its text.
    const std::string system = control::engine_error_name(
        vss::error_code(5, boost::system::system_category()));
    REQUIRE_EQ(system, std::string("engine.io_error"));
    REQUIRE_EQ(control::engine_error_name(agent::make_error_code(
                   agent::protocol_errc::connection_closed)),
               std::string("engine.io_error"));
}

TEST_CASE("control policy: a caller that can only look is told from one that "
          "can change things")
{
    using control::scope;
    REQUIRE(control::looks_only({}));
    REQUIRE(control::looks_only(control::role_monitor()));
    REQUIRE(control::looks_only({scope::monitor, scope::monitor_data}));
    REQUIRE(control::looks_only({scope::monitor_data}));
    REQUIRE(!control::looks_only(control::role_control()));
    REQUIRE(!control::looks_only(control::every_scope()));
    for (const control::scope_set held : every_scope_set())
    {
        bool changes = false;
        for (const scope value :
             {scope::control_ports, scope::control_data, scope::control_faults,
              scope::control_apply, scope::record, scope::admin})
        {
            changes = changes || held.contains(value);
        }
        REQUIRE_EQ(control::looks_only(held), !changes);
    }

    // The streams: a monitor gets events, bytes need their own scope, and
    // who is connected is administration.
    const auto may = [](control::transport via, control::scope_set scopes,
                        control::operation wanted)
    {
        control::principal caller;
        caller.id = "local:test";
        caller.via = via;
        caller.scopes = scopes;
        return control::authorise(caller, control::info_of(wanted)).allowed;
    };
    const auto local = control::transport::local;
    REQUIRE(may(local, control::role_monitor(),
                control::operation::events_subscribe));
    REQUIRE(!may(local, control::role_monitor(),
                 control::operation::traffic_subscribe));
    REQUIRE(may(local, {scope::monitor, scope::monitor_data},
                control::operation::traffic_subscribe));
    REQUIRE(!may(local, {scope::monitor_data},
                 control::operation::events_subscribe));
    for (const control::operation wanted :
         {control::operation::sessions_list, control::operation::sessions_kick})
    {
        REQUIRE(!may(local, control::every_scope(), wanted));
        REQUIRE(!may(control::transport::tls, control::every_scope(), wanted));
        REQUIRE(may(control::transport::local_admin, control::every_scope(),
                    wanted));
    }
}

// -- what changes the ports --------------------------------------------------

TEST_CASE("control proto: the requests that change ports survive the wire")
{
    const control::request create{30,
                                  control::ports_create_request{"rig", "pty"}};
    REQUIRE(round_trip(create) == create);
    const control::request plain{31, control::ports_create_request{"rig", ""}};
    REQUIRE(round_trip(plain) == plain);
    const control::request remove{32, control::ports_remove_request{"p_7"}};
    REQUIRE(round_trip(remove) == remove);
    const control::request link{
        33, control::links_connect_request{"p_1", "p_2", true}};
    REQUIRE(round_trip(link) == link);

    control::ports_send_request sending;
    sending.port = "p_1";
    sending.data = vss::to_bytes(std::string_view("\x00\xff\r\n{\"", 6));
    const control::request send{34, sending};
    const control::frame wire = control::encode(send);
    // The bytes are the blob and are not in the header.
    REQUIRE_EQ(wire.blob, sending.data);
    REQUIRE_EQ(wire.header.find("data"), std::string::npos);
    REQUIRE(round_trip(send) == send);

    // The fields that may be left out.
    const auto bare = control::decode_request(
        header_frame(R"({"id":1,"op":"ports.create","name":"a"})"));
    REQUIRE(bare);
    REQUIRE(std::get<control::ports_create_request>(bare->body).kind.empty());
    const auto both_ways = control::decode_request(header_frame(
        R"({"id":1,"op":"links.connect","first":"p_1","second":"p_2"})"));
    REQUIRE(both_ways);
    REQUIRE(!std::get<control::links_connect_request>(both_ways->body).one_way);

    // More bytes than a send may have are still read, so that the gateway
    // can say `limit_exceeded` and the caller keeps its connection.
    control::ports_send_request large;
    large.port = "p_1";
    large.data.assign(control::max_send_size + 1, vss::byte{0x55});
    const control::request too_much{35, large};
    REQUIRE(round_trip(too_much) == too_much);
}

TEST_CASE("control proto: disconnecting a link and renaming a port survive "
          "the wire")
{
    const control::request unlink{36, control::links_disconnect_request{"l_2"}};
    REQUIRE(round_trip(unlink) == unlink);
    const control::request rename{
        37, control::ports_rename_request{"p_3", "bench"}};
    REQUIRE(round_trip(rename) == rename);
    // An id the gateway will not know, and a name as short and as long as a
    // name may be, are still read, so that the gateway answers them.
    for (const char* const odd : {"", "nonsense", "p_1", "l_01"})
    {
        const control::request asking{38,
                                      control::links_disconnect_request{odd}};
        REQUIRE(round_trip(asking) == asking);
    }
    const control::request empty{39, control::ports_rename_request{"p_1", ""}};
    REQUIRE(round_trip(empty) == empty);
    const control::request longest{
        40, control::ports_rename_request{
                "p_1", std::string(control::max_text_size, 'n')}};
    REQUIRE(round_trip(longest) == longest);

    // The wire form: the operation's name and one field for each part.
    REQUIRE_EQ(
        control::encode(unlink).header,
        std::string(R"({"id":36,"op":"links.disconnect","link":"l_2"})"));
    REQUIRE_EQ(
        control::encode(rename).header,
        std::string(
            R"({"id":37,"op":"ports.rename","port":"p_3","name":"bench"})"));
    REQUIRE(control::operation_of(unlink.body) ==
            control::operation::links_disconnect);
    REQUIRE(control::operation_of(rename.body) ==
            control::operation::ports_rename);

    // A name that is not valid UTF-8 is made safe on the way out.
    const control::request odd_name{
        41, control::ports_rename_request{"p_1", "a\xffz"}};
    const auto cleaned = control::decode_request(control::encode(odd_name));
    REQUIRE(cleaned);
    REQUIRE_EQ(std::get<control::ports_rename_request>(cleaned->body).name,
               std::string("a?z"));

    // A field this version does not know is ignored, as for every request.
    const auto later = control::decode_request(header_frame(
        R"({"id":1,"op":"ports.rename","port":"p_1","name":"a","later":1})"));
    REQUIRE(later);
}

TEST_CASE("control proto: a disconnect or a rename that is not one is refused")
{
    const std::string too_long(control::max_text_size + 1, 'n');
    const std::array bad{
        std::string(R"({"id":1,"op":"links.disconnect"})"),
        std::string(R"({"id":1,"op":"links.disconnect","link":3})"),
        std::string(R"({"id":1,"op":"links.disconnect","link":null})"),
        std::string(R"({"id":1,"op":"links.disconnect","link":["l_1"]})"),
        // The field of another operation is not the one.
        std::string(R"({"id":1,"op":"links.disconnect","port":"l_1"})"),
        std::string(R"({"id":1,"op":"links.disconnect","link":")") + too_long +
            "\"}",
        std::string(R"({"id":1,"op":"ports.rename"})"),
        std::string(R"({"id":1,"op":"ports.rename","port":"p_1"})"),
        std::string(R"({"id":1,"op":"ports.rename","name":"a"})"),
        std::string(R"({"id":1,"op":"ports.rename","port":3,"name":"a"})"),
        std::string(R"({"id":1,"op":"ports.rename","port":"p_1","name":3})"),
        std::string(R"({"id":1,"op":"ports.rename","port":"p_1","name":null})"),
        std::string(
            R"({"id":1,"op":"ports.rename","port":"p_1","name":["a"]})"),
        std::string(R"({"id":1,"op":"ports.rename","port":"p_1","name":")") +
            too_long + "\"}",
        std::string(R"({"id":1,"op":"ports.rename","port":")") + too_long +
            R"(","name":"a"})"};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_request(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
    for (const control::request_body& body :
         {control::request_body{control::links_disconnect_request{"l_1"}},
          control::request_body{control::ports_rename_request{"p_1", "a"}}})
    {
        control::frame carried = control::encode(control::request{1, body});
        carried.blob = {vss::byte{1}};
        const auto decoded = control::decode_request(carried);
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
}

TEST_CASE("control proto: only a send carries bytes, and a change that is "
          "not one is refused")
{
    for (const control::request& asked :
         {control::request{1, control::ports_create_request{"a", ""}},
          control::request{1, control::ports_remove_request{"p_1"}},
          control::request{1, control::links_connect_request{"p_1", "p_2"}},
          control::request{1, control::status_request{}}})
    {
        control::frame carried = control::encode(asked);
        carried.blob = {vss::byte{1}};
        const auto decoded = control::decode_request(carried);
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }

    const std::array bad{
        std::string(R"({"id":1,"op":"ports.create"})"),
        std::string(R"({"id":1,"op":"ports.create","name":7})"),
        std::string(R"({"id":1,"op":"ports.create","name":"a","kind":7})"),
        std::string(R"({"id":1,"op":"ports.remove"})"),
        std::string(R"({"id":1,"op":"ports.remove","port":3})"),
        std::string(R"({"id":1,"op":"links.connect","first":"p_1"})"),
        std::string(
            R"({"id":1,"op":"links.connect","first":"p_1","second":2})"),
        std::string(R"({"id":1,"op":"links.connect","first":"p_1",)"
                    R"("second":"p_2","one_way":"yes"})"),
        std::string(R"({"id":1,"op":"ports.send"})"),
        std::string(R"({"id":1,"op":"ports.send","port":null})")};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_request(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
}

TEST_CASE("control proto: an engine error crosses by name and comes back as "
          "the engine's own code")
{
    const control::reply made{4, control::created_reply{"p_12"}};
    REQUIRE(round_trip(made) == made);
    REQUIRE(
        !control::decode_reply(header_frame(R"({"id":1,"reply":"created"})")));
    REQUIRE(!control::decode_reply(
        header_frame(R"({"id":1,"reply":"created","created":7})")));

    for (int value = 1; value <= static_cast<int>(vss::errc::internal); ++value)
    {
        const vss::error_code engine =
            vss::make_error_code(static_cast<vss::errc>(value));
        const control::api_error wire = control::engine_api_error(engine);
        REQUIRE_EQ(wire.code, control::engine_error_name(engine));
        REQUIRE(!wire.retryable);
        // The message is the gateway's and the same for every engine error.
        REQUIRE_EQ(wire.message, control::engine_api_error(
                                     vss::make_error_code(vss::errc::not_found))
                                     .message);
        const control::reply refused{9, control::error_reply{wire}};
        REQUIRE(round_trip(refused) == refused);
        REQUIRE_EQ(control::from_api_error(wire), engine);
    }
    // What the operating system said is an I/O error of the engine.
    const control::api_error system = control::engine_api_error(
        vss::error_code(5, boost::system::system_category()));
    REQUIRE_EQ(control::from_api_error(system),
               vss::make_error_code(vss::errc::io_error));
    // A name nobody knows, of either family.
    REQUIRE_EQ(control::from_api_error({"engine.surprise", "", false}),
               code(control::api_errc::internal));
    REQUIRE_EQ(control::from_api_error({"engine.", "", false}),
               code(control::api_errc::internal));
    REQUIRE_EQ(control::from_api_error({"forbidden", "", false}),
               code(control::api_errc::forbidden));
}

// -- waiting for bytes -------------------------------------------------------

TEST_CASE("control proto: an expect.bytes request survives the wire")
{
    control::expect_bytes_request plain;
    plain.port = "p_2";
    plain.pattern = vss::to_bytes(std::string_view("\x00\xffOK\r\n", 6));
    const control::request waiting{40, plain};
    const control::frame wire = control::encode(waiting);
    // The pattern is hex text in the header; with nothing to send there is
    // no blob.
    REQUIRE(wire.header.find("\"pattern\":\"00ff4f4b0d0a\"") !=
            std::string::npos);
    REQUIRE(wire.blob.empty());
    REQUIRE(round_trip(waiting) == waiting);

    control::expect_bytes_request full = plain;
    full.direction = "tx";
    full.timeout_ms = 2500;
    full.send_port = "p_1";
    full.send = vss::to_bytes("AT\r");
    const control::request sending{41, full};
    REQUIRE_EQ(control::encode(sending).blob, full.send);
    REQUIRE(round_trip(sending) == sending);

    // The time may be left out.
    const auto bare = control::decode_request(header_frame(
        R"({"id":1,"op":"expect.bytes","port":"p_1","pattern":"41"})"));
    REQUIRE(bare);
    const auto& read = std::get<control::expect_bytes_request>(bare->body);
    REQUIRE_EQ(read.timeout_ms, control::default_expect_timeout_ms);
    REQUIRE(read.direction.empty());
    REQUIRE(read.send_port.empty());

    const control::reply found{5,
                               control::matched_reply{12, 81234000001ull, 3}};
    REQUIRE(round_trip(found) == found);
}

TEST_CASE("control proto: an expect.bytes that is not one is refused")
{
    const std::string start = R"({"id":1,"op":"expect.bytes","port":"p_1",)";
    const std::array bad{
        std::string(R"({"id":1,"op":"expect.bytes","pattern":"41"})"),
        start + R"("pattern":65})",
        // Hex that is not: odd, upper case, not a digit, a separator.
        start + R"("pattern":"4"})", start + R"("pattern":"4A"})",
        start + R"("pattern":"zz"})", start + R"("pattern":"41 42"})",
        start + R"("pattern":"0x41"})",
        start + R"("pattern":"41","timeout_ms":"1000"})",
        start + R"("pattern":"41","timeout_ms":-1})",
        start + R"("pattern":"41","direction":7})",
        start + R"("pattern":"41","send_port":7})"};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_request(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
    // Bytes with no port to send them to.
    control::frame stray = header_frame(start + R"("pattern":"41"})");
    stray.blob = {vss::byte{1}};
    REQUIRE(!control::decode_request(stray));

    for (
        const char* const reply :
        {R"({"id":1,"reply":"matched"})",
         R"({"id":1,"reply":"matched","offset":1,"mono_ns":2})",
         R"({"id":1,"reply":"matched","offset":"1","mono_ns":2,"events_lost":0})"})
    {
        REQUIRE(!control::decode_reply(header_frame(reply)));
    }
}

// -- runs --------------------------------------------------------------------

TEST_CASE("control proto: the run requests and replies survive the wire")
{
    const control::request begin{50, control::runs_begin_request{"smoke"}};
    REQUIRE(round_trip(begin) == begin);
    const control::request end{51, control::runs_end_request{}};
    REQUIRE(round_trip(end) == end);
    const control::request note{
        52, control::runs_record_request{"fuse blown", "failed",
                                         "no reply to \"AT\"", 1250}};
    REQUIRE(round_trip(note) == note);
    const control::request latest{53, control::report_junit_request{}};
    REQUIRE(round_trip(latest) == latest);
    const control::request named{54, control::report_junit_request{"r_3"}};
    REQUIRE(round_trip(named) == named);

    control::expect_bytes_request labelled;
    labelled.port = "p_1";
    labelled.pattern = vss::to_bytes("x");
    labelled.label = "banner arrives";
    const control::request waiting{55, labelled};
    REQUIRE(round_trip(waiting) == waiting);

    // A record may leave its message and its time out.
    const auto bare = control::decode_request(header_frame(
        R"({"id":1,"op":"runs.record","name":"a","outcome":"passed"})"));
    REQUIRE(bare);

    const control::reply run{
        6, control::run_reply{"r_3", "smoke", false, 5, 1, 1, 1, 61000}};
    REQUIRE(round_trip(run) == run);
    // A report is long, and is not cut or changed on the way.
    control::report_reply report;
    report.xml = "<?xml version=\"1.0\"?>\n<testsuites>" +
                 std::string(200000, 'x') + "\xc3\xa9</testsuites>\n";
    const control::reply document{7, report};
    REQUIRE(round_trip(document) == document);

    for (
        const char* const bad :
        {R"({"id":1,"op":"runs.begin"})",
         R"({"id":1,"op":"runs.begin","name":7})",
         R"({"id":1,"op":"runs.record","name":"a"})",
         R"({"id":1,"op":"runs.record","name":"a","outcome":"passed","duration_ms":"1"})",
         R"({"id":1,"op":"report.junit","run":3})"})
    {
        const auto decoded = control::decode_request(header_frame(bad));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
    for (const char* const bad :
         {R"({"id":1,"reply":"run","run":"r_1","name":"a","open":true})",
          R"({"id":1,"reply":"report"})",
          R"({"id":1,"reply":"report","xml":7})"})
    {
        REQUIRE(!control::decode_reply(header_frame(bad)));
    }
    control::report_reply too_long;
    too_long.xml.assign(control::max_report_size + 1, 'x');
    REQUIRE(
        !control::decode_reply(control::encode(control::reply{8, too_long})));
}

// -- reading one port, the links, ping and the end of a session -------------

TEST_CASE("control proto: the reading requests survive the wire")
{
    const control::request port{60, control::ports_get_request{"p_7"}};
    REQUIRE(round_trip(port) == port);
    const control::request links{61, control::links_list_request{}};
    REQUIRE(round_trip(links) == links);
    const control::request link{62, control::links_get_request{"l_2"}};
    REQUIRE(round_trip(link) == link);
    const control::request ping{63, control::ping_request{}};
    REQUIRE(round_trip(ping) == ping);
    const control::request close{64, control::session_close_request{}};
    REQUIRE(round_trip(close) == close);

    // The wire form: the operation's name and the id in its own field.
    REQUIRE_EQ(control::encode(port).header,
               std::string(R"({"id":60,"op":"ports.get","port":"p_7"})"));
    REQUIRE_EQ(control::encode(links).header,
               std::string(R"({"id":61,"op":"links.list"})"));
    REQUIRE_EQ(control::encode(link).header,
               std::string(R"({"id":62,"op":"links.get","link":"l_2"})"));
    REQUIRE_EQ(control::encode(ping).header,
               std::string(R"({"id":63,"op":"ping"})"));
    REQUIRE_EQ(control::encode(close).header,
               std::string(R"({"id":64,"op":"session.close"})"));

    // Each names the operation of the table.
    REQUIRE(control::operation_of(port.body) == control::operation::ports_get);
    REQUIRE(control::operation_of(links.body) ==
            control::operation::links_list);
    REQUIRE(control::operation_of(link.body) == control::operation::links_get);
    REQUIRE(control::operation_of(ping.body) == control::operation::ping);
    REQUIRE(control::operation_of(close.body) ==
            control::operation::session_close);

    // An id the gateway will not know is still read, so that the gateway can
    // answer it and the caller keeps its connection.
    for (const char* const odd : {"", "nonsense", "l_1", "p_01"})
    {
        const control::request asking{65, control::ports_get_request{odd}};
        REQUIRE(round_trip(asking) == asking);
    }
    // The longest text is read, and one byte more is not.
    const control::request longest{66, control::ports_get_request{std::string(
                                           control::max_text_size, 'p')}};
    REQUIRE(round_trip(longest) == longest);
}

TEST_CASE("control proto: a reading request that is not one is refused")
{
    const std::string too_long(control::max_text_size + 1, 'p');
    const std::array bad{
        std::string(R"({"id":1,"op":"ports.get"})"),
        std::string(R"({"id":1,"op":"ports.get","port":3})"),
        std::string(R"({"id":1,"op":"ports.get","port":null})"),
        std::string(R"({"id":1,"op":"ports.get","port":["p_1"]})"),
        // The field of the other operation is not the one.
        std::string(R"({"id":1,"op":"ports.get","link":"l_1"})"),
        std::string(R"({"id":1,"op":"ports.get","port":")") + too_long + "\"}",
        std::string(R"({"id":1,"op":"links.get"})"),
        std::string(R"({"id":1,"op":"links.get","link":3})"),
        std::string(R"({"id":1,"op":"links.get","link":false})"),
        std::string(R"({"id":1,"op":"links.get","port":"p_1"})"),
        std::string(R"({"id":1,"op":"links.get","link":")") + too_long + "\"}"};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_request(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
    // The three that take no field read without them and with a field this
    // version does not know.
    for (const char* const fine :
         {R"({"id":1,"op":"links.list"})", R"({"id":1,"op":"ping"})",
          R"({"id":1,"op":"session.close"})",
          R"({"id":1,"op":"ping","later":{"a":1}})"})
    {
        REQUIRE(control::decode_request(header_frame(fine)));
    }
    // None of the five carries bytes.
    for (const control::request_body& body :
         {control::request_body{control::ports_get_request{"p_1"}},
          control::request_body{control::links_list_request{}},
          control::request_body{control::links_get_request{"l_1"}},
          control::request_body{control::ping_request{}},
          control::request_body{control::session_close_request{}}})
    {
        control::frame carried = control::encode(control::request{1, body});
        carried.blob = {vss::byte{1}};
        const auto decoded = control::decode_request(carried);
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
}

TEST_CASE("control proto: the reading replies survive the wire")
{
    const control::port_entry alpha{"p_1",  "alpha", "/dev/pts/3", "virtual",
                                    "open", true,    10,           20};
    const control::port_entry beta{"p_2",     "beta", "", "mock",
                                   "created", false,  0,  0};
    const control::link_entry duplex{"l_1", "duplex", {"p_1"}, {"p_2"}, 30};
    const control::link_entry fan{"l_2", "one_way", {"p_1"}, {"p_2", "p_3"}, 0};

    const control::reply port_alpha{70, control::port_reply{12, alpha}};
    REQUIRE(round_trip(port_alpha) == port_alpha);
    const control::reply port_beta{71, control::port_reply{0, beta}};
    REQUIRE(round_trip(port_beta) == port_beta);
    const control::reply links{72, control::links_reply{12, {duplex, fan}}};
    REQUIRE(round_trip(links) == links);
    const control::reply no_links{73, control::links_reply{3, {}}};
    REQUIRE(round_trip(no_links) == no_links);
    const control::reply one_link{74, control::link_reply{12, fan}};
    REQUIRE(round_trip(one_link) == one_link);

    // The port of a `port` reply is the object `ports` has for it, so a
    // reader of either reads the other's.
    const control::ports_reply listed{12, {alpha, beta}, {duplex, fan}};
    const auto from_list =
        control::decode_reply(control::encode(control::reply{75, listed}));
    REQUIRE(from_list);
    const auto& ports = std::get<control::ports_reply>(from_list->body);
    REQUIRE(ports.ports.at(0) ==
            std::get<control::port_reply>(round_trip(port_alpha).body).port);
    REQUIRE(ports.links.at(1) ==
            std::get<control::link_reply>(round_trip(one_link).body).link);

    // The wire form, for a reader that is not this library.
    REQUIRE_EQ(control::encode(one_link).header,
               std::string(R"({"id":74,"reply":"link","revision":12,"link":)"
                           R"({"id":"l_2","mode":"one_way","sources":["p_1"],)"
                           R"("targets":["p_2","p_3"],"bytes":0}})"));
    REQUIRE_EQ(control::encode(no_links).header,
               std::string(R"({"id":73,"reply":"links","revision":3,)"
                           R"("links":[]})"));
}

TEST_CASE("control proto: a reading reply that is not one is refused")
{
    const std::string port_object =
        R"({"id":"p_1","name":"a","path":"","kind":"k","state":"s",)"
        R"("open":true,"bytes_rx":1,"bytes_tx":2})";
    const std::string link_object =
        R"({"id":"l_1","mode":"m","sources":["p_1"],"targets":["p_2"],)"
        R"("bytes":3})";
    const std::array bad{
        // A `port` reply without its port, with a port that is not an
        // object, or one that is short of a field.
        std::string(R"({"id":1,"reply":"port","revision":1})"),
        std::string(R"({"id":1,"reply":"port","revision":1,"port":"p_1"})"),
        std::string(R"({"id":1,"reply":"port","revision":1,"port":[]})"),
        std::string(R"({"id":1,"reply":"port","revision":1,"port":{)"
                    R"("id":"p_1"}})"),
        std::string(R"({"id":1,"reply":"port","port":)") + port_object + "}",
        std::string(R"({"id":1,"reply":"port","revision":-1,"port":)") +
            port_object + "}",
        std::string(R"({"id":1,"reply":"port","revision":"1","port":)") +
            port_object + "}",
        // A `links` reply whose links are not a list, or hold what is not a
        // link.
        std::string(R"({"id":1,"reply":"links","revision":1})"),
        std::string(R"({"id":1,"reply":"links","revision":1,"links":{}})"),
        std::string(R"({"id":1,"reply":"links","revision":1,"links":"l_1"})"),
        std::string(R"({"id":1,"reply":"links","revision":1,"links":[7]})"),
        std::string(R"({"id":1,"reply":"links","revision":1,"links":[{)"
                    R"("id":"l_1"}]})"),
        std::string(R"({"id":1,"reply":"links","links":[]})"),
        // A `link` reply without its link, or with the port's object.
        std::string(R"({"id":1,"reply":"link","revision":1})"),
        std::string(R"({"id":1,"reply":"link","revision":1,"link":null})"),
        std::string(R"({"id":1,"reply":"link","revision":1,"link":)") +
            port_object + "}",
        std::string(R"({"id":1,"reply":"link","link":)") + link_object + "}"};
    for (const std::string& header : bad)
    {
        const auto decoded = control::decode_reply(header_frame(header));
        REQUIRE(!decoded);
        REQUIRE_EQ(decoded.error(), code(control::api_errc::invalid_argument));
    }
    // The good ones, from the same pieces.
    REQUIRE(control::decode_reply(header_frame(
        R"({"id":1,"reply":"port","revision":1,"port":)" + port_object + "}")));
    REQUIRE(control::decode_reply(header_frame(
        R"({"id":1,"reply":"link","revision":1,"link":)" + link_object + "}")));
    REQUIRE(control::decode_reply(
        header_frame(R"({"id":1,"reply":"links","revision":1,"links":[)" +
                     link_object + "]}")));
}

TEST_CASE("control proto: a link id is read back only in the form it is "
          "written")
{
    for (const std::uint64_t id :
         {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{42},
          std::numeric_limits<std::uint64_t>::max()})
    {
        const auto read = control::parse_link_id(control::link_id_text(id));
        REQUIRE(read == id);
    }
    for (const char* const bad :
         {"", "l", "l_", "l_-1", "l_+1", "l_ 1", "l_1 ", " l_1", "L_1", "p_1",
          "l_01", "l_1x", "l_18446744073709551616", "1", "l__1", "l_\xd9\xa1"})
    {
        REQUIRE(!control::parse_link_id(bad).has_value());
    }
    // A port's id is not a link's, and a link's is not a port's.
    REQUIRE(!control::parse_link_id(control::port_id_text(1)).has_value());
    REQUIRE(!control::parse_port_id(control::link_id_text(1)).has_value());
}
