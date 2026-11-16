//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `bundle.*` (`make_bundle_point`) on its own.  A test
// makes the handler with a hand-made `extension_context`: the link records
// what is asked, the executor is an `io_context` that the test runs by hand,
// the files are read on a thread pool, and the plugin's files are written
// into a scratch folder by the test.  Nothing here starts a process.
//

#include "support/test_support.hpp"

#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/bundle_point.hpp>
#include <vss/plugin/host/bundle_sections.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using vss::plugin::api_message;
using vss::plugin::bundle_file;
using vss::plugin::bundle_prepare_message;
using vss::plugin::bundle_result_message;
using vss::plugin::decode_as;
using vss::plugin::encode_as;
using vss::plugin::error_message;
using vss::plugin::other_message;
using vss::plugin::permission;
using vss::plugin::sha512_of;
using vss::plugin::to_hex;
using vss::plugin::host::bundle_ask;
using vss::plugin::host::bundle_outcome;
using vss::plugin::host::bundle_point;
using vss::plugin::host::bundle_problem;
using vss::plugin::host::extension_context;
using vss::plugin::host::make_bundle_point;
using vss::plugin::host::offered_section;
using vss::plugin::host::section_rules;
using vss::plugin::host::verify_sections;
using vss::testkit::wait_until;
using vss_test_support::scratch_directory;
using namespace std::chrono_literals;

namespace
{

namespace fs = std::filesystem;

constexpr std::string_view plugin_id = "com.example.diag";
constexpr std::string_view section_id = "com.example.diag.log";

struct asked
{
    api_message message;
    vss::plugin::host::plugin_link::answer_handler handler;
};

void write_file(const fs::path& path, std::string_view content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

api_message result_reply(std::vector<bundle_file> files)
{
    bundle_result_message body;
    body.files = std::move(files);
    auto encoded = encode_as(body, 1);
    REQUIRE(encoded.has_value());
    return std::move(encoded).value();
}

bundle_file file_of(std::string name, std::uint64_t size)
{
    bundle_file out;
    out.name = std::move(name);
    out.size = size;
    return out;
}

api_message error_reply()
{
    error_message body;
    body.category = "vss.plugin.api";
    body.value = "refused";
    body.message = "no";
    api_message reply;
    reply.body = std::move(body);
    reply.op = std::string(vss::plugin::op_of(reply.body));
    return reply;
}

/// The handler with everything around it.  The handler goes first: it must
/// not outlive the executor it posts to.
struct rig
{
    explicit rig(std::vector<permission> granted = {permission::bundle_section},
                 std::vector<std::string> reserved = {"traffic"})
        : data(scratch.path() / "data")
    {
        std::error_code made;
        fs::create_directories(data, made);
        extension_context context;
        context.plugin_id = std::string(plugin_id);
        context.plugin_name = "Diagnostics";
        context.plugin_version = "1.0.0";
        context.granted = std::move(granted);
        context.data_directory = data;
        context.executor = io.get_executor();
        context.link.request =
            [this](api_message message,
                   vss::plugin::host::plugin_link::answer_handler reply,
                   std::chrono::milliseconds)
        { requests.push_back({std::move(message), std::move(reply)}); };
        handler = make_bundle_point(context, pool.get_executor(),
                                    std::move(reserved));
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    /// Asks for a section and keeps what `done` is given.
    void ask(std::string section, std::string title = "Log")
    {
        bundle_ask request;
        request.section = std::move(section);
        request.title = std::move(title);
        request.sensitive = true;
        handler->prepare(std::move(request),
                         [this](bundle_outcome outcome)
                         { outcomes.push_back(std::move(outcome)); });
    }

    void answer(std::size_t index, const api_message& reply)
    {
        REQUIRE(index < requests.size());
        requests[index].handler({}, reply);
    }

    /// Runs the strand until `count` requests are over.
    bool until(std::size_t count)
    {
        return wait_until(
            [&]
            {
                io.poll();
                io.restart();
                return outcomes.size() >= count;
            },
            20000ms);
    }

    scratch_directory scratch{};
    fs::path data;
    boost::asio::io_context io{};
    boost::asio::thread_pool pool{2};
    std::vector<asked> requests{};
    std::vector<bundle_outcome> outcomes{};
    std::unique_ptr<bundle_point> handler{};
};

std::optional<bundle_problem> problem_of(const bundle_outcome& outcome)
{
    if (!outcome.failure)
        return std::nullopt;
    return outcome.failure->problem;
}

std::string hash_of(std::string_view content)
{
    return to_hex(sha512_of(content));
}

} // namespace

TEST_CASE("plugin bundle: without bundle.section nothing is asked")
{
    rig fixture({});
    fixture.ask(std::string(section_id));
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{1});
    REQUIRE(!fixture.outcomes[0].prepared);
    REQUIRE(!fixture.outcomes[0].reason.empty());
    REQUIRE(fixture.requests.empty());
}

TEST_CASE("plugin bundle: a section that is not the plugin's is refused")
{
    rig fixture;
    fixture.ask("other");
    fixture.ask("com.example.diag");
    fixture.ask("Bad Id");
    fixture.ask(std::string(section_id), "");
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{4});
    REQUIRE(problem_of(fixture.outcomes[0]) == bundle_problem::id_not_allowed);
    REQUIRE(problem_of(fixture.outcomes[1]) == bundle_problem::id_not_allowed);
    REQUIRE(problem_of(fixture.outcomes[2]) == bundle_problem::id_bad);
    REQUIRE(problem_of(fixture.outcomes[3]) == bundle_problem::title_bad);
    REQUIRE(fixture.requests.empty());
}

TEST_CASE("plugin bundle: a good section is asked for, read and measured")
{
    rig fixture;
    const std::string one = "line one\nline two\n";
    const std::string two(70000, 'b');
    const fs::path folder = fixture.data / "bundles" / std::string(section_id);
    write_file(folder / "log.txt", one);
    write_file(folder / "sub" / "big.bin", two);

    fixture.ask(std::string(section_id));
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
    const auto prepare =
        decode_as<bundle_prepare_message>(fixture.requests[0].message);
    REQUIRE(prepare.has_value());
    REQUIRE_EQ(prepare.value().section, std::string(section_id));
    REQUIRE_EQ(prepare.value().settings, std::string("{}"));

    fixture.answer(0, result_reply({file_of("log.txt", one.size()),
                                    file_of("sub/big.bin", two.size())}));
    REQUIRE(fixture.until(1));
    const bundle_outcome& outcome = fixture.outcomes[0];
    REQUIRE(outcome.prepared.has_value());
    const auto& offered = outcome.prepared->offered;
    REQUIRE_EQ(offered.id, std::string(section_id));
    REQUIRE_EQ(offered.files.size(), std::size_t{2});
    REQUIRE_EQ(offered.files[0].path,
               std::string("bundles/") + std::string(section_id) +
                   "/log.txt");
    REQUIRE_EQ(offered.files[0].size, std::uint64_t{one.size()});
    REQUIRE_EQ(offered.files[0].sha512, hash_of(one));
    REQUIRE_EQ(offered.files[1].sha512, hash_of(two));
    REQUIRE_EQ(outcome.prepared->verified.bytes,
               std::uint64_t{one.size() + two.size()});

    // What was listed passes the check again, as the saving of a bundle
    // does it.
    section_rules rules;
    rules.plugin_id = std::string(plugin_id);
    const std::vector<offered_section> again = {offered};
    REQUIRE(verify_sections(fixture.data, again, rules).has_value());
}

TEST_CASE("plugin bundle: a reserved id is allowed and the settings go "
          "through")
{
    rig fixture;
    bundle_ask request;
    request.section = "traffic";
    request.title = "Recorded traffic";
    request.settings = "{\"ports\":[\"a\"]}";
    write_file(fixture.data / "bundles" / "traffic" / "1.jsonl", "x\n");
    fixture.handler->prepare(std::move(request),
                             [&](bundle_outcome outcome)
                             { fixture.outcomes.push_back(std::move(outcome)); });
    REQUIRE_EQ(fixture.requests.size(), std::size_t{1});
    const auto prepare =
        decode_as<bundle_prepare_message>(fixture.requests[0].message);
    REQUIRE(prepare.has_value());
    REQUIRE_EQ(prepare.value().settings, std::string("{\"ports\":[\"a\"]}"));
    fixture.answer(0, result_reply({file_of("1.jsonl", 2)}));
    REQUIRE(fixture.until(1));
    REQUIRE(fixture.outcomes[0].prepared.has_value());
}

TEST_CASE("plugin bundle: a section of no files is a good section")
{
    rig fixture;
    fixture.ask(std::string(section_id));
    fixture.answer(0, result_reply({}));
    REQUIRE(fixture.until(1));
    const auto& prepared = fixture.outcomes[0].prepared;
    REQUIRE(prepared.has_value());
    REQUIRE(prepared.value().offered.files.empty());
}

TEST_CASE("plugin bundle: a file that is not there, or not as large as "
          "said, is refused")
{
    rig fixture;
    const fs::path folder = fixture.data / "bundles" / std::string(section_id);
    write_file(folder / "real.txt", "12345");

    fixture.ask(std::string(section_id));
    fixture.answer(0, result_reply({file_of("missing.txt", 1)}));
    fixture.ask(std::string(section_id));
    fixture.answer(1, result_reply({file_of("real.txt", 6)}));
    fixture.ask(std::string(section_id));
    fixture.answer(2, result_reply({file_of("real.txt", 4)}));
    REQUIRE(fixture.until(3));
    for (const bundle_outcome& outcome : fixture.outcomes)
    {
        REQUIRE(!outcome.prepared.has_value());
        REQUIRE(!outcome.reason.empty());
    }
    // The three come in any order: the problems are these.
    std::size_t missing = 0;
    std::size_t changed = 0;
    for (const bundle_outcome& outcome : fixture.outcomes)
    {
        missing += problem_of(outcome) == bundle_problem::file_missing;
        changed += problem_of(outcome) == bundle_problem::size_changed;
    }
    REQUIRE_EQ(missing, std::size_t{1});
    REQUIRE_EQ(changed, std::size_t{2});
}

TEST_CASE("plugin bundle: a link in place of a file is never followed")
{
    rig fixture;
    const fs::path outside = fixture.scratch.path() / "outside.txt";
    write_file(outside, "secret");
    const fs::path folder = fixture.data / "bundles" / std::string(section_id);
    write_file(folder / "plain.txt", "plain");
    std::error_code ec;
    fs::create_symlink(outside, folder / "link.txt", ec);
    if (ec)
    {
        VSS_SKIP("symbolic links cannot be made here");
    }
    fixture.ask(std::string(section_id));
    fixture.answer(0, result_reply({file_of("link.txt", 6)}));
    REQUIRE(fixture.until(1));
    REQUIRE(!fixture.outcomes[0].prepared.has_value());
    REQUIRE(problem_of(fixture.outcomes[0]) == bundle_problem::file_is_link);
}

TEST_CASE("plugin bundle: an error or an answer that is not a list is "
          "refused")
{
    rig fixture;
    fixture.ask(std::string(section_id));
    fixture.answer(0, error_reply());
    fixture.ask(std::string(section_id));
    api_message odd;
    odd.op = "bundle.result";
    odd.body = other_message{"{\"files\":\"no\"}"};
    fixture.answer(1, odd);
    fixture.ask(std::string(section_id));
    fixture.requests[2].handler(vss::plugin::make_error_code(
                                    vss::plugin::api_errc::timeout),
                                api_message{});
    REQUIRE(fixture.until(3));
    for (const bundle_outcome& outcome : fixture.outcomes)
    {
        REQUIRE(!outcome.prepared.has_value());
        REQUIRE(!outcome.reason.empty());
    }
}

TEST_CASE("plugin bundle: the plugin going ends what waits, once")
{
    rig fixture;
    fixture.ask(std::string(section_id));
    fixture.ask("traffic");
    REQUIRE(fixture.outcomes.empty());
    fixture.handler->plugin_gone();
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{2});
    // The link answers the requests as it does for a plugin that is gone:
    // nothing is called a second time.
    fixture.answer(0, error_reply());
    fixture.answer(1, error_reply());
    fixture.io.poll();
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{2});
}

TEST_CASE("plugin bundle: the handler going ends what waits")
{
    rig fixture;
    fixture.ask(std::string(section_id));
    REQUIRE(fixture.outcomes.empty());
    fixture.handler.reset();
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{1});
    REQUIRE(!fixture.outcomes[0].prepared.has_value());
    // A late answer does nothing.
    fixture.answer(0, result_reply({}));
    fixture.io.poll();
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{1});
}

TEST_CASE("plugin bundle: the files are read after the handler went and "
          "nothing is called")
{
    rig fixture;
    const fs::path folder = fixture.data / "bundles" / std::string(section_id);
    write_file(folder / "log.txt", "abc");
    fixture.ask(std::string(section_id));
    fixture.answer(0, result_reply({file_of("log.txt", 3)}));
    fixture.handler.reset();
    // The handler said so once, when it went.
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{1});
    fixture.pool.wait();
    fixture.io.poll();
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{1});
}

TEST_CASE("plugin bundle: too many sections at once are refused")
{
    rig fixture;
    for (int i = 0; i < 8; ++i)
        fixture.ask(std::string(section_id));
    REQUIRE_EQ(fixture.requests.size(), std::size_t{8});
    REQUIRE(fixture.outcomes.empty());
    fixture.ask(std::string(section_id));
    REQUIRE_EQ(fixture.requests.size(), std::size_t{8});
    REQUIRE_EQ(fixture.outcomes.size(), std::size_t{1});
    REQUIRE(!fixture.outcomes[0].prepared.has_value());
}
