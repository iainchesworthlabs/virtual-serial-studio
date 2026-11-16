//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "updates/plain_checked_file.hpp"

#include <QFile>
#include <QFileInfo>

namespace vss::gui::updates
{
namespace
{

class plain_checked_file final : public checked_file
{
public:
    explicit plain_checked_file(const QString& path) : file_(path) {}

    bool open() { return file_.open(QIODevice::ReadOnly); }

    QString path() const override { return file_.fileName(); }

    std::optional<QByteArray> read(qint64 max) override
    {
        QByteArray bytes(max, Qt::Uninitialized);
        const qint64 got = file_.read(bytes.data(), max);
        if (got < 0)
            return std::nullopt;
        bytes.truncate(got);
        return bytes;
    }

private:
    QFile file_;
};

} // namespace

std::unique_ptr<checked_file> open_plain_checked_file(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile())
        return nullptr;
    auto file = std::make_unique<plain_checked_file>(path);
    if (!file->open())
        return nullptr;
    return file;
}

} // namespace vss::gui::updates
