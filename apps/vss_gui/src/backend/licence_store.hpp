//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence files on this computer: where they are, which one Studio uses,
// importing and removing. It reads files and judges them with vss_license;
// it has no network code. Everything it depends on is passed in, so the
// tests give it folders, keys and a clock of their own.
//

#pragma once

#include "backend/licence_view.hpp"

#include <vss/license/license.hpp>
#include <vss/license/signed_file.hpp>

#include <QString>

#include <functional>
#include <vector>

namespace vss::gui
{

struct licence_store_options
{
    /// Where Import copies a licence. Made when the first file is imported.
    QString user_directory;
    /// Where an administrator puts a site licence. Only read. May be empty.
    QString machine_directory;
    /// The file that keeps the latest time seen and the clock resets used.
    /// Empty keeps them in memory only.
    QString clock_file;
    /// The keys that may sign a licence or a revocation list.
    std::vector<vss::license::signed_file::trusted_key> keys;
    /// When this program was built: a time that has certainly passed.
    vss::license::time_point build_time = vss::license::time_point::min();
    QString computer_name;
    /// The time now. Empty uses the system clock.
    std::function<vss::license::time_point()> clock;
};

/// The folders and names the real program uses, with the keys built into
/// this build.
licence_store_options default_licence_store_options();

/// The per-machine licence folder of this system.
QString machine_licence_directory();

class licence_store
{
public:
    /// The extension of a licence file and of a revocation list.
    static constexpr const char* kLicenceSuffix = "vsslic";
    static constexpr const char* kRevocationsSuffix = "vssrev";
    /// No more files than this are read from a folder.
    static constexpr int kMaxFiles = 64;

    explicit licence_store(licence_store_options options);

    /// Reads the folders again and notes the time. Returns true when the
    /// view changed.
    bool refresh();

    /// What `refresh` last found.
    const licence_view& view() const noexcept { return view_; }

    /// Checks the file at `path` and, when its signature verifies and
    /// nothing refuses it, copies it to the user's folder. The original is
    /// left where it was. An invalid file never replaces anything. Calls
    /// `refresh` when something was copied.
    licence_import_result import_file(const QString& path);

    /// Removes the file `file_name` (a `licence_file_view::file_name`) from
    /// the user's folder. A file in the per-machine folder is never touched.
    /// Returns true when a file was removed, and then has called `refresh`.
    bool remove(const QString& file_name);

private:
    vss::license::context context(vss::license::time_point now) const;
    void load_clock();
    void save_clock() const;
    void load_revocations();

    licence_store_options options_;
    vss::license::clock_state clock_;
    vss::license::revocation_list revocations_;
    bool have_revocations_ = false;
    licence_view view_;
};

} // namespace vss::gui
