//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What vss_license asks of the operating system. CMake picks the file that
// answers: platform/windows or platform/posix.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vss::license::tool::platform
{

/// Fills `out` from the operating system's random source for keys. False
/// when the system could not supply it; `out` is then not to be used.
bool random_bytes(std::span<std::uint8_t> out);

/// Makes a new file that only its owner can read or write, and writes `text`
/// to it. False when the file exists already or cannot be made; an existing
/// file is never touched.
bool write_private_file(const std::filesystem::path& path,
                        std::string_view text);

/// Reads a file straight into `out`, with no stream and so no buffer of the
/// library's own that keeps a copy. It is for a file that holds a private key.
/// Returns the number of bytes read, which is `out.size()` when the file is
/// at least that long, or nothing when the file cannot be opened or read.
std::optional<std::size_t> read_file_into(const std::filesystem::path& path,
                                          std::span<char> out);

std::optional<std::string> read_variable(const std::string& name);

} // namespace vss::license::tool::platform
