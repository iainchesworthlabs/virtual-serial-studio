//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update service as the program runs it: the transport that makes real
// requests, the feed keys built into the program, the administrator's
// policy, the real clock and the person's folders (docs/updates.md). `main`
// makes one of these and gives its service to the window. Nothing here
// makes a request; the service does, when the person or its timer asks.
//

#pragma once

#include "updates/network_transport.hpp"
#include "updates/update_service.hpp"

#include <QObject>
#include <QString>

#include <filesystem>
#include <memory>

namespace vss::gui::updates
{

/// True when an administrator's policy file says `"update_check": false`
/// (docs/updates.md, section 6.3). `program_directory` is the folder that
/// holds the running program. A policy file that is missing, cannot be read
/// or cannot be trusted turns nothing off: the check then follows the
/// person's own setting, because the worst that forcing a check gains is one
/// request for a public file.
bool update_check_disabled_by_policy(
    const std::filesystem::path& program_directory);

class ProgramUpdates final : public QObject
{
    Q_OBJECT

public:
    /// `running_version` is the program's version as `build_info::version`
    /// gives it.
    explicit ProgramUpdates(const QString& running_version,
                            QObject* parent = nullptr);
    ~ProgramUpdates() override;

    UpdateService& service() noexcept { return *service_; }

private:
    // The transport outlives the service that uses it.
    NetworkTransport transport_;
    std::unique_ptr<UpdateService> service_;
};

} // namespace vss::gui::updates
