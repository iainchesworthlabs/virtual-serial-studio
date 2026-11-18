//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The debug server over its real pipe or socket: the approval, the scopes, the
// closed list of operations, the traffic switch, the live trace, the audit
// line of each request, the redaction of every reply, and what an unwelcome
// connection learns.  Every server here has its own instance name and its own
// recorder, so none meets Studio's or another test's.
//

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/debug/client.hpp>
#include <vss/debug/server.hpp>
#include <vss/debug/studio_provider.hpp>
#include <vss/studio/studio.hpp>
#include <vss/testkit/harness.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace dbg = vss::debug;
namespace json = boost::json;
namespace agent = vss::agent;
using namespace std::chrono_literals;

namespace
{

std::string unique_instance()
{
    static const unsigned random = std::random_device{}();
    static std::atomic<unsigned> counter{0};
    std::ostringstream out;
    out << "dbg-" << std::hex << random << '-' << std::dec
        << counter.fetch_add(1);
    return out.str();
}

std::string read_text(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// The text of every file in a directory, joined.
std::string read_directory(const std::filesystem::path& directory)
{
    std::string out;
    std::error_code ignored;
    for (const auto& item :
         std::filesystem::directory_iterator(directory, ignored))
        out += read_text(item.path());
    return out;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

/// A provider that answers with what a test gives it.
class fixed_provider : public dbg::studio_provider
{
public:
    using studio_provider::studio_provider;

    std::string leak;

    dbg::op_result plugins() override { return dbg::op_result::done(leak); }
};

/// A studio with two loopback ports, a recorder of its own, and a server whose
/// person is a function the test sets.
struct rig
{
    explicit rig(dbg::scope_set grant = dbg::bit(dbg::scope::read) |
                                        dbg::bit(dbg::scope::act) |
                                        dbg::bit(dbg::scope::data))
        : granted(grant)
    {
        first =
            app.create_virtual_port("alpha", vss::virtual_port_kind::loopback)
                .value();
        second =
            app.create_virtual_port("beta", vss::virtual_port_kind::loopback)
                .value();
        provider = std::make_shared<fixed_provider>(app, &log);
        start();
    }

    void start(std::chrono::milliseconds traffic_window = 10min)
    {
        dbg::server_options options;
        options.instance = unique_instance();
        options.source = provider;
        options.recorder = &log;
        options.audit_directory = scratch.path() / "audit";
        options.instance_directory = scratch.path() / "instances";
        options.traffic_window = traffic_window;
        options.hello_deadline = 600ms;
        options.max_requests_per_second = 40;
        options.redact =
            dbg::redactor::for_user("C:\\Users\\tester", "tester", "TEST-PC");
        options.approve = [this](const dbg::client_request& request,
                                 std::function<void(dbg::approval)> answer)
        {
            ++asked;
            last_name = request.name;
            last_program = request.program;
            last_wanted = request.wanted;
            if (!person_answers.load())
                return;
            dbg::approval reply;
            reply.granted = granted.load();
            reply.remember = remember.load();
            answer(reply);
        };
        auto started = dbg::server::start(std::move(options));
        REQUIRE(started.has_value());
        server = std::move(started).value();
    }

    std::unique_ptr<dbg::client>
    connect(dbg::scope_set scopes = dbg::bit(dbg::scope::read))
    {
        dbg::client_options options;
        options.instance = server->instance();
        options.scopes = scopes;
        options.timeout = 10s;
        auto made = dbg::client::connect(options);
        REQUIRE(made.has_value());
        return std::move(made).value();
    }

    vss_test_support::scratch_directory scratch;
    vss::studio app;
    std::shared_ptr<vss::endpoint> first;
    std::shared_ptr<vss::endpoint> second;
    vss::trace::recorder log;
    std::shared_ptr<fixed_provider> provider;
    std::unique_ptr<dbg::server> server;
    std::atomic<int> asked{0};
    std::atomic<dbg::scope_set> granted;
    std::atomic<bool> remember{false};
    std::atomic<bool> person_answers{true};
    std::string last_name;
    std::string last_program;
    dbg::scope_set last_wanted = 0;
};

json::object parse_object(const std::string& text)
{
    boost::system::error_code ec;
    json::value value = json::parse(text, ec);
    REQUIRE(!ec);
    REQUIRE(value.is_object());
    return value.as_object();
}

} // namespace

TEST_CASE("debug server: an approved client is told what it was granted")
{
    rig world(dbg::bit(dbg::scope::read));
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::act));
    // The person gave reading only.
    REQUIRE_EQ(session->granted(), dbg::bit(dbg::scope::read));
    REQUIRE_EQ(world.asked.load(), 1);
    REQUIRE_EQ(world.last_name, std::string("vss_cli"));
    REQUIRE_EQ(world.last_wanted,
               dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::act));
}

TEST_CASE("debug server: a refused client gets nothing and the audit says so")
{
    rig world(0);
    dbg::client_options options;
    options.instance = world.server->instance();
    options.timeout = 5s;
    const auto refused = dbg::client::connect(options);
    REQUIRE(!refused.has_value());
    REQUIRE_EQ(refused.error(),
               vss::make_error_code(vss::errc::permission_denied));
    REQUIRE(world.server->clients().empty());

    const std::string audit = read_directory(world.scratch.path() / "audit");
    REQUIRE(contains(audit, "debug.session.denied"));
}

TEST_CASE("debug server: a person who never answers is a refusal")
{
    rig world;
    world.person_answers = false;
    dbg::client_options options;
    options.instance = world.server->instance();
    options.timeout = 5s;
    // The server's approval timeout is a minute; shorten it with a new server.
    world.server->stop();
    dbg::server_options again;
    again.instance = unique_instance();
    again.source = world.provider;
    again.recorder = &world.log;
    again.approval_timeout = 300ms;
    again.approve = [](const dbg::client_request&,
                       std::function<void(dbg::approval)>) {};
    auto started = dbg::server::start(std::move(again));
    REQUIRE(started.has_value());
    options.instance = (*started)->instance();
    REQUIRE(!dbg::client::connect(options).has_value());
}

TEST_CASE("debug server: reading is not acting")
{
    rig world(dbg::bit(dbg::scope::read));
    auto session = world.connect(dbg::bit(dbg::scope::read));
    const auto tried =
        session->call("debug.run", R"({"op":"stop_port","port":"alpha"})");
    REQUIRE(tried.has_value());
    REQUIRE(!tried->ok);
    REQUIRE_EQ(tried->code, std::string("forbidden"));

    const auto shot = session->call("debug.gui.screenshot");
    REQUIRE(shot.has_value());
    REQUIRE(!shot->ok);
    REQUIRE_EQ(shot->code, std::string("forbidden"));

    const auto traffic = session->call("debug.traffic");
    REQUIRE(traffic.has_value());
    REQUIRE_EQ(traffic->code, std::string("forbidden"));
}

TEST_CASE("debug server: an operation that is not on the list is refused")
{
    rig world;
    auto session = world.connect();
    const auto tried = session->call("debug.format_disk");
    REQUIRE(tried.has_value());
    REQUIRE(!tried->ok);
    REQUIRE_EQ(tried->code, std::string("unsupported"));
}

TEST_CASE("debug server: the snapshot lists every port and its counters")
{
    rig world;
    auto session = world.connect();
    const auto got = session->call("debug.snapshot");
    REQUIRE(got.has_value());
    REQUIRE(got->ok);
    const auto body = parse_object(got->json);
    const auto& endpoints = body.at("endpoints").as_array();
    REQUIRE_EQ(endpoints.size(), std::size_t{2});
    REQUIRE(endpoints[0].as_object().contains("counters"));
    REQUIRE(endpoints[0].as_object().contains("state"));
    REQUIRE(endpoints[0].as_object().contains("settings"));
    REQUIRE(body.contains("links"));
    REQUIRE(body.contains("recorder"));
}

TEST_CASE("debug server: an operation a provider lacks says it is unsupported")
{
    rig world(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::gui));
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::gui));
    const auto got = session->call("debug.gui.state");
    REQUIRE(got.has_value());
    REQUIRE(!got->ok);
    REQUIRE_EQ(got->code, std::string("unsupported"));
}

TEST_CASE("debug server: the trace shows the failing call and its raw code")
{
    rig world;
    {
        vss::trace::call_scope scope("CreateFileW", world.first->id(), "COM5",
                                     world.log);
        scope.fail(
            boost::system::error_code(5, boost::system::system_category()));
    }
    vss::trace::call_scope("ReadFile", world.first->id(), "", world.log).ok();

    auto session = world.connect();
    const auto all = session->call("debug.trace");
    REQUIRE(all.has_value());
    REQUIRE(all->ok);
    const auto body = parse_object(all->json);
    REQUIRE_EQ(body.at("entries").as_array().size(), std::size_t{2});

    const auto failed =
        session->call("debug.trace", R"({"failures_only":true})");
    REQUIRE(failed.has_value());
    const auto entries = parse_object(failed->json).at("entries").as_array();
    REQUIRE_EQ(entries.size(), std::size_t{1});
    const auto& entry = entries[0].as_object();
    REQUIRE_EQ(std::string(entry.at("call").as_string()),
               std::string("CreateFileW"));
    REQUIRE_EQ(entry.at("code").as_int64(), std::int64_t{5});
    REQUIRE_EQ(std::string(entry.at("result").as_string()),
               std::string("failed"));
}

TEST_CASE("debug server: a live trace delivers entries as they are recorded")
{
    rig world;
    auto session = world.connect();
    const auto started = session->call("debug.trace.subscribe");
    REQUIRE(started.has_value());
    REQUIRE(started->ok);

    vss::trace::call_scope("SetCommState", 1, "COM5", world.log).ok();
    const auto event = session->next_event(10s);
    REQUIRE(event.has_value());
    const auto body = parse_object(*event);
    REQUIRE_EQ(std::string(body.at("event").as_string()), std::string("trace"));
    const auto& entries = body.at("entries").as_array();
    REQUIRE_EQ(entries.size(), std::size_t{1});
    REQUIRE_EQ(std::string(entries[0].as_object().at("call").as_string()),
               std::string("SetCommState"));

    const auto stopped = session->call("debug.trace.unsubscribe");
    REQUIRE(stopped.has_value());
    REQUIRE(stopped->ok);
    vss::trace::call_scope("Later", 1, "", world.log).ok();
    REQUIRE(!session->next_event(400ms).has_value());
}

TEST_CASE("debug server: run returns the whole error chain")
{
    rig world;
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::act));
    const auto missing = session->call(
        "debug.run", R"({"op":"stop_port","port":"no-such-port"})");
    REQUIRE(missing.has_value());
    REQUIRE(missing->ok);
    const auto body = parse_object(missing->json);
    REQUIRE(!body.at("ok").as_bool());
    REQUIRE(body.at("error").as_object().contains("message"));

    const auto added = session->call(
        "debug.run", R"({"op":"add_monitor","from":"alpha","to":"beta"})");
    REQUIRE(added.has_value());
    const auto done = parse_object(added->json);
    REQUIRE(done.at("ok").as_bool());
    REQUIRE(done.contains("calls"));
    REQUIRE(done.contains("duration_ms"));
}

TEST_CASE("debug server: a bad run says what is wrong and changes nothing")
{
    rig world;
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::act));
    const auto bad = session->call("debug.run", R"({"op":"format_disk"})");
    REQUIRE(bad.has_value());
    REQUIRE(!bad->ok);
    REQUIRE_EQ(bad->code, std::string("invalid_argument"));
    const auto none = session->call("debug.run", R"({})");
    REQUIRE(!none->ok);
}

TEST_CASE("debug server: traffic needs the person's second switch")
{
    rig world;
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::data));
    const auto off = session->call("debug.traffic");
    REQUIRE(off.has_value());
    REQUIRE(!off->ok);
    REQUIRE_EQ(off->code, std::string("forbidden"));

    world.server->set_traffic(true);
    REQUIRE(world.server->traffic_on());
    const auto on = session->call("debug.traffic");
    REQUIRE(on.has_value());
    REQUIRE(on->ok);

    world.server->set_traffic(false);
    const auto again = session->call("debug.traffic");
    REQUIRE(!again->ok);
}

TEST_CASE("debug server: the traffic switch turns itself off")
{
    rig world;
    world.server->stop();
    world.start(150ms);
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::data));
    world.server->set_traffic(true);
    REQUIRE(vss::testkit::wait_until(
        [&] { return !world.server->traffic_on(); }, 10s));
    REQUIRE(!session->call("debug.traffic")->ok);
}

TEST_CASE("debug server: traffic carries bytes only when the switch is on")
{
    rig world;
    world.first->write(vss::as_bytes("hello"));
    REQUIRE(vss::testkit::wait_until(
        [&] { return !world.app.engine().traffic(0).empty(); }, 5s));

    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::data));
    // Off: no bytes in the snapshot, the trace or anywhere else.
    const auto snapshot = session->call("debug.snapshot");
    REQUIRE(!contains(snapshot->json, "68656c6c6f"));
    const auto trace = session->call("debug.trace");
    REQUIRE(!contains(trace->json, "68656c6c6f"));

    world.server->set_traffic(true);
    const auto traffic = session->call("debug.traffic");
    REQUIRE(traffic->ok);
    REQUIRE(contains(traffic->json, "68656c6c6f"));
}

TEST_CASE("debug server: every string is redacted before it leaves")
{
    rig world;
    world.provider->leak =
        R"({"path":"C:\\Users\\tester\\setup.json","host":"TEST-PC",)"
        R"("url":"tcp://u:secretword@10.0.0.1:99","nested":{"list":["tester"]}})";
    auto session = world.connect();
    const auto got = session->call("debug.plugins");
    REQUIRE(got.has_value());
    REQUIRE(got->ok);
    REQUIRE(!contains(got->json, "tester"));
    REQUIRE(!contains(got->json, "TEST-PC"));
    REQUIRE(!contains(got->json, "secretword"));
    REQUIRE(contains(got->json, "<home>"));
}

TEST_CASE("debug server: each request writes an audit line and the chain holds")
{
    rig world;
    auto session = world.connect();
    REQUIRE(session->call("debug.snapshot")->ok);
    REQUIRE(session->call("debug.trace")->ok);
    REQUIRE(session->call("debug.status")->ok);

    const std::string audit = read_directory(world.scratch.path() / "audit");
    REQUIRE(contains(audit, "debug.session.opened"));
    REQUIRE(contains(audit, "\"op\":\"debug.snapshot\""));
    REQUIRE(contains(audit, "\"op\":\"debug.trace\""));
    REQUIRE(contains(audit, "\"op\":\"debug.status\""));
    // The reply is not in the audit log.
    REQUIRE(!contains(audit, "alpha"));

    const auto verdict = session->call("debug.audit.verify");
    REQUIRE(verdict.has_value());
    REQUIRE(verdict->ok);
    REQUIRE(contains(verdict->json, "\"intact\":true"));
}

TEST_CASE("debug server: a refused request is audited with its code")
{
    rig world(dbg::bit(dbg::scope::read));
    auto session = world.connect();
    REQUIRE(!session->call("debug.run", R"({"op":"stop_port"})")->ok);
    const std::string audit = read_directory(world.scratch.path() / "audit");
    REQUIRE(contains(audit, "\"op\":\"debug.run\""));
    REQUIRE(contains(audit, "\"code\":\"forbidden\""));
}

TEST_CASE("debug server: a program that was let in is not asked again")
{
    rig world;
    world.remember = true;
    {
        auto first = world.connect();
        REQUIRE(first->call("debug.status")->ok);
    }
    const int before = world.asked.load();
    {
        auto second = world.connect();
        REQUIRE(second->call("debug.status")->ok);
    }
    if (world.last_program.empty())
        VSS_SKIP("this system does not say which program connected");
    REQUIRE_EQ(world.asked.load(), before);

    world.server->disconnect_all();
    auto third = world.connect();
    REQUIRE_EQ(world.asked.load(), before + 1);
}

TEST_CASE("debug server: the instance file exists while it runs and not after")
{
    rig world;
    const auto file = world.scratch.path() / "instances" /
                      (world.server->instance() + ".json");
    REQUIRE(std::filesystem::exists(file));
    const auto found = dbg::find_instances(world.scratch.path() / "instances");
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].instance, world.server->instance());

    const std::string instance = world.server->instance();
    world.server->stop();
    REQUIRE(!std::filesystem::exists(file));

    // And nothing listens: a client finds no server.
    dbg::client_options options;
    options.instance = instance;
    options.timeout = 2s;
    const auto late = dbg::client::connect(options);
    REQUIRE(!late.has_value());
    REQUIRE_EQ(late.error(),
               agent::make_error_code(agent::protocol_errc::not_running));
}

TEST_CASE("debug server: a connection that says nothing is closed")
{
    rig world;
    boost::asio::io_context io;
    auto stream =
        agent::connect_local(io.get_executor(), agent::local_endpoint::debug,
                             world.server->instance());
    REQUIRE(stream.has_value());

    std::array<vss::byte, 64> buffer{};
    boost::system::error_code failed;
    bool done = false;
    (*stream)->async_read_some(
        boost::asio::buffer(buffer),
        [&](const boost::system::error_code& ec, std::size_t)
        {
            failed = ec;
            done = true;
        });
    io.run_for(10s);
    // The server closed it without sending a byte.
    REQUIRE(done);
    REQUIRE(static_cast<bool>(failed));
}

TEST_CASE("debug server: a large frame before hello closes the connection")
{
    rig world;
    boost::asio::io_context io;
    auto stream =
        agent::connect_local(io.get_executor(), agent::local_endpoint::debug,
                             world.server->instance());
    REQUIRE(stream.has_value());

    agent::frame big;
    big.header =
        "{\"op\":\"hello\",\"pad\":\"" + std::string(6000, 'x') + "\"}";
    const auto bytes = agent::encode_frame(big);
    REQUIRE(bytes.has_value());
    bool wrote = false;
    (*stream)->async_write_some(
        boost::asio::buffer(*bytes),
        [&](const boost::system::error_code&, std::size_t) { wrote = true; });

    std::array<vss::byte, 64> buffer{};
    bool done = false;
    boost::system::error_code failed;
    (*stream)->async_read_some(
        boost::asio::buffer(buffer),
        [&](const boost::system::error_code& ec, std::size_t)
        {
            failed = ec;
            done = true;
        });
    io.run_for(10s);
    (void)wrote;
    REQUIRE(done);
    REQUIRE(static_cast<bool>(failed));
    REQUIRE_EQ(world.asked.load(), 0);
}

TEST_CASE("debug server: a client that asks too fast is slowed, not served")
{
    rig world;
    auto session = world.connect();
    int refused = 0;
    for (int index = 0; index < 120; ++index)
    {
        const auto got = session->call("debug.status");
        REQUIRE(got.has_value());
        if (!got->ok && got->code == "limit_exceeded")
            ++refused;
    }
    REQUIRE(refused > 0);
}

TEST_CASE("debug server: stopping closes the clients")
{
    rig world;
    auto session = world.connect();
    REQUIRE(session->call("debug.status")->ok);
    world.server->stop();
    const auto after = session->call("debug.status");
    REQUIRE(!after.has_value());
    REQUIRE(world.server->clients().empty());
}

TEST_CASE("debug server: the clients are listed while they are connected")
{
    rig world;
    auto session = world.connect();
    REQUIRE(session->call("debug.status")->ok);
    const auto listed = world.server->clients();
    REQUIRE_EQ(listed.size(), std::size_t{1});
    REQUIRE_EQ(listed[0].name, std::string("vss_cli"));
    REQUIRE_EQ(listed[0].granted, dbg::bit(dbg::scope::read));
}

TEST_CASE("debug server: two servers on one computer do not meet")
{
    rig first;
    rig second;
    REQUIRE_NE(first.server->instance(), second.server->instance());
    auto one = first.connect();
    auto two = second.connect();
    REQUIRE(one->call("debug.status")->ok);
    REQUIRE(two->call("debug.status")->ok);
}

TEST_CASE("debug server: a server needs a provider and a valid name")
{
    dbg::server_options options;
    options.instance = unique_instance();
    REQUIRE(!dbg::server::start(options).has_value());
}

TEST_CASE("debug server: every operation in the closed list runs and answers")
{
    rig world;
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::act));
    const std::vector<std::string> runs{
        R"({"op":"reopen","port":"alpha"})",
        R"({"op":"stop_port","port":"alpha"})",
        R"({"op":"start_port","port":"alpha"})",
        R"({"op":"restart_port","port":"beta"})",
        R"({"op":"restart_port","port":"beta","settings":{"baud_rate":9600,"data_bits":7,"parity":"even"}})",
        R"({"op":"set_com_number","port":"alpha","number":12})",
        R"({"op":"set_com_number","port":"alpha","number":0})",
        R"({"op":"check_listen_free","uri":"tcp://127.0.0.1:1"})",
        R"({"op":"check_listen_free"})",
        R"({"op":"add_monitor","from":"alpha"})",
        R"({"op":"stop_port","port":99999})",
        R"({"op":"stop_port"})"};
    for (const std::string& args : runs)
    {
        const auto got = session->call("debug.run", args);
        REQUIRE(got.has_value());
        // Either the operation ran, and its own result says how it went, or
        // the request was refused for what is wrong with it.
        if (got->ok)
            REQUIRE(parse_object(got->json).contains("ok"));
        else
            REQUIRE_EQ(got->code, std::string("invalid_argument"));
    }
    const auto bad = session->call("debug.run", "[1]");
    REQUIRE(bad.has_value());
}

TEST_CASE("debug server: the COM picture, the driver and the audit answer")
{
    rig world;
    auto session = world.connect();
    const auto com = session->call("debug.com_numbers");
    REQUIRE(com.has_value());
    if (com->ok)
        REQUIRE(parse_object(com->json).contains("numbers"));
    else
        REQUIRE_EQ(com->code, std::string("unsupported"));

    const auto driver = session->call("debug.driver");
    REQUIRE(driver.has_value());
    REQUIRE(driver->ok);
    REQUIRE(parse_object(driver->json).contains("last_failure"));
    REQUIRE(parse_object(driver->json).contains("ports"));
}

TEST_CASE(
    "debug server: the snapshot shows the queue of a port that was written")
{
    rig world;
    auto session = world.connect();
    const auto got = session->call("debug.snapshot");
    REQUIRE(got->ok);
    const auto body = parse_object(got->json);
    REQUIRE(body.at("hub").as_object().contains("dispatch_queue_depth"));
    const auto& first = body.at("endpoints").as_array()[0].as_object();
    REQUIRE(first.at("write_queue").as_object().contains("pending_writes"));
}

TEST_CASE("debug server: a failing call of a port shows in that port's row")
{
    rig world;
    {
        vss::trace::call_scope scope("SetCommState", world.first->id(), "COM5",
                                     world.log);
        scope.fail(
            boost::system::error_code(5, boost::system::system_category()));
    }
    auto session = world.connect();
    const auto got = session->call("debug.snapshot");
    const auto body = parse_object(got->json);
    bool found = false;
    for (const auto& item : body.at("endpoints").as_array())
    {
        const auto& row = item.as_object();
        if (row.at("id").to_number<vss::endpoint_id>() != world.first->id())
            continue;
        found = row.contains("last_failure") &&
                row.at("last_failure").as_object().at("code").as_int64() == 5;
    }
    REQUIRE(found);
}

TEST_CASE("debug server: traffic filters by port and limits the count")
{
    rig world;
    world.first->write(vss::as_bytes("one"));
    REQUIRE(vss::testkit::wait_until(
        [&] { return !world.app.engine().traffic(0).empty(); }, 5s));
    world.server->set_traffic(true);
    auto session =
        world.connect(dbg::bit(dbg::scope::read) | dbg::bit(dbg::scope::data));
    const auto none =
        session->call("debug.traffic", R"({"endpoints":[99999],"limit":5})");
    REQUIRE(none->ok);
    REQUIRE(!contains(none->json, "6f6e65"));
    const auto some =
        session->call("debug.traffic", "{\"endpoints\":[" +
                                           std::to_string(world.first->id()) +
                                           "],\"limit\":5}");
    REQUIRE(some->ok);
}

TEST_CASE("debug server: a hello for another protocol version is refused")
{
    rig world;
    boost::asio::io_context io;
    auto stream =
        agent::connect_local(io.get_executor(), agent::local_endpoint::debug,
                             world.server->instance());
    REQUIRE(stream.has_value());
    agent::frame hello;
    hello.header = R"({"id":1,"op":"hello","version":99,"client":"x"})";
    const auto bytes = agent::encode_frame(hello);
    REQUIRE(bytes.has_value());
    (*stream)->async_write_some(
        boost::asio::buffer(*bytes),
        [](const boost::system::error_code&, std::size_t) {});
    std::array<vss::byte, 512> buffer{};
    std::size_t got = 0;
    (*stream)->async_read_some(boost::asio::buffer(buffer),
                               [&](const boost::system::error_code&,
                                   std::size_t size) { got = size; });
    io.run_for(10s);
    REQUIRE(got > 0);
    const std::string reply(reinterpret_cast<const char*>(buffer.data()), got);
    REQUIRE(contains(reply, "unsupported"));
    REQUIRE_EQ(world.asked.load(), 0);
}

TEST_CASE("debug server: a request before hello closes the connection")
{
    rig world;
    boost::asio::io_context io;
    auto stream =
        agent::connect_local(io.get_executor(), agent::local_endpoint::debug,
                             world.server->instance());
    REQUIRE(stream.has_value());
    agent::frame request;
    request.header = R"({"id":1,"op":"debug.snapshot"})";
    const auto bytes = agent::encode_frame(request);
    (*stream)->async_write_some(
        boost::asio::buffer(*bytes),
        [](const boost::system::error_code&, std::size_t) {});
    std::array<vss::byte, 64> buffer{};
    bool closed = false;
    (*stream)->async_read_some(
        boost::asio::buffer(buffer),
        [&](const boost::system::error_code& ec, std::size_t)
        { closed = static_cast<bool>(ec); });
    io.run_for(10s);
    REQUIRE(closed);
}

TEST_CASE("debug server: a ping needs no scope and a second hello is refused")
{
    rig world;
    auto session = world.connect();
    const auto pong = session->call("ping");
    REQUIRE(pong.has_value());
    REQUIRE(pong->ok);
    const auto again = session->call("hello");
    REQUIRE(again.has_value());
    REQUIRE(!again->ok);
}

TEST_CASE("debug server: the engine's own events reach the recorder")
{
    auto& global = vss::trace::recorder::global();
    const auto before = global.last_seq();
    vss::studio app;
    auto port = app.create_virtual_port("recorded-port",
                                        vss::virtual_port_kind::loopback)
                    .value();
    // A port that stops says so, with its own number, in time order beside the
    // calls to Windows.
    app.engine().remove_endpoint(port->id());
    REQUIRE(vss::testkit::wait_until(
        [&]
        {
            vss::trace::filter what;
            what.after_seq = before;
            for (const auto& item : global.read(what))
            {
                if (item.call_text().starts_with("engine."))
                    return true;
            }
            return false;
        },
        5s));
}
