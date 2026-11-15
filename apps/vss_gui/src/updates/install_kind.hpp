//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How this copy of Studio was installed, which decides the package an
// update offers (docs/updates.md, section 7.1). Each system's test is its
// own source file under `platform/`, chosen in CMake.
//
// A wrong answer installs nothing by itself: it only chooses which published
// file is offered, and that file's hash still has to match the feed.
//

#pragma once

#include <vss/update/feed.hpp>

#include <QString>

#include <functional>
#include <memory>

namespace vss::gui::updates
{

class install_kind_probe
{
public:
    install_kind_probe() = default;
    install_kind_probe(const install_kind_probe&) = delete;
    install_kind_probe& operator=(const install_kind_probe&) = delete;
    virtual ~install_kind_probe() = default;

    /// Works out the kind and calls `done` exactly once, on the calling
    /// thread. It may call it from inside this call (Windows and macOS
    /// know at once) or later (Linux asks the package tools in a process
    /// of their own, with a time limit, and never blocks the caller).
    virtual void detect(std::function<void(vss::update::file_kind)> done) = 0;
};

/// The probe for this system. `program_file` is the full path of the
/// running program (`QCoreApplication::applicationFilePath()`).
std::unique_ptr<install_kind_probe>
make_install_kind_probe(const QString& program_file);

/// The architecture this program was built for.
vss::update::architecture built_architecture();

/// The system this program was built for.
vss::update::operating_system built_system();

} // namespace vss::gui::updates
