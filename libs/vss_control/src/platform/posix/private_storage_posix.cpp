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
//   replace    the whole file: written to `<path>.tmp` (O_TRUNC, mode 0600, the
//              same checks), flushed with fsync and renamed over the target;
//              the temporary file is removed when anything fails.  Reading
//              uses the same open flags and the same checks on the descriptor.
//

#include "platform/private_storage_platform.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
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

/// Writes all of `bytes` to a descriptor, again after a signal and after a
/// short write.
error_code write_all(int descriptor, std::string_view bytes)
{
    std::size_t done = 0;
    while (done < bytes.size())
    {
        const ssize_t written =
            ::write(descriptor, bytes.data() + done, bytes.size() - done);
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

/// Says whether an open file is a regular file of this user that nobody else
/// can read or change.
error_code check_private_file(const struct stat& info)
{
    if (!S_ISREG(info.st_mode))
        return make_error_code(errc::permission_denied);
    if (info.st_uid != ::geteuid())
        return make_error_code(errc::permission_denied);
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        return make_error_code(errc::permission_denied);
    return {};
}

/// Makes `temporary` (emptying it when it exists), writes `bytes` and flushes
/// them to the disk.  The file is closed when this returns.
error_code write_temporary_file(const std::string& temporary,
                                std::string_view bytes)
{
    const descriptor_owner descriptor(
        ::open(temporary.c_str(),
               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR));
    if (descriptor.get() < 0)
        return system_error(errno);

    struct stat info{};
    if (::fstat(descriptor.get(), &info) != 0)
        return system_error(errno);
    if (const error_code refused = check_private_file(info))
        return refused;

    if (const error_code failed = write_all(descriptor.get(), bytes))
        return failed;
    if (::fsync(descriptor.get()) != 0)
        return system_error(errno);
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
        return write_all(descriptor_.get(), bytes);
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
    if (const error_code refused = check_private_file(info))
        return refused;

    return std::unique_ptr<private_file>(
        std::make_unique<posix_file>(std::move(descriptor)));
}

error_code write_private_file(const std::filesystem::path& path,
                              std::string_view bytes)
{
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    const std::string temporary_text = temporary.string();
    const std::string path_text = path.string();

    error_code failed = write_temporary_file(temporary_text, bytes);
    if (!failed && ::rename(temporary_text.c_str(), path_text.c_str()) != 0)
        failed = system_error(errno);
    // Whatever went wrong, no half-written file is left beside the target.  If
    // the temporary name was a link, the link is removed and not what it
    // points to.
    if (failed)
        static_cast<void>(::unlink(temporary_text.c_str()));
    return failed;
}

result<std::string> read_private_file(const std::filesystem::path& path,
                                      std::size_t limit)
{
    const std::string text = path.string();
    const descriptor_owner descriptor(
        ::open(text.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (descriptor.get() < 0)
    {
        const int failed = errno;
        if (failed == ENOENT)
            return make_error_code(errc::not_found);
        // O_NOFOLLOW refuses a link with ELOOP.
        if (failed == ELOOP)
            return make_error_code(errc::permission_denied);
        return system_error(failed);
    }

    struct stat info{};
    if (::fstat(descriptor.get(), &info) != 0)
        return system_error(errno);
    if (const error_code refused = check_private_file(info))
        return refused;
    if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > limit)
        return make_error_code(errc::invalid_argument);

    // The file may grow after it was measured, so the limit is kept while
    // reading too.
    constexpr std::size_t chunk_size = std::size_t{64} * 1024;
    std::string content;
    for (;;)
    {
        const std::size_t used = content.size();
        content.resize(used + chunk_size);
        const ssize_t got =
            ::read(descriptor.get(), content.data() + used, chunk_size);
        if (got < 0)
        {
            const int failed = errno;
            if (failed == EINTR)
            {
                content.resize(used);
                continue;
            }
            return system_error(failed);
        }
        content.resize(used + static_cast<std::size_t>(got));
        if (got == 0)
            return content;
        if (content.size() > limit)
            return make_error_code(errc::invalid_argument);
    }
}

} // namespace vss::control::platform
