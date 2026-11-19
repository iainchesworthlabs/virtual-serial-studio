//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <filesystem>
#include <functional>
#include <memory>

namespace vss
{
class studio;
}

namespace vss::debug
{
class server;
}

namespace vss::gui
{

class PluginBackend;

namespace support
{

/// The window's handle on the debug API (docs/debug-api.md): the one place
/// that turns the debug server on and off, asks the person about each program
/// that wants in, and tells the Settings page and the status bar what is
/// going on.
///
/// Debug access is off every time Studio starts and is never remembered.
/// While it is on a pipe that only this user can open exists, and a program
/// that uses it must be allowed by the person in a dialog first.  Nothing
/// leaves this computer.
class DebugAccess final : public QObject
{
    Q_OBJECT

public:
    static DebugAccess& instance();

    /// The studio and, when there is one, the plugin host that the answers
    /// come from.  Called once by the program, before the window shows.  A
    /// studio that was not given means debug access is not available (the
    /// gallery and the tests that make no studio).
    void attach(vss::studio& engine, PluginBackend* plugins);
    bool available() const noexcept { return engine_ != nullptr; }
    /// Where the audit log and the instance file go, in place of the user's
    /// state folder.  For a test, which must not write there.
    void use_directories(std::filesystem::path audit,
                         std::filesystem::path instances);

    bool enabled() const noexcept { return server_ != nullptr; }
    /// Turns it on.  False, with `last_error()`, when the pipe cannot be made.
    bool enable();
    /// Turns it off: every program is disconnected and the pipe is removed.
    void disable();

    /// The second switch: the bytes of traffic.  Needs debug access on, and
    /// turns itself off after ten minutes.
    bool traffic_on() const;
    void set_traffic(bool on);

    QString last_error() const { return error_; }
    /// One sentence for the Settings page and the chip's name.
    QString summary() const;
    /// A short phrase for the status bar chip, empty while it is off.
    QString chip_text() const;

    struct client_row
    {
        QString name;
        QString program;
        quint64 process = 0;
        QString scopes;
    };
    QVector<client_row> clients() const;
    /// Disconnects every program and forgets which ones were allowed.
    void disconnect_all();

Q_SIGNALS:
    void changed();

private:
    DebugAccess();
    ~DebugAccess() override;
    void ask_person(const QString& name, const QString& program,
                    quint64 process, unsigned wanted,
                    std::function<void(unsigned, bool)> answer);

    vss::studio* engine_ = nullptr;
    PluginBackend* plugins_ = nullptr;
    std::unique_ptr<vss::debug::server> server_;
    QString error_;
    std::filesystem::path audit_directory_;
    std::filesystem::path instance_directory_;
};

} // namespace support
} // namespace vss::gui
