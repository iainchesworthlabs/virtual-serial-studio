//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The file that `vss_cli control token create --out` makes, read back with
// `lstat` as another program on this computer would see it.
//

#include "cli_gateway.hpp"
#include "test_framework.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace
{

constexpr int exit_ok = 0;

struct stat status_of(const std::filesystem::path& path)
{
    struct stat info{};
    REQUIRE(::lstat(path.c_str(), &info) == 0);
    return info;
}

} // namespace

TEST_CASE("control cli: the file of --out is a regular file of this user with "
          "mode 0600")
{
    const vss_test_support::cli_gateway rig;
    const auto file = rig.directory() / "rig.token";
    REQUIRE_EQ(rig.run({"token", "create", "--label=rig", "--role=monitor",
                        "--out=" + file.string()}),
               exit_ok);

    const struct stat info = status_of(file);
    REQUIRE(S_ISREG(info.st_mode));
    REQUIRE_EQ(info.st_uid, ::geteuid());
    REQUIRE_EQ(static_cast<unsigned>(info.st_mode & 07777), 0600U);
}

TEST_CASE("control cli: the file of --out is 0600 with a umask that takes "
          "nothing away")
{
    const vss_test_support::cli_gateway rig;
    const auto file = rig.directory() / "rig.token";

    // The directory is an ordinary one, and a file that is made with the
    // usual mode (0666) would be open to everyone here.
    const mode_t before = ::umask(0);
    const int code = rig.run({"token", "create", "--label=rig",
                              "--role=monitor", "--out=" + file.string()});
    static_cast<void>(::umask(before));
    REQUIRE_EQ(code, exit_ok);
    REQUIRE_EQ(static_cast<unsigned>(status_of(file).st_mode & 07777), 0600U);
}
