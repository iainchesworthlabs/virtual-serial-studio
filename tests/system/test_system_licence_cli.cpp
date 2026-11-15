//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli licence` as a separate process, the way a person or a script uses
// it.  The unit tests call the command's function with folders of their own;
// these run the program, so a command that `main` refuses before it reaches
// that function fails here.  The program reads the licence folders of this
// computer, so only what holds on every computer is checked, and the one
// import is of a file that is not a licence, which copies nothing.
//

#include "child_process.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <vector>

using vss_test_support::child_process;
using vss_test_support::scratch_directory;

namespace
{

constexpr std::chrono::milliseconds long_limit{20000};

/// How the command ended, and what it printed to either stream.
struct cli_result
{
    std::optional<int> code;
    std::string output;
};

cli_result run_licence(const std::vector<std::string>& words)
{
    std::vector<std::string> arguments{"licence"};
    arguments.insert(arguments.end(), words.begin(), words.end());
    child_process child(VSS_CLI_PROGRAM, arguments);
    cli_result result;
    result.output = child.read_all(long_limit);
    result.code = child.wait(long_limit);
    return result;
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("licence cli: the program runs licence and prints the edition")
{
    const cli_result result = run_licence({});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(result.code.value_or(-1), 0);
    REQUIRE(!has(result.output, "unknown command"));
    // Which edition and state depends on the computer; that both lines are
    // there does not.
    REQUIRE(result.output.starts_with("Edition: "));
    REQUIRE(has(result.output, "\nState: "));
    REQUIRE(has(result.output, "\nFiles:"));
}

TEST_CASE("licence cli: the program refuses to import a file that is not a "
          "licence")
{
    const scratch_directory scratch;
    const std::filesystem::path file = scratch.path() / "not-a-licence.vsslic";
    {
        std::ofstream out(file, std::ios::binary);
        REQUIRE(out.is_open());
        out << "this is not a licence\n";
    }

    const cli_result result = run_licence({"import", file.string()});
    REQUIRE(result.code.has_value());
    REQUIRE_EQ(result.code.value_or(-1), 1);
    REQUIRE(!has(result.output, "unknown command"));
    REQUIRE(has(result.output, "Not imported: "));
    // The original is left where it was.
    REQUIRE(std::filesystem::exists(file));

    const cli_result missing =
        run_licence({"import", (scratch.path() / "no-such-file").string()});
    REQUIRE_EQ(missing.code.value_or(-1), 1);
    REQUIRE(has(missing.output, "Not imported: "));
}

TEST_CASE("licence cli: a command line it does not understand exits with 2")
{
    const std::vector<std::vector<std::string>> mistakes = {
        {"frobnicate"},
        {"import"},
        {"import", "one", "two"},
    };
    for (const std::vector<std::string>& words : mistakes)
    {
        const cli_result result = run_licence(words);
        REQUIRE(result.code.has_value());
        REQUIRE_EQ(result.code.value_or(-1), 2);
        // The licence command's own usage, not main's "unknown command
        // licence".
        REQUIRE(has(result.output, "vss_cli licence import <file>"));
    }
}
