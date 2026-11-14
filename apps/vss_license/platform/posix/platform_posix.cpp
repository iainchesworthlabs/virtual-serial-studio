//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "platform.hpp"

#include <cstddef>
#include <cstdlib>

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vss::license::tool::platform
{

bool random_bytes(std::span<std::uint8_t> out)
{
    // getentropy gives at most 256 bytes a call. It blocks until the system's
    // generator is seeded and never returns less than was asked.
    constexpr std::size_t most = 256;
    while (!out.empty())
    {
        const std::size_t count = out.size() < most ? out.size() : most;
        if (::getentropy(out.data(), count) != 0)
            return false;
        out = out.subspan(count);
    }
    return true;
}

bool write_private_file(const std::filesystem::path& path,
                        std::string_view text)
{
    // O_EXCL: fail if the name exists, also when it is a link. 0600: the
    // owner only, from the moment the file exists.
    const int file =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
               S_IRUSR | S_IWUSR);
    if (file < 0)
        return false;

    bool written = true;
    while (written && !text.empty())
    {
        const auto count = ::write(file, text.data(), text.size());
        if (count <= 0)
            written = false;
        else
            text.remove_prefix(static_cast<std::size_t>(count));
    }
    if (written && ::fsync(file) != 0)
        written = false;
    if (::close(file) != 0)
        written = false;
    if (!written)
        ::unlink(path.c_str());
    return written;
}

std::optional<std::string> read_variable(const std::string& name)
{
    if (const char* value = std::getenv(name.c_str()))
        return std::string(value);
    return std::nullopt;
}

} // namespace vss::license::tool::platform
