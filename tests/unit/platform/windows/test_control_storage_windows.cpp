//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audit log's storage on Windows: the access list of the directory is read
// back, which is what another program would see.  It must be protected and
// hold one entry that allows, naming the user who runs the test.  A directory
// in place of the file is refused.
//

#include <vss/control/audit_log.hpp>
#include <vss/core/error.hpp>

#include "platform/private_storage_platform.hpp"
#include "scratch_directory.hpp"
#include "test_framework.hpp"

// windows.h first: the other headers need its types.
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace control = vss::control;
namespace platform = vss::control::platform;

namespace
{

struct handle_closer
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};
using handle_owner = std::unique_ptr<void, handle_closer>;

/// Memory that Windows allocated and `LocalFree` returns.
struct local_releaser
{
    void operator()(void* memory) const noexcept
    {
        if (memory != nullptr)
            ::LocalFree(memory);
    }
};
using local_owner = std::unique_ptr<void, local_releaser>;

/// The user of this process, as the bytes of a TOKEN_USER.
std::vector<unsigned char> current_user()
{
    HANDLE raw = nullptr;
    REQUIRE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw) != 0);
    const handle_owner token(raw);
    DWORD size = 0;
    static_cast<void>(
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size));
    REQUIRE(size > 0);
    std::vector<unsigned char> buffer(size);
    REQUIRE(::GetTokenInformation(token.get(), TokenUser, buffer.data(), size,
                                  &size) != 0);
    return buffer;
}

PSID sid_of(const std::vector<unsigned char>& user)
{
    return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
}

vss::result<std::unique_ptr<control::audit_log>>
try_open(const std::filesystem::path& directory)
{
    control::audit_options options;
    options.directory = directory;
    return control::audit_log::open(std::move(options));
}

} // namespace

TEST_CASE("control storage: the directory is open to this user alone")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    const auto opened = try_open(directory);
    REQUIRE(opened);

    // What another program would see: the owner and the access list.
    const std::wstring name = directory.wstring();
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetNamedSecurityInfoW(
        name.c_str(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, &owner, nullptr,
        &dacl, nullptr, &descriptor);
    const local_owner release(descriptor);
    REQUIRE_EQ(status, DWORD{0});
    REQUIRE(descriptor != nullptr);
    REQUIRE(owner != nullptr);
    REQUIRE(dacl != nullptr);

    // One entry that allows, and it names this user.
    ACL_SIZE_INFORMATION entries{};
    REQUIRE(::GetAclInformation(dacl, &entries, sizeof(entries),
                                AclSizeInformation) != 0);
    REQUIRE_EQ(entries.AceCount, DWORD{1});
    void* found = nullptr;
    REQUIRE(::GetAce(dacl, 0, &found) != 0);
    REQUIRE(found != nullptr);
    const auto* header = static_cast<const ACE_HEADER*>(found);
    REQUIRE_EQ(header->AceType, static_cast<BYTE>(ACCESS_ALLOWED_ACE_TYPE));
    auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(found);
    REQUIRE(allowed->Mask != 0);
    PSID entry_sid = &allowed->SidStart;
    const auto user = current_user();
    REQUIRE(::EqualSid(entry_sid, sid_of(user)) != 0);

    // The list is protected: nothing is inherited into it.
    SECURITY_DESCRIPTOR_CONTROL control_bits = 0;
    DWORD revision = 0;
    REQUIRE(::GetSecurityDescriptorControl(descriptor, &control_bits,
                                           &revision) != 0);
    REQUIRE((control_bits & SE_DACL_PROTECTED) != 0);
}

TEST_CASE("control storage: a directory in place of the file is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    REQUIRE(try_open(directory));

    const auto file = directory / "audit.jsonl";
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);

    const auto opened = try_open(directory);
    REQUIRE(!opened);
}

TEST_CASE("control storage: the directory can be opened again")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "audit";
    {
        const auto first = try_open(directory);
        REQUIRE(first);
        (*first)->record("test.event");
    }
    const auto again = try_open(directory);
    REQUIRE(again);
    (*again)->record("test.event");
    REQUIRE_EQ((*again)->failed_records(), std::uint64_t{0});
    const auto verdict = control::verify_audit_log(directory);
    REQUIRE(verdict);
    REQUIRE(verdict->intact);
    REQUIRE_EQ(verdict->records, std::uint64_t{2});
}

TEST_CASE("control storage: the default directory is named control")
{
    const auto directory = control::default_audit_directory();
    REQUIRE(directory);
    REQUIRE(directory->is_absolute());
    REQUIRE_EQ(directory->filename().string(), "control");
    REQUIRE_EQ(directory->parent_path().filename().string(),
               "Virtual Serial Studio");
}

// -- whole files and random bytes --------------------------------------------

namespace
{

/// Text of every byte value in turn, `size` bytes long: it holds zero bytes and
/// bytes above 127, and is long enough to need more than one read and write.
std::string patterned_bytes(std::size_t size)
{
    std::string bytes(size, '\0');
    for (std::size_t position = 0; position < size; ++position)
        bytes[position] = static_cast<char>(position % 256);
    return bytes;
}

vss::error_code denied()
{
    return vss::make_error_code(vss::errc::permission_denied);
}

bool all_zero(std::span<const std::uint8_t> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

} // namespace

TEST_CASE("control storage: a file is written and read back")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";

    const std::string brief = "{\"version\": 1}";
    REQUIRE(!platform::write_private_file(file, brief));
    const auto read_brief = platform::read_private_file(file, 1024);
    REQUIRE(read_brief);
    REQUIRE_EQ(*read_brief, brief);

    // Bytes of every value, more than one read and one write long.
    const std::string large = patterned_bytes(300 * 1000 + 17);
    REQUIRE(!platform::write_private_file(file, large));
    const auto read_large = platform::read_private_file(file, large.size());
    REQUIRE(read_large);
    REQUIRE(*read_large == large);

    // An empty file is a file.
    REQUIRE(!platform::write_private_file(file, ""));
    const auto read_empty = platform::read_private_file(file, 0);
    REQUIRE(read_empty);
    REQUIRE(read_empty->empty());
}

TEST_CASE("control storage: a second write replaces the file")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";

    REQUIRE(!platform::write_private_file(file, "the first, and the longer"));
    REQUIRE(!platform::write_private_file(file, "the second"));
    const auto content = platform::read_private_file(file, 1024);
    REQUIRE(content);
    REQUIRE_EQ(*content, std::string("the second"));

    // One file is left, and no temporary one.
    std::size_t files = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(scratch.path()))
    {
        ++files;
        REQUIRE_EQ(entry.path().filename().string(), "state.json");
    }
    REQUIRE_EQ(files, std::size_t{1});
    REQUIRE(!std::filesystem::exists(scratch.path() / "state.json.tmp"));
}

TEST_CASE("control storage: a file that is not there is not found")
{
    const vss_test_support::scratch_directory scratch;
    const auto not_found = vss::make_error_code(vss::errc::not_found);

    const auto no_file = platform::read_private_file(scratch.path() / "no", 10);
    REQUIRE(!no_file);
    REQUIRE_EQ(no_file.error(), not_found);

    // The directory is missing too.
    const auto no_directory =
        platform::read_private_file(scratch.path() / "no" / "file", 10);
    REQUIRE(!no_directory);
    REQUIRE_EQ(no_directory.error(), not_found);
}

TEST_CASE("control storage: a file longer than the limit is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";
    const std::string bytes(100, 'x');
    REQUIRE(!platform::write_private_file(file, bytes));

    const auto too_long = platform::read_private_file(file, 99);
    REQUIRE(!too_long);
    REQUIRE_EQ(too_long.error(),
               vss::make_error_code(vss::errc::invalid_argument));

    const auto just_right = platform::read_private_file(file, 100);
    REQUIRE(just_right);
    REQUIRE_EQ(*just_right, bytes);
}

TEST_CASE("control storage: a directory in place of a whole file is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state.json";
    std::filesystem::create_directory(file);

    const auto read = platform::read_private_file(file, 10);
    REQUIRE(!read);
    REQUIRE_EQ(read.error(), denied());

    // The move over a directory fails, and the temporary file is not left.
    REQUIRE(platform::write_private_file(file, "bytes"));
    REQUIRE(std::filesystem::is_directory(file));
    REQUIRE(!std::filesystem::exists(scratch.path() / "state.json.tmp"));
}

TEST_CASE("control storage: a write into a directory that is missing fails")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "no" / "state.json";

    REQUIRE(platform::write_private_file(file, "bytes"));
    REQUIRE(!std::filesystem::exists(scratch.path() / "no"));
}

TEST_CASE("control storage: random bytes fill a buffer and differ each time")
{
    std::array<std::uint8_t, 64> first{};
    std::array<std::uint8_t, 64> second{};
    REQUIRE(!platform::random_bytes(first));
    REQUIRE(!platform::random_bytes(second));

    // 64 random bytes are all zero, or equal to another 64, with a probability
    // of 2 to the power of -512.
    REQUIRE(!all_zero(first));
    REQUIRE(!all_zero(second));
    REQUIRE(first != second);
}

TEST_CASE("control storage: random bytes fill a long buffer to its end")
{
    std::vector<std::uint8_t> bytes(3 * 1000 * 1000 + 5, 0);
    REQUIRE(!platform::random_bytes(bytes));
    const std::span<const std::uint8_t> all(bytes);
    REQUIRE(!all_zero(all.first(64)));
    REQUIRE(!all_zero(all.subspan(all.size() / 2, 64)));
    REQUIRE(!all_zero(all.last(64)));
}

TEST_CASE("control storage: random bytes of no length succeed")
{
    REQUIRE(!platform::random_bytes(std::span<std::uint8_t>()));

    std::array<std::uint8_t, 8> untouched{};
    REQUIRE(
        !platform::random_bytes(std::span<std::uint8_t>(untouched).first(0)));
    REQUIRE(all_zero(untouched));
}
