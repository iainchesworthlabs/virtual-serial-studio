//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence files on this computer: where they are, which one Studio uses,
// importing and removing (docs/licensing.md, "Where Studio finds a licence").
// It reads files and judges them with the functions of license.hpp; it has no
// network code and no Qt. Everything it depends on is passed in, so a test
// gives it folders, keys and a clock of its own.
//

#pragma once

#include <vss/license/license.hpp>
#include <vss/license/signed_file.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::license
{

/// The extension of a licence file and of a revocation list, without the dot.
inline constexpr std::string_view licence_suffix = "vsslic";
inline constexpr std::string_view revocations_suffix = "vssrev";

/// No more files than this are read from one folder, in name order.
inline constexpr std::size_t max_files_per_folder = 64;

struct store_options
{
    /// Where an import copies a licence. Made when the first file is
    /// imported. May be empty: nothing is read from it and nothing imported.
    std::filesystem::path user_directory;
    /// Where an administrator puts a site licence. Only read. May be empty.
    std::filesystem::path machine_directory;
    /// The file that keeps the latest time seen and the clock resets used.
    /// Empty keeps them in memory only.
    std::filesystem::path clock_file;
    /// The keys that may sign a licence or a revocation list.
    std::vector<signed_file::trusted_key> keys;
    /// When the program was built: a time that has certainly passed.
    time_point build_time = time_point::min();
    /// What a licence's `machines` is compared with.
    std::string computer_name;
    /// The time now. Empty uses the system clock.
    std::function<time_point()> clock;
};

/// One licence file found in a licence folder.
struct found_licence
{
    license::status status;
    /// The file's name without its folder, in UTF-8.
    std::string file_name;
    /// In the per-machine folder: an administrator put it there, and the
    /// store only reads it.
    bool machine_wide = false;
    /// The file the program is using now.
    bool in_use = false;
};

/// The licence in use and the other files found.
struct store_view
{
    /// `state::none` when there is no licence: the Community edition.
    license::status status = no_licence();
    std::string computer_name;
    /// Every file found, the one in use first. Empty for the Community
    /// edition with no files.
    std::vector<found_licence> files;
};

enum class import_outcome
{
    /// The file was copied to the user's licence folder. `status` says what
    /// it is worth: it may be valid, ended or not started yet.
    imported,
    /// The same file is installed already. Nothing was copied.
    already_installed,
    /// The file is not a licence this program accepts. Nothing was copied.
    /// `status.reason` says why.
    not_valid,
    /// The file could not be read, or the copy could not be written. Nothing
    /// was copied.
    unreadable
};

struct import_result
{
    import_outcome outcome = import_outcome::unreadable;
    /// The file's own status. For `not_valid` the reason; `no_licence()` for
    /// `unreadable`.
    license::status status = no_licence();
    /// The library's message for a file that was refused. For "Copy
    /// details"; never the explanation a person is given.
    std::string detail;
};

class store
{
public:
    explicit store(store_options options);

    /// Reads the folders again and notes the time. Returns true when the
    /// view changed.
    bool refresh();

    /// What `refresh` last found.
    const store_view& view() const noexcept { return view_; }

    /// Checks the file at `path` and, when its signature verifies and
    /// nothing refuses it, copies it to the user's folder under a name that
    /// is new. The original is left where it was and no file is replaced.
    /// Calls `refresh` when something was copied.
    import_result import_file(const std::filesystem::path& path);

    /// Removes the file `file_name` (a `found_licence::file_name`) from the
    /// user's folder. A file in the per-machine folder is never touched, and
    /// a name that is not in the user's folder's own listing is not used as
    /// a path at all. Returns true when a file was removed, and then has
    /// called `refresh`.
    bool remove(const std::string& file_name);

private:
    context judging_context(time_point now) const;
    void load_clock();
    void save_clock() const;
    void load_revocations();

    store_options options_;
    clock_state clock_;
    revocation_list revocations_;
    store_view view_;
};

/// The per-machine licence folder of this system. It holds the licences an
/// administrator installs for every user:
///
///   Windows  %ProgramData%\Virtual Serial Studio\licences
///   Linux    /etc/virtual-serial-studio/licences
///   macOS    /Library/Application Support/Virtual Serial Studio/licences
///
/// An empty path when the system gives no answer.
std::filesystem::path machine_licence_directory();

/// The licence folder of the user who runs the program:
///
///   Windows  %LOCALAPPDATA%\Virtual Serial Studio\licences
///   Linux    $XDG_DATA_HOME/virtual-serial-studio/licences, or
///            ~/.local/share/virtual-serial-studio/licences when
///            XDG_DATA_HOME is unset, empty or not an absolute path
///   macOS    ~/Library/Application Support/Virtual Serial Studio/licences
///
/// An empty path when the system gives no answer. The folder is not made.
std::filesystem::path user_licence_directory();

/// The name of this computer as a licence's `machines` is compared with it:
/// the host name. Empty when the system gives none.
std::string computer_name();

/// The folders and names the real program uses, with the keys built into this
/// build. The clock file is `licence-clock.json` beside the user's licence
/// folder. `build_time` is left for the caller to set.
store_options default_store_options();

} // namespace vss::license
