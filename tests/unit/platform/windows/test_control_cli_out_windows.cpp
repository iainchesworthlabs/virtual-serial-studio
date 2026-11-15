//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The file that `vss_cli control token create --out` makes, with its access
// list read back as another program on this computer would see it; and a
// place that cannot be made private, which is refused with nothing left.
//

#include <vss/control/private_file.hpp>
#include <vss/core/error.hpp>

#include "cli_gateway.hpp"
#include "test_framework.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <aclapi.h>
// clang-format on

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace control = vss::control;

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;

struct handle_closer
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};

struct local_releaser
{
    void operator()(void* memory) const noexcept
    {
        if (memory != nullptr)
            ::LocalFree(memory);
    }
};

/// True when the access list of `file` is protected and has one entry that
/// allows, naming the user who runs this process.
bool open_to_this_user_alone(const std::filesystem::path& file)
{
    HANDLE raw = nullptr;
    REQUIRE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) != 0);
    const std::unique_ptr<void, handle_closer> token(raw);
    DWORD size = 0;
    static_cast<void>(
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size));
    REQUIRE(size > 0);
    std::vector<unsigned char> user(size);
    REQUIRE(::GetTokenInformation(token.get(), TokenUser, user.data(), size,
                                  &size) != 0);

    const std::wstring name = file.wstring();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
        nullptr, &dacl, nullptr, &descriptor);
    const std::unique_ptr<void, local_releaser> release(descriptor);
    REQUIRE_EQ(status, DWORD{0});
    REQUIRE(dacl != nullptr);

    SECURITY_DESCRIPTOR_CONTROL bits = 0;
    DWORD revision = 0;
    REQUIRE(::GetSecurityDescriptorControl(descriptor, &bits, &revision) != 0);
    ACL_SIZE_INFORMATION entries{};
    REQUIRE(::GetAclInformation(dacl, &entries, sizeof(entries),
                                AclSizeInformation) != 0);
    if ((bits & SE_DACL_PROTECTED) == 0 || entries.AceCount != 1)
        return false;
    void* found = nullptr;
    REQUIRE(::GetAce(dacl, 0, &found) != 0);
    if (static_cast<const ACE_HEADER*>(found)->AceType !=
        ACCESS_ALLOWED_ACE_TYPE)
        return false;
    PSID entry = &static_cast<ACCESS_ALLOWED_ACE*>(found)->SidStart;
    return ::EqualSid(
               entry,
               reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid) != 0;
}

std::string read_all(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("control cli: the file of --out is open to this user alone")
{
    const vss_test_support::cli_gateway rig;

    // A file that is made in this directory the usual way takes the access
    // list of the directory, which is open to more than this user.
    const auto ordinary = rig.directory() / "ordinary.txt";
    {
        std::ofstream out(ordinary, std::ios::binary);
        out << "x";
    }
    REQUIRE(!open_to_this_user_alone(ordinary));

    const auto file = rig.directory() / "rig.token";
    REQUIRE_EQ(rig.run({"token", "create", "--label=rig", "--role=monitor",
                        "--out=" + file.string()}),
               exit_ok);
    REQUIRE(open_to_this_user_alone(file));
}

TEST_CASE("control cli: a place that cannot be made private is refused, with "
          "no token and nothing left")
{
    const vss_test_support::cli_gateway rig;

    // A stream on a file that exists is a place whose access list is that of
    // the file, which here is the directory's.  The list is read back before
    // anything is written, and the stream is removed again.
    const auto base = rig.directory() / "ordinary.txt";
    {
        std::ofstream out(base, std::ios::binary);
        out << "kept";
    }
    REQUIRE(!open_to_this_user_alone(base));
    const std::filesystem::path stream = base.wstring() + L":token";

    const auto claim = control::claim_private_file(stream);
    REQUIRE(!claim);
    REQUIRE_EQ(claim.error(),
               vss::make_error_code(vss::errc::permission_denied));
    REQUIRE_EQ(read_all(base), std::string("kept"));
    {
        std::ifstream gone(stream, std::ios::binary);
        REQUIRE(!gone);
    }

    // The command makes no token for it and shows nothing.
    REQUIRE_EQ(rig.run({"token", "create", "--label=rig", "--role=monitor",
                        "--out=" + stream.string()}),
               exit_refused);
    REQUIRE_EQ(rig.token_count(), std::size_t{0});
    REQUIRE_EQ(read_all(base), std::string("kept"));
}
