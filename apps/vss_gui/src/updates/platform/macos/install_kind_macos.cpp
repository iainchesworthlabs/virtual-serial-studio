//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// macOS: a program inside an application bundle came from the disk image.
// Anything else was unpacked from the archive.

#include "updates/install_kind.hpp"

#include <QFileInfo>

#include <utility>

namespace vss::gui::updates
{
namespace
{

class macos_probe final : public install_kind_probe
{
public:
    explicit macos_probe(QString program_file)
        : program_file_(std::move(program_file))
    {
    }

    void detect(std::function<void(vss::update::file_kind)> done) override
    {
        const QString path = QFileInfo(program_file_).canonicalFilePath();
        const bool in_bundle =
            path.contains(QLatin1String(".app/Contents/MacOS/"));
        done(in_bundle ? vss::update::file_kind::macos_dmg
                       : vss::update::file_kind::macos_archive);
    }

private:
    QString program_file_;
};

} // namespace

std::unique_ptr<install_kind_probe>
make_install_kind_probe(const QString& program_file)
{
    return std::make_unique<macos_probe>(program_file);
}

vss::update::operating_system built_system()
{
    return vss::update::operating_system::macos;
}

} // namespace vss::gui::updates
