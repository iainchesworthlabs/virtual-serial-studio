//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/proto/frame.hpp>
#include <vss/plugin/api_message.hpp>

#include "plugin_api_support.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace vss;
using namespace vss::plugin;
using namespace vss::plugin_api_test;

namespace
{

namespace fs = std::filesystem;

/// The folder of the repository, from the folder of the test data.
fs::path repository()
{
    return fs::path(VSS_TEST_DATA_DIR).parent_path().parent_path();
}

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// The name of the seed of a message: `fixture.line.set` is
/// `message_api_fixture_line_set.bin`.
std::string seed_name(std::string_view op)
{
    std::string out = "message_api_";
    for (const char c : op)
        out += c == '.' ? '_' : c;
    out += ".bin";
    return out;
}

} // namespace

TEST_CASE("plugin api: every message is listed once, with its own role")
{
    const std::span<const api_op> ops = api_ops();
    REQUIRE_EQ(ops.size(), std::size_t{40});
    for (std::size_t i = 0; i < ops.size(); ++i)
    {
        REQUIRE(is_op_name(ops[i].op));
        REQUIRE(ops[i].check != nullptr);
        REQUIRE(ops[i].recode != nullptr);
        const api_op* const found = find_api_op(ops[i].op);
        REQUIRE(found == &ops[i]);
        for (std::size_t j = 0; j < i; ++j)
            REQUIRE(ops[i].op != ops[j].op);
    }
    REQUIRE(find_api_op("hello") == nullptr);
    REQUIRE(find_api_op("no.such") == nullptr);
    REQUIRE(find_api_op("") == nullptr);
}

TEST_CASE("plugin api: every message has a seed for the fuzz harness")
{
    const fs::path corpus = repository() / "libs/vss_plugin/fuzz/corpus";
    if (!fs::exists(corpus))
        VSS_SKIP("the corpus is not beside the tests");
    for (const api_op& entry : api_ops())
    {
        const fs::path path = corpus / seed_name(entry.op);
        const std::string bytes = read_file(path);
        if (bytes.empty())
            throw vss_test::failure("no seed for " + std::string(entry.op));
        agent::frame_reader reader;
        reader.feed(vss::bytes_view(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        auto next = reader.next();
        REQUIRE(next.has_value());
        const std::optional<agent::frame>& frame = next.value();
        REQUIRE(frame.has_value());
        const api_message message = message_of(frame->header, frame->blob);
        REQUIRE_EQ(message.op, std::string(entry.op));
        std::string detail;
        const auto problem = entry.check(message, &detail);
        if (problem)
            throw vss_test::failure("the seed of " + std::string(entry.op) +
                                    " is refused: " + detail);
        // The role of the seed is the role of the message.
        switch (entry.role)
        {
        case api_role::request:
            REQUIRE(message.id != 0 && message.re == 0);
            break;
        case api_role::notification:
            REQUIRE(message.id == 0 && message.re == 0);
            break;
        case api_role::answer:
            REQUIRE(message.id == 0 && message.re != 0);
            break;
        }
    }
}

TEST_CASE("plugin api: the reference page lists every message")
{
    const fs::path page = repository() / "docs/plugin-api-reference.md";
    if (!fs::exists(page))
        VSS_SKIP("the documents are not beside the tests");
    const std::string text = read_file(page);
    for (const api_op& entry : api_ops())
    {
        const std::string cell = "| `" + std::string(entry.op) + "` | " +
                                 std::string(to_string(entry.from)) + " | " +
                                 std::string(to_string(entry.role)) + " |";
        if (text.find(cell) == std::string::npos)
            throw vss_test::failure("the reference has no row that starts " +
                                    cell);
    }
}
