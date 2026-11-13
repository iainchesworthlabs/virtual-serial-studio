//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private storage on Linux and macOS: a directory with mode 0700 and files with
// mode 0600, both owned by the user who runs the gateway.
//
//   directory  made with mode 0700 and checked on every use: a directory that
//              is a link, belongs to someone else or is open to group or
//              others is refused with `errc::permission_denied`.
//   file       opened with O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW and
//              mode 0600, then checked on the open descriptor: a file that is
//              not a regular file, belongs to someone else or is open to group
//              or others is refused.  A link in place of the file is not
//              followed.
//

#include "platform/private_storage_platform.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace vss::control::platform
{
namespace
{

error_code system_error(int value)
{
    return {value, boost::system::system_category()};
}

/// A file descriptor that is closed with its owner.
class descriptor_owner
{
public:
    descriptor_owner() = default;

    explicit descriptor_owner(int descriptor) noexcept : descriptor_(descriptor)
    {
    }

    descriptor_owner(descriptor_owner&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }

    ~descriptor_owner()
    {
        if (descriptor_ >= 0)
            static_cast<void>(::close(descriptor_));
    }

    descriptor_owner(const descriptor_owner&) = delete;
    descriptor_owner& operator=(const descriptor_owner&) = delete;
    descriptor_owner& operator=(descriptor_owner&&) = delete;

    int get() const noexcept { return descriptor_; }

private:
    int descriptor_ = -1;
};

/// Says whether `directory` is a real directory of this user that nobody else
/// can enter, read or change.  `lstat`, not `stat`: a link in place of the
/// directory is refused.
error_code check_private_directory(const std::filesystem::path& directory)
{
    const std::string text = directory.string();
    struct stat info{};
    if (::lstat(text.c_str(), &info) != 0)
        return system_error(errno);
    if (!S_ISDIR(info.st_mode))
        return make_error_code(errc::permission_denied);
    if (info.st_uid != ::geteuid())
        return make_error_code(errc::permission_denied);
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return make_error_code(errc::permission_denied);
    return {};
}

class posix_file final : public private_file
{
public:
    explicit posix_file(descriptor_owner descriptor)
        : descriptor_(std::move(descriptor))
    {
    }

    error_code append(std::string_view bytes) override
    {
        std::size_t done = 0;
        while (done < bytes.size())
        {
            const ssize_t written = ::write(
                descriptor_.get(), bytes.data() + done, bytes.size() - done);
            if (written < 0)
            {
                const int failed = errno;
                if (failed == EINTR)
                    continue;
                return system_error(failed);
            }
            if (written == 0)
                return make_error_code(errc::io_error);
            done += static_cast<std::size_t>(written);
        }
        return {};
    }

private:
    descriptor_owner descriptor_;
};

} // namespace

/// Makes the directory when it is not there, then checks it.
error_code ensure_private_directory(const std::filesystem::path& directory)
{
    const std::string text = directory.string();

    // Each missing parent gets the default mode; the last component is closed
    // to everyone but the owner.
    std::error_code ignored;
    if (directory.has_parent_path())
        std::filesystem::create_directories(directory.parent_path(), ignored);

    if (::mkdir(text.c_str(), S_IRWXU) != 0 && errno != EEXIST)
        return system_error(errno);
    return check_private_directory(directory);
}

result<std::unique_ptr<private_file>>
open_private_append(const std::filesystem::path& path)
{
    const std::string text = path.string();
    descriptor_owner descriptor(::open(
        text.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR));
    if (descriptor.get() < 0)
        return system_error(errno);

    // A file that was there before is checked on the descriptor, so nothing
    // can be swapped in between the check and the use.
    struct stat info{};
    if (::fstat(descriptor.get(), &info) != 0)
        return system_error(errno);
    if (!S_ISREG(info.st_mode))
        return make_error_code(errc::permission_denied);
    if (info.st_uid != ::geteuid())
        return make_error_code(errc::permission_denied);
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return make_error_code(errc::permission_denied);

    return std::unique_ptr<private_file>(
        std::make_unique<posix_file>(std::move(descriptor)));
}

} // namespace vss::control::platform
