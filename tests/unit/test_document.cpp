//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/config/document.hpp>

#include "test_framework.hpp"

#include <filesystem>
#include <fstream>

using namespace vss;
using namespace vss::config;

namespace {

document sample_document()
{
    document value;
    value.name = "lab";
    value.description = "A representative topology";

    port_entry virtual_port;
    virtual_port.name = "virtual";
    virtual_port.kind = "virtual";
    virtual_port.virtual_kind = "loopback";
    virtual_port.serial.baud_rate = 57600;
    value.ports.push_back(virtual_port);

    port_entry physical;
    physical.name = "real";
    physical.kind = "physical";
    physical.uri = "serial://COM3?baud=115200";
    physical.serial.baud_rate = 9600;
    physical.serial.parity = parity_mode::even;
    physical.serial.read_timeout = std::chrono::milliseconds(150);
    physical.capture = false;
    value.ports.push_back(physical);

    port_entry mock;
    mock.name = "fake";
    mock.kind = "mock";
    mock.mock_target = "modem";
    value.ports.push_back(mock);

    link_entry split;
    split.mode = "split";
    split.sources = {"virtual"};
    split.targets = {"real", "fake"};
    value.links.push_back(split);

    return value;
}

} // namespace

TEST_CASE("document: roundtrip through JSON")
{
    const document original = sample_document();
    const auto text = to_json(original, false);
    REQUIRE(text);

    const auto parsed = from_json(*text);
    REQUIRE(parsed);

    REQUIRE_EQ(parsed->name, original.name);
    REQUIRE_EQ(parsed->description, original.description);
    REQUIRE_EQ(parsed->ports.size(), original.ports.size());
    REQUIRE_EQ(parsed->links.size(), original.links.size());

    REQUIRE_EQ(parsed->ports[1].name, std::string("real"));
    REQUIRE_EQ(parsed->ports[1].uri, std::string("serial://COM3?baud=115200"));
    REQUIRE_EQ(parsed->ports[1].serial.baud_rate, 9600u);
    REQUIRE_EQ(parsed->ports[1].serial.parity, parity_mode::even);
    REQUIRE_EQ(parsed->ports[1].serial.read_timeout.count(), std::int64_t{150});
    REQUIRE(!parsed->ports[1].capture);

    REQUIRE_EQ(parsed->ports[2].mock_target, std::string("modem"));
    REQUIRE_EQ(parsed->links[0].mode, std::string("split"));
    REQUIRE_EQ(parsed->links[0].targets.size(), std::size_t{2});
    REQUIRE(parsed->links[0].emulate_modem_lines);
}

TEST_CASE("document: pretty and compact output")
{
    const auto pretty = to_json(sample_document(), true);
    REQUIRE(pretty);
    REQUIRE(pretty->find('\n') != std::string::npos);
    REQUIRE(pretty->find("  \"version\"") != std::string::npos);
    REQUIRE_EQ(pretty->back(), '\n');

    const auto compact = to_json(sample_document(), false);
    REQUIRE(compact);
    REQUIRE(compact->find('\n') == std::string::npos);
}

TEST_CASE("document: validation rejects broken topologies")
{
    REQUIRE(!validate(sample_document()));

    {
        document value = sample_document();
        value.name.clear();
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.ports[1].name = "virtual";
        REQUIRE_EQ(validate(value), make_error_code(errc::already_exists));
    }
    {
        document value = sample_document();
        value.ports[0].kind = "teleport";
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.ports[0].virtual_kind = "magic";
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.ports[1].uri.clear();
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.ports[0].uri = "serial://COM1";
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.ports[1].serial.baud_rate = 0;
        REQUIRE_EQ(validate(value), make_error_code(errc::invalid_argument));
    }
    {
        document value = sample_document();
        value.links[0].mode = "carrier-pigeon";
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.links[0].sources = {"ghost"};
        REQUIRE_EQ(validate(value), make_error_code(errc::not_found));
    }
    {
        document value = sample_document();
        value.links[0].targets = {"ghost"};
        REQUIRE_EQ(validate(value), make_error_code(errc::not_found));
    }
    {
        document value = sample_document();
        value.links[0].sources.clear();
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.links[0].mode = "duplex";
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.links[0].mode = "split";
        value.links[0].sources = {"virtual", "real"};
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
    {
        document value = sample_document();
        value.links[0].mode = "join";
        REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    }
}

TEST_CASE("document: every malformed field is reported")
{
    struct broken
    {
        const char* json;
        const char* why;
    };

    const broken cases[] = {
        {R"({"ports":[{"name":"a","kind":"virtual","serial":42}]})",
         "serial is not an object"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"baud_rate":"fast"}}]})",
         "baud rate is not a number"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"baud_rate":-1}}]})",
         "baud rate is negative"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"data_bits":"8"}}]})",
         "data bits is not a number"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"parity":1}}]})",
         "parity is not a string"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"stop_bits":1}}]})",
         "stop bits is not a string"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"flow_control":1}}]})",
         "flow control is not a string"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"read_timeout_ms":-5}}]})",
         "read timeout is negative"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"write_timeout_ms":"x"}}]})",
         "write timeout is not a number"},
        {R"({"ports":[{"name":"a","kind":"virtual","serial":{"exclusive":1}}]})",
         "exclusive is not a bool"},
        {R"({"ports":[{"name":"a","kind":"virtual"}],"links":[{"mode":"duplex","sources":"a","targets":["a"]}]})",
         "sources is not an array"},
        {R"({"ports":[{"name":"a","kind":"virtual"}],"links":[{"mode":"duplex","sources":["a"],"targets":"a"}]})",
         "targets is not an array"},
        {R"({"ports":[{"name":"a","kind":"virtual"}],"links":[{"mode":"duplex","sources":[1],"targets":["a"]}]})",
         "source is not a string"},
    };

    for (const auto& entry : cases)
    {
        const auto parsed = from_json(entry.json);
        REQUIRE(!parsed);
        REQUIRE_EQ(parsed.error(), make_error_code(errc::config_error));
    }

    // to_json refuses to serialise an invalid document.
    document invalid;
    invalid.name.clear();
    REQUIRE(!to_json(invalid));
}

TEST_CASE("document: parsing rejects malformed JSON")
{
    REQUIRE_EQ(from_json("{"), make_error_code(errc::config_error));
    REQUIRE_EQ(from_json("[]"), make_error_code(errc::config_error));
    REQUIRE_EQ(from_json("\"text\""), make_error_code(errc::config_error));

    REQUIRE_EQ(from_json(R"({"name":"x","ports":42})").error(),
               make_error_code(errc::config_error));
    REQUIRE_EQ(from_json(R"({"name":"x","links":42})").error(),
               make_error_code(errc::config_error));
    REQUIRE_EQ(from_json(R"({"name":"x","ports":[42]})").error(),
               make_error_code(errc::config_error));
    REQUIRE_EQ(from_json(
                   R"({"ports":[{"name":"a","kind":"physical","uri":"serial://C","serial":{"baud_rate":"fast"}}]})")
                   .error(),
               make_error_code(errc::config_error));
    REQUIRE_EQ(from_json(
                   R"({"ports":[{"name":"a","kind":"virtual","serial":{"parity":"triple"}}]})")
                   .error(),
               make_error_code(errc::config_error));
    REQUIRE_EQ(from_json(
                   R"({"ports":[{"name":"a","kind":"virtual"}],"links":[{"mode":"duplex","sources":["a"],"targets":["a"],"policy":"anarchy"}]})")
                   .error(),
               make_error_code(errc::config_error));
}

TEST_CASE("document: defaults are applied when fields are missing")
{
    const auto parsed = from_json(
        R"({"ports":[{"name":"v","kind":"virtual"}]})");
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->name, std::string("untitled"));
    REQUIRE_EQ(parsed->ports.size(), std::size_t{1});
    REQUIRE_EQ(parsed->ports[0].virtual_kind, std::string("auto"));
    REQUIRE_EQ(parsed->ports[0].serial.baud_rate, 115200u);
    REQUIRE(parsed->ports[0].capture);
    REQUIRE(parsed->links.empty());
}

TEST_CASE("document: optional fields tolerate null and foreign types")
{
    const auto null_serial = from_json(
        R"({"ports":[{"name":"a","kind":"virtual","serial":null}]})");
    REQUIRE(null_serial);
    REQUIRE_EQ(null_serial->ports[0].serial.baud_rate, 115200u);

    const auto loose = from_json(
        R"({"name":42,"ports":[{"name":"a","kind":"virtual","capture":"yes"}]})");
    REQUIRE(loose);
    REQUIRE_EQ(loose->name, std::string("untitled"));
    REQUIRE(loose->ports[0].capture);
}

TEST_CASE("document: every virtual port kind is accepted")
{
    for (const char* kind : virtual_port_kinds)
    {
        document value;
        value.name = "kinds";
        port_entry port;
        port.name = "port";
        port.kind = "virtual";
        port.virtual_kind = kind;
        value.ports.push_back(port);

        REQUIRE(!validate(value));
        REQUIRE(to_json(value, false));
    }
}

TEST_CASE("document: kind mapping")
{
    REQUIRE_EQ(*to_endpoint_kind("virtual"), endpoint_kind::virtual_port);
    REQUIRE_EQ(*to_endpoint_kind("physical"), endpoint_kind::physical);
    REQUIRE_EQ(*to_endpoint_kind("tcp"), endpoint_kind::tcp);
    REQUIRE_EQ(*to_endpoint_kind("udp"), endpoint_kind::udp);
    REQUIRE_EQ(*to_endpoint_kind("rfc2217"), endpoint_kind::rfc2217);
    REQUIRE_EQ(*to_endpoint_kind("mock"), endpoint_kind::mock);
    REQUIRE_EQ(to_endpoint_kind("serial").error(),
               make_error_code(errc::config_error));
}

TEST_CASE("document: save and load a file")
{
    const auto path = std::filesystem::current_path() / "vss-document-test.json";
    std::filesystem::remove(path);

    const auto saved = save_file(path.string(), sample_document(), true);
    REQUIRE(saved);
    REQUIRE(std::filesystem::exists(path));

    const auto loaded = load_file(path.string());
    REQUIRE(loaded);
    REQUIRE_EQ(loaded->name, std::string("lab"));
    REQUIRE_EQ(loaded->ports.size(), std::size_t{3});

    std::filesystem::remove(path);

    REQUIRE_EQ(load_file(path.string()).error(),
               make_error_code(errc::not_found));

    REQUIRE_EQ(load_file(std::filesystem::current_path().string()).error(),
               make_error_code(errc::invalid_argument));
    REQUIRE_EQ(load_file("").error(), make_error_code(errc::not_found));

    const auto missing_directory =
        (std::filesystem::current_path() / "no-such-directory" / "file.json")
            .string();
    REQUIRE_EQ(save_file(missing_directory, sample_document()).error(),
               make_error_code(errc::permission_denied));
}

TEST_CASE("document: owner policy roundtrips")
{
    document value = sample_document();
    value.links[0].mode = "join";
    value.links[0].sources = {"virtual", "fake"};
    value.links[0].targets = {"real"};
    value.links[0].policy = merge_policy::owner;
    value.links[0].emulate_modem_lines = false;

    const auto text = to_json(value, false);
    REQUIRE(text);
    const auto parsed = from_json(*text);
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->links[0].policy, merge_policy::owner);
    REQUIRE(!parsed->links[0].emulate_modem_lines);
    REQUIRE_EQ(parsed->links[0].mode, std::string("join"));
}

namespace
{

document bus_document()
{
    document value;
    value.name = "bus";
    for (const char* name : {"master", "one", "two", "tap"})
    {
        port_entry port;
        port.name = name;
        port.kind = "virtual";
        port.virtual_kind = "loopback";
        value.ports.push_back(port);
    }

    link_entry bus;
    bus.mode = "bus";
    bus.members = {"master", "one", "two"};
    bus.emulate_modem_lines = false;
    value.links.push_back(bus);

    link_entry tap;
    tap.mode = "join";
    tap.sources = {"master", "one", "two"};
    tap.targets = {"tap"};
    tap.emulate_modem_lines = false;
    value.links.push_back(tap);
    return value;
}

} // namespace

TEST_CASE("document: a bus round trips with its members")
{
    const document original = bus_document();
    REQUIRE(!validate(original));

    const auto text = to_json(original);
    REQUIRE(text);
    REQUIRE(text->find("\"mode\": \"bus\"") != std::string::npos);
    REQUIRE(text->find("\"members\"") != std::string::npos);
    // The version is still 1: a file without a bus loads as before.
    REQUIRE(text->find("\"version\": 1") != std::string::npos);

    const auto parsed = from_json(*text);
    REQUIRE(parsed);
    REQUIRE(*parsed == original);
    REQUIRE_EQ(parsed->links[0].members.size(), std::size_t{3});
    REQUIRE(parsed->links[0].sources.empty());
    REQUIRE(parsed->links[0].targets.empty());
}

TEST_CASE("document: the links of a plain bus are written exactly as before")
{
    // The whole text of the links, so that an option added to a bus later
    // cannot change what a file without it looks like.
    const auto text = to_json(bus_document(), false);
    REQUIRE(text);
    const auto at = text->find("\"links\":");
    REQUIRE(at != std::string::npos);
    REQUIRE_EQ(text->substr(at),
               std::string(
                   R"("links":[{"mode":"bus","members":["master","one","two"],)"
                   R"("emulate_modem_lines":false,"policy":"interleave"},)"
                   R"({"mode":"join","sources":["master","one","two"],)"
                   R"("targets":["tap"],"emulate_modem_lines":false,)"
                   R"("policy":"interleave"}]})"));
}

TEST_CASE("document: a bus in a hand written file needs only its members")
{
    const auto parsed =
        from_json(R"({"version":1,"ports":[{"name":"a","kind":"virtual"},)"
                  R"({"name":"b","kind":"virtual"}],)"
                  R"("links":[{"mode":"bus","members":["a","b"]}]})");
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->links.size(), std::size_t{1});
    REQUIRE_EQ(parsed->links[0].mode, std::string("bus"));
    // Absent means off for a bus.
    REQUIRE(!parsed->links[0].emulate_modem_lines);
    REQUIRE(parsed->links[0].policy == merge_policy::interleave);
}

TEST_CASE("document: a bus is validated")
{
    const auto expect_invalid = [](const std::string& links, errc expected)
    {
        const auto parsed = from_json(
            R"({"version":1,"ports":[{"name":"a","kind":"virtual"},)"
            R"({"name":"b","kind":"virtual"},{"name":"c","kind":"virtual"}],)"
            R"("links":[)" +
            links + "]}");
        REQUIRE(!parsed);
        REQUIRE_EQ(parsed.error(), make_error_code(expected));
    };

    expect_invalid(R"({"mode":"bus","members":["a"]})", errc::config_error);
    expect_invalid(R"({"mode":"bus"})", errc::config_error);
    expect_invalid(R"({"mode":"bus","members":["a","a"]})", errc::config_error);
    expect_invalid(R"({"mode":"bus","members":["a","zzz"]})", errc::not_found);
    expect_invalid(R"({"mode":"bus","members":["a","b"],"sources":["c"]})",
                   errc::config_error);
    expect_invalid(R"({"mode":"bus","members":["a","b"],"targets":["c"]})",
                   errc::config_error);
    expect_invalid(
        R"({"mode":"bus","members":["a","b"],"emulate_modem_lines":true})",
        errc::config_error);
    expect_invalid(R"({"mode":"bus","members":["a","b"],"policy":"owner"})",
                   errc::config_error);
    expect_invalid(R"({"mode":"bus","members":"a"})", errc::config_error);
    // Members belong to a bus only.
    expect_invalid(
        R"({"mode":"split","sources":["a"],"targets":["b"],"members":["c"]})",
        errc::config_error);

    // The most members a bus takes, and one more.
    document big;
    big.name = "big";
    link_entry bus;
    bus.mode = "bus";
    bus.emulate_modem_lines = false;
    for (std::size_t index = 0; index <= max_bus_members; ++index)
    {
        port_entry port;
        port.name = "p" + std::to_string(index);
        port.kind = "virtual";
        big.ports.push_back(port);
        bus.members.push_back(port.name);
    }
    big.links.push_back(bus);
    REQUIRE_EQ(validate(big), make_error_code(errc::config_error));
    big.links[0].members.pop_back();
    REQUIRE(!validate(big));
}

TEST_CASE("document: a virtual port can ask for its COM number")
{
    document value;
    value.name = "numbers";
    port_entry app;
    app.name = "app";
    app.kind = "virtual";
    app.virtual_kind = "windows-driver";
    app.com_number = 7;
    value.ports.push_back(app);
    port_entry automatic;
    automatic.name = "auto";
    automatic.kind = "virtual";
    automatic.virtual_kind = "auto";
    automatic.com_number = 255;
    value.ports.push_back(automatic);
    port_entry plain;
    plain.name = "plain";
    value.ports.push_back(plain);

    REQUIRE(!validate(value));
    const auto text = to_json(value, false);
    REQUIRE(text.has_value());
    // Only the ports that ask for a number write one.
    REQUIRE(text->find("\"com_number\":7") != std::string::npos);
    REQUIRE(text->find("\"com_number\":255") != std::string::npos);
    REQUIRE_EQ(text->find("\"com_number\"", text->find("\"com_number\":255") + 1),
               std::string::npos);

    const auto parsed = from_json(*text);
    REQUIRE(parsed.has_value());
    REQUIRE_EQ(parsed->ports.size(), std::size_t{3});
    REQUIRE_EQ(parsed->ports[0].com_number, 7u);
    REQUIRE_EQ(parsed->ports[1].com_number, 255u);
    REQUIRE_EQ(parsed->ports[2].com_number, 0u);
    REQUIRE(*parsed == value);
}

TEST_CASE("document: a COM number must be a number a COM port can have, on a port that has one")
{
    const auto parse = [](const char* port_fields)
    {
        return from_json(std::string(R"({"name":"n","ports":[{"name":"p",)")
                         + port_fields + "}]}");
    };

    REQUIRE(parse(R"("kind":"virtual","com_number":1)").has_value());
    REQUIRE(parse(R"("kind":"virtual","virtual_kind":"windows-driver","com_number":255)")
                .has_value());

    // Out of range, or not an integer.
    REQUIRE(!parse(R"("kind":"virtual","com_number":0)").has_value());
    REQUIRE(!parse(R"("kind":"virtual","com_number":256)").has_value());
    REQUIRE(!parse(R"("kind":"virtual","com_number":-3)").has_value());
    REQUIRE(!parse(R"("kind":"virtual","com_number":"7")").has_value());
    REQUIRE(!parse(R"("kind":"virtual","com_number":4.5)").has_value());

    // A kind of port that has no COM number.
    REQUIRE(!parse(R"("kind":"virtual","virtual_kind":"pty","com_number":4)").has_value());
    REQUIRE(!parse(R"("kind":"virtual","virtual_kind":"loopback","com_number":4)").has_value());
    REQUIRE(!parse(R"("kind":"tcp","uri":"tcp://127.0.0.1:1","com_number":4)").has_value());
    REQUIRE(!parse(R"("kind":"mock","com_number":4)").has_value());

    // Setting it in memory is held to the same rules.
    document value;
    value.name = "n";
    port_entry port;
    port.name = "p";
    port.virtual_kind = "pty";
    port.com_number = 4;
    value.ports.push_back(port);
    REQUIRE(validate(value) == make_error_code(errc::config_error));
    REQUIRE(!to_json(value).has_value());
    value.ports[0].virtual_kind = "auto";
    value.ports[0].com_number = 256;
    REQUIRE(validate(value) == make_error_code(errc::config_error));
}

TEST_CASE("document: a link's delay and jitter round trip and stay out when zero")
{
    document value = bus_document();
    value.links[0].delay_ms = 100;
    value.links[0].jitter_ms = 20;
    value.links[1].delay_ms = 5;

    const auto text = to_json(value);
    REQUIRE(text);
    REQUIRE(text->find("\"delay_ms\": 100") != std::string::npos);
    REQUIRE(text->find("\"jitter_ms\": 20") != std::string::npos);
    REQUIRE(text->find("\"delay_ms\": 5") != std::string::npos);
    // The version is still 1.
    REQUIRE(text->find("\"version\": 1") != std::string::npos);

    const auto parsed = from_json(*text);
    REQUIRE(parsed);
    REQUIRE(*parsed == value);
    REQUIRE_EQ(parsed->links[0].delay_ms, std::uint32_t{100});
    REQUIRE_EQ(parsed->links[0].jitter_ms, std::uint32_t{20});
    REQUIRE_EQ(parsed->links[1].jitter_ms, std::uint32_t{0});

    // A link without latency writes neither field, so an old file is stable.
    const auto plain = to_json(bus_document());
    REQUIRE(plain);
    REQUIRE(plain->find("delay_ms") == std::string::npos);
    REQUIRE(plain->find("jitter_ms") == std::string::npos);
}

TEST_CASE("document: delay and jitter are checked")
{
    const auto parse = [](const std::string& link)
    {
        return from_json(
            R"({"version":1,"ports":[{"name":"a","kind":"virtual"},)"
            R"({"name":"b","kind":"virtual"}],"links":[)" +
            link + "]}");
    };

    const auto ok = parse(
        R"({"mode":"duplex","sources":["a"],"targets":["b"],)"
        R"("delay_ms":200,"jitter_ms":10})");
    REQUIRE(ok);
    REQUIRE_EQ(ok->links[0].delay_ms, std::uint32_t{200});

    const auto bus = parse(
        R"({"mode":"bus","members":["a","b"],"delay_ms":30})");
    REQUIRE(bus);
    REQUIRE_EQ(bus->links[0].delay_ms, std::uint32_t{30});

    const auto expect_invalid = [&](const std::string& fields)
    {
        const auto parsed = parse(
            R"({"mode":"duplex","sources":["a"],"targets":["b"],)" + fields +
            "}");
        REQUIRE(!parsed);
        REQUIRE_EQ(parsed.error(), make_error_code(errc::config_error));
    };

    expect_invalid(R"("delay_ms":-1)");
    expect_invalid(R"("delay_ms":"100")");
    expect_invalid(R"("delay_ms":1.5)");
    expect_invalid(R"("delay_ms":60001)");
    expect_invalid(R"("delay_ms":true)");
    expect_invalid(R"("jitter_ms":-1,"delay_ms":10)");
    // Jitter cannot exceed the delay, or a chunk would be due before it left.
    expect_invalid(R"("delay_ms":10,"jitter_ms":11)");
    expect_invalid(R"("jitter_ms":5)");

    const auto edge = parse(
        R"({"mode":"duplex","sources":["a"],"targets":["b"],)"
        R"("delay_ms":60000,"jitter_ms":60000})");
    REQUIRE(edge);

    // The same limits hold for a document built in code.
    document value = bus_document();
    value.links[0].delay_ms = max_link_delay_ms + 1;
    REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
    value.links[0].delay_ms = 10;
    value.links[0].jitter_ms = 11;
    REQUIRE_EQ(validate(value), make_error_code(errc::config_error));
}

TEST_CASE("document: a half-duplex bus round trips and stays out when off")
{
    document value = bus_document();
    value.links[0].delay_ms = 3;
    value.links[0].half_duplex.enabled = true;
    value.links[0].half_duplex.bit_rate = 19200;
    value.links[0].half_duplex.turnaround_ms = 2;
    value.links[0].half_duplex.on_collision = collision_action::garble;
    value.links[0].half_duplex.rts_gating = true;
    REQUIRE(!validate(value));

    const auto text = to_json(value, false);
    REQUIRE(text);
    REQUIRE(text->find(R"("delay_ms":3,"half_duplex":{"bit_rate":19200,)"
                       R"("turnaround_ms":2,"collisions":"garble",)"
                       R"("rts_gating":true}})") != std::string::npos);
    // The version is still 1.
    REQUIRE(text->find("\"version\":1") != std::string::npos);

    const auto parsed = from_json(*text);
    REQUIRE(parsed);
    REQUIRE(*parsed == value);
    REQUIRE(parsed->links[0].half_duplex == value.links[0].half_duplex);
    // The join beside it has none.
    REQUIRE(!parsed->links[1].half_duplex.enabled);

    // A bus without the option does not mention it.
    const auto plain = to_json(bus_document());
    REQUIRE(plain);
    REQUIRE(plain->find("half_duplex") == std::string::npos);

    // Values of an option that is off are not written either.
    document off = bus_document();
    off.links[0].half_duplex.bit_rate = 300;
    const auto silent = to_json(off);
    REQUIRE(silent);
    REQUIRE(silent->find("half_duplex") == std::string::npos);
}

TEST_CASE("document: a half-duplex bus in a hand written file")
{
    const auto parse = [](const std::string& link)
    {
        return from_json(
            R"({"version":1,"ports":[{"name":"a","kind":"virtual"},)"
            R"({"name":"b","kind":"virtual"}],"links":[)" +
            link + "]}");
    };

    // The object alone turns the option on, with the defaults.
    const auto bare =
        parse(R"({"mode":"bus","members":["a","b"],"half_duplex":{}})");
    REQUIRE(bare);
    REQUIRE(bare->links[0].half_duplex.enabled);
    REQUIRE_EQ(bare->links[0].half_duplex.bit_rate, std::uint32_t{9600});
    REQUIRE_EQ(bare->links[0].half_duplex.turnaround_ms, std::uint32_t{0});
    REQUIRE(bare->links[0].half_duplex.on_collision ==
            collision_action::report);
    REQUIRE(!bare->links[0].half_duplex.rts_gating);

    const auto full =
        parse(R"({"mode":"bus","members":["a","b"],"half_duplex":)"
              R"({"bit_rate":4000000,"turnaround_ms":60000,)"
              R"("collisions":"report","rts_gating":false}})");
    REQUIRE(full);
    REQUIRE_EQ(full->links[0].half_duplex.bit_rate, max_bus_bit_rate);
    REQUIRE_EQ(full->links[0].half_duplex.turnaround_ms, max_link_delay_ms);

    const auto expect_invalid = [&](const std::string& line)
    {
        const auto parsed = parse(
            R"({"mode":"bus","members":["a","b"],"half_duplex":)" + line + "}");
        REQUIRE(!parsed);
        REQUIRE(parsed.error() == make_error_code(errc::config_error));
    };
    expect_invalid("true");
    expect_invalid("[]");
    expect_invalid(R"({"bit_rate":49})");
    expect_invalid(R"({"bit_rate":4000001})");
    expect_invalid(R"({"bit_rate":-9600})");
    expect_invalid(R"({"bit_rate":"fast"})");
    expect_invalid(R"({"bit_rate":9600.5})");
    expect_invalid(R"({"turnaround_ms":60001})");
    expect_invalid(R"({"turnaround_ms":-1})");
    expect_invalid(R"({"turnaround_ms":"2"})");
    expect_invalid(R"({"collisions":"drop"})");
    expect_invalid(R"({"collisions":1})");
    expect_invalid(R"({"rts_gating":"yes"})");

    // Only a bus is one wire that its members take turns on.
    for (const char* mode : {"duplex", "redirect", "monitor"})
    {
        const auto parsed = parse(
            std::string(R"({"mode":")") + mode +
            R"(","sources":["a"],"targets":["b"],"emulate_modem_lines":false,)"
            R"("half_duplex":{}})");
        REQUIRE(!parsed);
        REQUIRE(parsed.error() == make_error_code(errc::config_error));
    }

    // validate() says the same of a document built in code.
    document value = bus_document();
    value.links[1].half_duplex.enabled = true;
    REQUIRE(validate(value) == make_error_code(errc::config_error));
    value = bus_document();
    value.links[0].half_duplex.enabled = true;
    value.links[0].half_duplex.bit_rate = min_bus_bit_rate - 1;
    REQUIRE(validate(value) == make_error_code(errc::config_error));
}

// -- plugin bindings ---------------------------------------------------------

namespace {

plugin_binding fixture_binding()
{
    plugin_binding binding;
    binding.id = "com.example.modbus-fixture";
    binding.kind = "fixture";
    binding.type = "meter";
    binding.name = "Modbus fixture";
    binding.publisher = "Example Ltd";
    binding.key_id = "9f2c41d07a6be835";
    binding.version = "1.2.0";
    binding.get_it_from = "https://example.com/plugins";
    binding.params = R"({"address":17,"registers":[1,2,3],"label":"A"})";
    return binding;
}

plugin_binding modifier_binding(const std::string& type)
{
    plugin_binding binding;
    binding.id = "com.example.line-noise";
    binding.kind = "modifier";
    binding.type = type;
    binding.on_timeout = "fail_closed";
    return binding;
}

plugin_binding decoder_binding()
{
    plugin_binding binding;
    binding.id = "com.example.decoders";
    binding.kind = "decoder";
    binding.type = "modbus-rtu";
    return binding;
}

/// A fixture `device`, a port `program` and a duplex link between them, with
/// a binding on each.
document bound_document()
{
    document value;
    value.name = "with plugins";

    port_entry device;
    device.name = "device";
    device.kind = "mock";
    device.plugins = {fixture_binding(), decoder_binding()};
    value.ports.push_back(device);

    port_entry program;
    program.name = "program";
    value.ports.push_back(program);

    link_entry link;
    link.sources = {"program"};
    link.targets = {"device"};
    link.plugins = {modifier_binding("drop"), modifier_binding("delay"),
                    decoder_binding()};
    value.links.push_back(link);
    return value;
}

/// A setup with one port whose `plugins` member is the given JSON text.
result<document> parse_port_plugins(const std::string& plugins)
{
    return from_json(R"({"version":1,"ports":[{"name":"a","kind":"mock",)"
                     R"("plugins":)" +
                     plugins + "}]}");
}

void expect_port_plugins_refused(const std::string& plugins)
{
    const auto parsed = parse_port_plugins(plugins);
    REQUIRE(!parsed);
    REQUIRE(parsed.error() == make_error_code(errc::config_error));
}

/// One binding with `member` added to the required three.
std::string binding_with(const std::string& member)
{
    return R"([{"id":"com.example.x","kind":"fixture","type":"t",)" + member +
           "}]";
}

} // namespace

TEST_CASE("document: plugin bindings round trip on a port and on a link")
{
    const document original = bound_document();
    REQUIRE(!validate(original));

    for (const bool pretty : {true, false})
    {
        const auto text = to_json(original, pretty);
        REQUIRE(text);
        const auto parsed = from_json(*text);
        REQUIRE(parsed);
        REQUIRE(*parsed == original);

        // Written again, the text is the same.
        const auto again = to_json(*parsed, pretty);
        REQUIRE(again);
        REQUIRE_EQ(*again, *text);
    }

    // The order of the list is kept: it is the order modifiers apply in.
    const auto parsed = from_json(*to_json(original));
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->links[0].plugins.size(), std::size_t{3});
    REQUIRE_EQ(parsed->links[0].plugins[0].type, std::string("drop"));
    REQUIRE_EQ(parsed->links[0].plugins[1].type, std::string("delay"));
    REQUIRE_EQ(parsed->links[0].plugins[2].kind, std::string("decoder"));
}

TEST_CASE("document: a binding is written with only the members that are set")
{
    const auto text = to_json(bound_document(), false);
    REQUIRE(text);
    // The version is still 1.
    REQUIRE(text->find("\"version\":1") != std::string::npos);

    // The whole fixture binding, in the order the members are written.
    REQUIRE(text->find(
                R"("plugins":[{"id":"com.example.modbus-fixture",)"
                R"("kind":"fixture","type":"meter","name":"Modbus fixture",)"
                R"("publisher":"Example Ltd","key_id":"9f2c41d07a6be835",)"
                R"("version":"1.2.0",)"
                R"("get_it_from":"https://example.com/plugins",)"
                R"("params":{"address":17,"registers":[1,2,3],"label":"A"}},)"
                R"({"id":"com.example.decoders","kind":"decoder",)"
                R"("type":"modbus-rtu"}])") != std::string::npos);
    // A modifier with nothing but its choice on a timeout.
    REQUIRE(text->find(R"({"id":"com.example.line-noise","kind":"modifier",)"
                       R"("type":"drop","on_timeout":"fail_closed"})") !=
            std::string::npos);
}

TEST_CASE("document: a setup without plugins is written exactly as before")
{
    const auto text = to_json(sample_document(), false);
    REQUIRE(text);
    REQUIRE(text->find("plugins") == std::string::npos);

    // An empty list in a file reads as no bindings and is not written back.
    const auto parsed = parse_port_plugins("[]");
    REQUIRE(parsed);
    REQUIRE(parsed->ports[0].plugins.empty());
    const auto written = to_json(*parsed, false);
    REQUIRE(written);
    REQUIRE(written->find("plugins") == std::string::npos);
}

TEST_CASE("document: a file from before plugin bindings reads as it did")
{
    const std::string old_file =
        R"({"version":1,"name":"old","ports":[{"name":"a","kind":"virtual"},)"
        R"({"name":"b","kind":"virtual"}],"links":[{"mode":"duplex",)"
        R"("sources":["a"],"targets":["b"]}]})";
    const auto parsed = from_json(old_file);
    REQUIRE(parsed);
    REQUIRE(parsed->ports[0].plugins.empty());
    REQUIRE(parsed->ports[1].plugins.empty());
    REQUIRE(parsed->links[0].plugins.empty());
}

TEST_CASE("document: a binding in a hand written file needs three members")
{
    const auto parsed = parse_port_plugins(
        R"([{"id":"com.example.x","kind":"fixture","type":"t"}])");
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->ports[0].plugins.size(), std::size_t{1});
    const plugin_binding& binding = parsed->ports[0].plugins[0];
    REQUIRE_EQ(binding.id, std::string("com.example.x"));
    REQUIRE(binding.name.empty());
    REQUIRE(binding.on_timeout.empty());
    REQUIRE(binding.params.empty());

    expect_port_plugins_refused(R"([{"kind":"fixture","type":"t"}])");
    expect_port_plugins_refused(R"([{"id":"com.example.x","type":"t"}])");
    expect_port_plugins_refused(R"([{"id":"com.example.x","kind":"fixture"}])");
    expect_port_plugins_refused("[{}]");
}

TEST_CASE("document: a plugin the reader does not know is kept")
{
    // Nothing here is looked up: the id, the type and the parameters of a
    // plugin that is not installed are read and written back.
    const std::string plugins =
        R"([{"id":"org.unknown.thing","kind":"fixture","type":"gadget",)"
        R"("params":{"mode":"x","deep":{"list":[true,null,15,"s"]}}}])";
    const auto parsed = parse_port_plugins(plugins);
    REQUIRE(parsed);
    const auto text = to_json(*parsed, false);
    REQUIRE(text);
    REQUIRE(text->find(plugins.substr(1, plugins.size() - 2)) !=
            std::string::npos);

    // A kind from a newer version is kept too, on a port or on a link.
    const auto newer = parse_port_plugins(
        R"([{"id":"org.unknown.thing","kind":"recorder","type":"t"}])");
    REQUIRE(newer);
    REQUIRE_EQ(newer->ports[0].plugins[0].kind, std::string("recorder"));
    document on_link = bound_document();
    on_link.links[0].plugins[2].kind = "recorder";
    REQUIRE(!validate(on_link));
}

TEST_CASE("document: the members of a binding are type checked")
{
    expect_port_plugins_refused("{}");
    expect_port_plugins_refused("\"com.example.x\"");
    expect_port_plugins_refused("[1]");
    expect_port_plugins_refused(R"(["com.example.x"])");
    expect_port_plugins_refused(R"([{"id":7,"kind":"fixture","type":"t"}])");
    expect_port_plugins_refused(
        R"([{"id":"com.example.x","kind":null,"type":"t"}])");
    for (const char* member :
         {R"("name":1)", R"("publisher":[])", R"("key_id":{})",
          R"("version":1.2)", R"("get_it_from":false)", R"("on_timeout":0)",
          R"("params":"{}")", R"("params":[1])", R"("params":null)"})
    {
        expect_port_plugins_refused(binding_with(member));
    }

    // A member the format does not have is passed over, as elsewhere in the
    // file.
    REQUIRE(parse_port_plugins(binding_with(R"("colour":"red")")));
}

TEST_CASE("document: the id, kind and type of a binding are checked")
{
    const auto with = [](const char* id, const char* kind, const char* type)
    {
        document value = bound_document();
        value.ports[0].plugins.resize(1);
        value.ports[0].plugins[0].id = id;
        value.ports[0].plugins[0].kind = kind;
        value.ports[0].plugins[0].type = type;
        return validate(value);
    };
    REQUIRE(!with("com.example.x", "fixture", "t"));
    REQUIRE(!with("a.b", "fixture", "t9_x-y"));
    REQUIRE(!with((std::string(126, 'a') + ".b").c_str(), "fixture", "t"));

    for (const char* id : {"", "ab", "nodots", "Com.Example.X", "com..x",
                           ".com.x", "com.x.", "com.x-", "com.ex ample",
                           "com.example/x", "com.ex\xc3\xa4mple.x"})
    {
        REQUIRE(with(id, "fixture", "t") ==
                make_error_code(errc::config_error));
    }
    REQUIRE(with((std::string(127, 'a') + ".b").c_str(), "fixture", "t") ==
            make_error_code(errc::config_error));

    for (const char* local : {"", "9t", "T", "a b", "a.b", "-a"})
    {
        REQUIRE(with("com.example.x", "fixture", local) ==
                make_error_code(errc::config_error));
        REQUIRE(with("com.example.x", local, "t") ==
                make_error_code(errc::config_error));
    }
    const std::string long_id(65, 'a');
    REQUIRE(with("com.example.x", "fixture", long_id.c_str()) ==
            make_error_code(errc::config_error));
    REQUIRE(!with("com.example.x", "fixture", long_id.substr(1).c_str()));
}

TEST_CASE("document: the texts of a binding are plain and have limits")
{
    const auto with = [](std::string plugin_binding::* member,
                         const std::string& text)
    {
        document value = bound_document();
        value.ports[0].plugins[0].*member = text;
        return validate(value);
    };
    const error_code refused = make_error_code(errc::config_error);

    REQUIRE(!with(&plugin_binding::name, std::string(100, 'n')));
    REQUIRE(with(&plugin_binding::name, std::string(101, 'n')) == refused);
    REQUIRE(!with(&plugin_binding::publisher, std::string(100, 'p')));
    REQUIRE(with(&plugin_binding::publisher, std::string(101, 'p')) ==
            refused);
    REQUIRE(!with(&plugin_binding::version, std::string(64, '1')));
    REQUIRE(with(&plugin_binding::version, std::string(65, '1')) == refused);
    REQUIRE(!with(&plugin_binding::key_id, std::string(128, 'f')));
    REQUIRE(with(&plugin_binding::key_id, std::string(129, 'f')) == refused);
    REQUIRE(!with(&plugin_binding::get_it_from, std::string(256, 'u')));
    REQUIRE(with(&plugin_binding::get_it_from, std::string(257, 'u')) ==
            refused);

    // Letters of any language are fine.
    REQUIRE(!with(&plugin_binding::name, "Z\xc3\xa4hler f\xc3\xbcr Modbus"));

    // A control character, a line separator, a byte order mark or a mark
    // that changes the direction of the text is not.  The two marks that
    // open a run are closed again, so that this file itself reads in order.
    for (const char* bad :
         {"two\nlines", "tab\there", "bell\x07", "del\x7f", "c1\xc2\x85x",
          "alm\xd8\x9cx", "lrm\xe2\x80\x8ex", "ls\xe2\x80\xa8x",
          "rlo\xe2\x80\xaegpj.exe\xe2\x80\xac",
          "iso\xe2\x81\xa6x\xe2\x81\xa9", "\xef\xbb\xbfmark"})
    {
        REQUIRE(with(&plugin_binding::name, bad) == refused);
        REQUIRE(with(&plugin_binding::publisher, bad) == refused);
        REQUIRE(with(&plugin_binding::get_it_from, bad) == refused);
    }
}

TEST_CASE("document: what a binding does on a timeout is one of two words")
{
    document value = bound_document();
    for (const char* word : {"", "fail_open", "fail_closed"})
    {
        value.links[0].plugins[0].on_timeout = word;
        REQUIRE(!validate(value));
    }
    for (const char* word : {"open", "FAIL_OPEN", "fail_open "})
    {
        value.links[0].plugins[0].on_timeout = word;
        REQUIRE(validate(value) == make_error_code(errc::config_error));
    }
}

TEST_CASE("document: the parameters of a binding are one object with limits")
{
    const error_code refused = make_error_code(errc::config_error);
    const auto with = [](const std::string& params)
    {
        document value = bound_document();
        value.ports[0].plugins[0].params = params;
        return validate(value);
    };
    REQUIRE(!with(""));
    REQUIRE(!with("{}"));
    REQUIRE(!with(R"({ "spaced" : 1 })"));
    for (const char* bad : {"[]", "1", "null", "\"text\"", "{", "{} {}"})
    {
        REQUIRE(with(bad) == refused);
    }

    // Eight levels are the most: the object itself is the first.
    const auto nested = [](std::size_t levels)
    {
        std::string text = "{}";
        for (std::size_t i = 1; i < levels; ++i)
        {
            text.insert(0, R"({"a":)");
            text += '}';
        }
        return text;
    };
    REQUIRE(!with(nested(max_binding_params_depth)));
    REQUIRE(with(nested(max_binding_params_depth + 1)) == refused);
    REQUIRE(parse_port_plugins(
        binding_with("\"params\":" + nested(max_binding_params_depth))));
    expect_port_plugins_refused(
        binding_with("\"params\":" + nested(max_binding_params_depth + 1)));

    // The size limit is on the compact text.
    const std::string frame = R"({"v":""})";
    const std::string fits =
        R"({"v":")" +
        std::string(max_binding_params_size - frame.size(), 'x') + "\"}";
    REQUIRE_EQ(fits.size(), max_binding_params_size);
    REQUIRE(!with(fits));
    REQUIRE(with(R"({"v":")" +
                 std::string(max_binding_params_size - frame.size() + 1, 'x') +
                 "\"}") == refused);

    // A document built in code with spaced parameters is written compact,
    // and reads back as the compact text.
    document value = bound_document();
    value.ports[0].plugins[0].params = R"({ "spaced" : 1 })";
    const auto text = to_json(value, false);
    REQUIRE(text);
    const auto parsed = from_json(*text);
    REQUIRE(parsed);
    REQUIRE_EQ(parsed->ports[0].plugins[0].params,
               std::string(R"({"spaced":1})"));
}

TEST_CASE("document: which bindings a port and a link may have")
{
    const error_code refused = make_error_code(errc::config_error);

    // One device at a port: a fixture or a generator, not two.
    document two = bound_document();
    plugin_binding generator = fixture_binding();
    generator.kind = "generator";
    two.ports[0].plugins.push_back(generator);
    REQUIRE(validate(two) == refused);
    two.ports[0].plugins = {generator, decoder_binding(), decoder_binding()};
    REQUIRE(!validate(two));

    // A modifier sits on a link, never at a port.
    document modifier_on_port = bound_document();
    modifier_on_port.ports[1].plugins = {modifier_binding("drop")};
    REQUIRE(validate(modifier_on_port) == refused);

    // A fixture or a generator stands at a port, never on a link.
    document device_on_link = bound_document();
    device_on_link.links[0].plugins = {fixture_binding()};
    REQUIRE(validate(device_on_link) == refused);
    device_on_link.links[0].plugins = {generator};
    REQUIRE(validate(device_on_link) == refused);

    // A bus may have bindings like any other link.
    document bus = bus_document();
    bus.links[0].plugins = {decoder_binding(), modifier_binding("noise")};
    REQUIRE(!validate(bus));
    const auto text = to_json(bus, false);
    REQUIRE(text);
    const auto parsed = from_json(*text);
    REQUIRE(parsed);
    REQUIRE(*parsed == bus);

    // Sixteen at one place are the most, in code and in a file.
    document many = bound_document();
    many.links[0].plugins.assign(max_plugin_bindings, decoder_binding());
    REQUIRE(!validate(many));
    many.links[0].plugins.push_back(decoder_binding());
    REQUIRE(validate(many) == refused);

    std::string list = "[";
    for (std::size_t i = 0; i <= max_plugin_bindings; ++i)
    {
        list += i == 0 ? "" : ",";
        list += R"({"id":"com.example.x","kind":"decoder","type":"t"})";
    }
    expect_port_plugins_refused(list + "]");
}

TEST_CASE("document: bindings are saved to a file and loaded from it")
{
    const auto path = std::filesystem::temp_directory_path() /
                      "vss_test_document_plugins.json";
    const document original = bound_document();
    REQUIRE(save_file(path.string(), original));
    const auto loaded = load_file(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    REQUIRE(loaded);
    REQUIRE(*loaded == original);
}
