//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Linux: the package tools are asked which package owns the program file.
// `dpkg-query -S` names one for a copy from the deb, `rpm -qf` for a copy
// from the rpm, and a copy that neither knows was unpacked from the
// archive. Each tool runs as a process of its own with a time limit, so
// the caller never waits.

#include "updates/install_kind.hpp"

#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <utility>

namespace vss::gui::updates
{
namespace
{

/// A package tool that has not answered by now is taken to say no.
constexpr int kToolTimeoutMs = 3000;

class linux_probe final : public QObject, public install_kind_probe
{
public:
    explicit linux_probe(const QString& program_file)
        : program_file_(QFileInfo(program_file).canonicalFilePath())
    {
        if (program_file_.isEmpty())
            program_file_ = program_file;
    }

    void detect(std::function<void(vss::update::file_kind)> done) override
    {
        using vss::update::file_kind;
        ask(QStringLiteral("dpkg-query"), {QStringLiteral("-S"), program_file_},
            [this, done = std::move(done)](bool owned)
            {
                if (owned)
                {
                    done(file_kind::linux_deb);
                    return;
                }
                ask(QStringLiteral("rpm"),
                    {QStringLiteral("-qf"), program_file_},
                    [done](bool owned_by_rpm)
                    {
                        done(owned_by_rpm ? file_kind::linux_rpm
                                          : file_kind::linux_archive);
                    });
            });
    }

private:
    /// Runs `program` and calls `done` once: true when it ran and exited
    /// with 0, false when it said no, is not installed, failed or took too
    /// long.
    void ask(const QString& program, const QStringList& arguments,
             std::function<void(bool)> done)
    {
        auto* const process = new QProcess(this);
        process->setStandardOutputFile(QProcess::nullDevice());
        process->setStandardErrorFile(QProcess::nullDevice());
        const auto settled = std::make_shared<bool>(false);
        const auto settle =
            [process, settled, done = std::move(done)](bool owned)
        {
            if (*settled)
                return;
            *settled = true;
            process->deleteLater();
            done(owned);
        };
        connect(process, &QProcess::finished, this,
                [settle](int code, QProcess::ExitStatus status)
                { settle(status == QProcess::NormalExit && code == 0); });
        connect(process, &QProcess::errorOccurred, this,
                [settle](QProcess::ProcessError) { settle(false); });
        auto* const limit = new QTimer(process);
        limit->setSingleShot(true);
        connect(limit, &QTimer::timeout, this,
                [process, settle]
                {
                    settle(false);
                    process->kill();
                });
        limit->start(kToolTimeoutMs);
        process->start(program, arguments, QIODevice::ReadOnly);
    }

    QString program_file_;
};

} // namespace

std::unique_ptr<install_kind_probe>
make_install_kind_probe(const QString& program_file)
{
    return std::make_unique<linux_probe>(program_file);
}

vss::update::operating_system built_system()
{
    return vss::update::operating_system::linux;
}

} // namespace vss::gui::updates
