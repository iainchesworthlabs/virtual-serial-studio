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
    REQUIRE_EQ(
        text->substr(at),
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
