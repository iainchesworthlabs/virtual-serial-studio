//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::control
{

/// One named value of an audit record.  There is no constructor from a secret
/// and there never may be one: a token, a key or the bytes of traffic are not
/// audit data (docs/remote-control.md, section 11).
struct audit_field
{
    using value_type =
        std::variant<std::string, std::uint64_t, std::int64_t, bool>;

    audit_field(std::string_view name, std::string_view text)
        : key(name), value(std::string(text))
    {
    }
    audit_field(std::string_view name, const char* text)
        : key(name), value(std::string(text))
    {
    }
    audit_field(std::string_view name, std::uint64_t number)
        : key(name), value(number)
    {
    }
    audit_field(std::string_view name, std::int64_t number)
        : key(name), value(number)
    {
    }
    audit_field(std::string_view name, bool flag) : key(name), value(flag) {}

    std::string key;
    value_type value;
};

struct audit_options
{
    /// Where the files are kept.  It is made when it is missing, private to
    /// this user, and refused when it is someone else's or open to others.
    std::filesystem::path directory;
    /// A file that would grow past this is closed and a new one started.
    std::uintmax_t max_file_size = std::uintmax_t{10} * 1024 * 1024;
    /// Files kept, the current one included.  The oldest is removed.
    std::size_t max_files = 5;
};

/// The gateway's record of who connected and what they did: one JSON object
/// on each line of `audit.jsonl`, in a directory only this user can read.
///
///     {"seq":12,"time":"2026-11-13T09:30:00.123Z","event":"session.opened",
///      "principal":"local:1000","prev":"<64 hex digits>"}
///
/// `seq` counts the records from 1 and never restarts, `time` is UTC, and
/// `prev` is the SHA-256 of the bytes of the line before, without its line
/// end (64 zeros on the very first line), so a line that was removed, changed
/// or moved is found by `verify_audit_log`.  That is evidence, not
/// protection: whoever can write the file can rewrite all of it.
///
/// When `audit.jsonl` is full it becomes `audit.1.jsonl`, the one before
/// `audit.2.jsonl` and so on, and the chain goes on across the files.
///
/// Nothing is sent anywhere.  A record never holds a secret or the bytes of
/// traffic; text from a client is stored as data: made valid UTF-8, cut to
/// 1024 bytes and escaped by the JSON writer, so it cannot start a line of
/// its own.
class audit_log
{
public:
    /// Opens the log in `options.directory` and goes on from its last line.
    /// `errc::permission_denied` when the directory is not private to this
    /// user.
    static result<std::unique_ptr<audit_log>> open(audit_options options);

    ~audit_log();
    audit_log(const audit_log&) = delete;
    audit_log& operator=(const audit_log&) = delete;

    /// Appends one record and hands it to the operating system before it
    /// returns.  Safe from any thread.  A record that cannot be written is
    /// counted, never thrown: the audit log must not be a way to stop the
    /// gateway.  `fields` may not use the keys `seq`, `time`, `event` and
    /// `prev`; one that does is dropped.
    void record(std::string_view event,
                std::vector<audit_field> fields = {}) noexcept;

    /// Records that could not be written since the log was opened.
    std::uint64_t failed_records() const noexcept;

    /// The file that is being written.
    std::filesystem::path current_file() const;

private:
    class impl;
    explicit audit_log(std::unique_ptr<impl> state);
    std::unique_ptr<impl> impl_;
};

/// What `verify_audit_log` found.
struct audit_verdict
{
    /// Every line is a record, the numbers follow each other and every
    /// `prev` is the hash of the line before.
    bool intact = false;
    /// Records read, over all the files.
    std::uint64_t records = 0;
    /// When not intact: the file and the 1-based line where the chain breaks,
    /// and what is wrong with it, for a person to read.
    std::filesystem::path file;
    std::uint64_t line = 0;
    std::string problem;
};

/// Reads every audit file in `directory`, oldest first, and checks the chain.
/// The first record of the oldest file is taken on trust, because the file
/// before it may have been removed by rotation.  An empty or missing log is
/// intact with no records.
result<audit_verdict> verify_audit_log(const std::filesystem::path& directory);

/// The directory the gateway uses when it is given none:
/// `%LOCALAPPDATA%\Virtual Serial Studio\control` on Windows,
/// `$XDG_STATE_HOME/virtual-serial-studio/control` (or
/// `~/.local/state/...`) on Linux and
/// `~/Library/Application Support/Virtual Serial Studio/control` on macOS.
result<std::filesystem::path> default_audit_directory();

} // namespace vss::control
