//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The licence files on this computer, for the screens. The store itself, with
// the folders, the clock and the judging, is vss::license::store in the
// licence library; this is the part that turns its answers into the Qt values
// the backend hands to a screen.
//

#pragma once

#include "backend/licence_view.hpp"

#include <vss/license/store.hpp>

#include <QString>

#include <filesystem>

namespace vss::gui
{

/// What the library's store found, as the screens are told it.
licence_view to_licence_view(const vss::license::store_view& view);

/// What the library's store did with an imported file.
licence_import_result
to_licence_import_result(const vss::license::import_result& result);

/// A path from the dialog's text.
std::filesystem::path to_path(const QString& text);

class licence_store
{
public:
    explicit licence_store(vss::license::store_options options);

    /// Reads the folders again and notes the time. Returns true when the
    /// view changed.
    bool refresh();

    /// What `refresh` last found.
    const licence_view& view() const noexcept { return view_; }

    /// Checks the file at `path` and, when its signature verifies and
    /// nothing refuses it, copies it to the user's folder. The original is
    /// left where it was. An invalid file never replaces anything.
    licence_import_result import_file(const QString& path);

    /// Removes the file `file_name` (a `licence_file_view::file_name`) from
    /// the user's folder. A file in the per-machine folder is never touched.
    /// Returns true when a file was removed.
    bool remove(const QString& file_name);

private:
    vss::license::store store_;
    licence_view view_;
};

} // namespace vss::gui
