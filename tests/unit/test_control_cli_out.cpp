//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli control token create --out=<file>`, run in the test's process
// against a gateway of the test's own: the file is made before the token,
// a name that is taken makes no token, and a token that cannot be written is
// revoked.  Who can read the file the command makes is read back in
// unit/platform/<windows|posix>/test_control_cli_out_*.cpp.
//

#include <vss/control/client.hpp>
#include <vss/control/private_file.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/control/token_to_file.hpp>
#include <vss/core/error.hpp>

#include "cli_gateway.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace control = vss::control;
using vss_test_support::cli_gateway;

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;

std::string read_all(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// A claim that is made by hand: it has no file, and its write fails.
class failing_claim final : public control::private_file_claim
{
public:
    explicit failing_claim(bool* removed) : removed_(removed) {}

    ~failing_claim() override
    {
        if (!kept_)
            *removed_ = true;
    }

    vss::error_code write(std::string_view) override
    {
        return vss::make_error_code(vss::errc::io_error);
    }

    void keep() noexcept override { kept_ = true; }

private:
    bool* removed_;
    bool kept_ = false;
};

/// A claim whose write is kept in a string.
class recording_claim final : public control::private_file_claim
{
public:
    vss::error_code write(std::string_view bytes) override
    {
        written.append(bytes);
        return {};
    }

    void keep() noexcept override {}

    std::string written;
};

std::unique_ptr<control::client> admin_of(const cli_gateway& rig)
{
    control::client_options options;
    options.instance = rig.instance();
    options.name = "test";
    options.admin = true;
    auto connected = control::client::connect(options);
    REQUIRE(connected);
    return std::move(*connected);
}

} // namespace

TEST_CASE("control cli: --out makes a file that holds the token and a line "
          "ending")
{
    const cli_gateway rig;
    const auto file = rig.directory() / "rig.token";

    REQUIRE_EQ(rig.run({"token", "create", "--label=rig", "--role=monitor",
                        "--out=" + file.string()}),
               exit_ok);
    const std::string text = read_all(file);
    REQUIRE_EQ(text.size(), control::token_size + 1);
    REQUIRE(text.ends_with('\n'));
    REQUIRE(control::parse_token(
                control::secret_string(text.substr(0, control::token_size)))
                .has_value());
    REQUIRE_EQ(rig.token_count(), std::size_t{1});
}

TEST_CASE("control cli: a second run with the same --out makes no token and "
          "leaves the file as it was")
{
    const cli_gateway rig;
    const auto file = rig.directory() / "rig.token";
    REQUIRE_EQ(rig.run({"token", "create", "--label=first", "--role=monitor",
                        "--out=" + file.string()}),
               exit_ok);
    const std::string first = read_all(file);
    REQUIRE_EQ(rig.token_count(), std::size_t{1});

    REQUIRE_EQ(rig.run({"token", "create", "--label=second", "--role=monitor",
                        "--out=" + file.string()}),
               exit_refused);
    // No token was made, not even one that was revoked again.
    REQUIRE_EQ(rig.token_count(), std::size_t{1});
    REQUIRE_EQ(read_all(file), first);
}

TEST_CASE("control cli: a place that cannot hold the file makes no token")
{
    const cli_gateway rig;
    const auto missing = rig.directory() / "no such folder" / "rig.token";
    REQUIRE_EQ(rig.run({"token", "create", "--label=lost", "--role=monitor",
                        "--out=" + missing.string()}),
               exit_refused);
    REQUIRE_EQ(rig.token_count(), std::size_t{0});

    const auto directory = rig.directory() / "folder";
    std::filesystem::create_directory(directory);
    REQUIRE_EQ(rig.run({"token", "create", "--label=lost", "--role=monitor",
                        "--out=" + directory.string()}),
               exit_refused);
    REQUIRE_EQ(rig.token_count(), std::size_t{0});
    REQUIRE(std::filesystem::is_directory(directory));
}

TEST_CASE("control cli: a token that cannot be written is revoked and the "
          "file is dropped")
{
    const cli_gateway rig;
    const auto admin = admin_of(rig);

    bool removed = false;
    failing_claim failing(&removed);
    const auto outcome = control::create_token_to_file(
        *admin, "unwritten", {"monitor"}, 0, {}, failing);
    REQUIRE(outcome);
    REQUIRE(static_cast<bool>(outcome->write_error));
    REQUIRE(outcome->revoked);
    // The record is there, revoked, so no live token is left that nobody holds.
    REQUIRE_EQ(rig.token_count(), std::size_t{1});
    REQUIRE_EQ(rig.live_token_count(), std::size_t{0});
    REQUIRE(!removed); // the claim is the caller's: it drops it
}

TEST_CASE("control cli: a token that is written goes to the file and nowhere "
          "else")
{
    const cli_gateway rig;
    const auto admin = admin_of(rig);

    recording_claim recording;
    const auto outcome = control::create_token_to_file(
        *admin, "written", {"monitor"}, 0, {}, recording);
    REQUIRE(outcome);
    REQUIRE(!outcome->write_error);
    REQUIRE(!outcome->revoked);
    REQUIRE_EQ(recording.written.size(), control::token_size + 1);
    REQUIRE_EQ(rig.live_token_count(), std::size_t{1});

    // The token is good: it is the secret that the gateway stored.
    const control::secret_string token(
        recording.written.substr(0, control::token_size));
    const auto parts = control::parse_token(token);
    REQUIRE(parts.has_value());
    REQUIRE_EQ(parts->id, outcome->entry.id);
}

TEST_CASE("control cli: a claim that is dropped removes its file, and one "
          "that is kept does not")
{
    const vss_test_support::scratch_directory scratch;
    const auto dropped = scratch.path() / "dropped.token";
    const auto kept = scratch.path() / "kept.token";
    {
        auto claim = control::claim_private_file(dropped);
        REQUIRE(claim);
        REQUIRE(std::filesystem::exists(dropped));
        REQUIRE_EQ(std::filesystem::file_size(dropped), std::uintmax_t{0});
        REQUIRE(!(*claim)->write("partly written"));
    }
    REQUIRE(!std::filesystem::exists(dropped));
    {
        auto claim = control::claim_private_file(kept);
        REQUIRE(claim);
        REQUIRE(!(*claim)->write("whole\n"));
        (*claim)->keep();
    }
    REQUIRE_EQ(read_all(kept), std::string("whole\n"));
}
