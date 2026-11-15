//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pieces that differ by system, run for real on the system the tests
// were built for: the kind of install, the checked file and the package
// command. What differs between systems is asked of `built_system()`, so
// this is one file for all of them.
//

#include "test_framework.hpp"

#include "updates/checked_file.hpp"
#include "updates/install_kind.hpp"
#include "updates/package_install.hpp"
#include "updates/plain_checked_file.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <functional>
#include <optional>

using namespace vss::gui::updates;
using vss::update::file_kind;
using vss::update::operating_system;

namespace
{

bool wait_until(const std::function<bool()>& done, int limit_ms = 15000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done())
    {
        if (clock.elapsed() > limit_ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QString write_file(const QTemporaryDir& folder, const QString& name,
                   const QByteArray& bytes)
{
    const QString path = QDir(folder.path()).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw vss_test::failure("could not write " + path.toStdString());
    return path;
}

QByteArray read_all(checked_file& file)
{
    QByteArray all;
    for (;;)
    {
        const std::optional<QByteArray> piece = file.read(1000);
        if (!piece.has_value())
            throw vss_test::failure("the checked file could not be read");
        if (piece->isEmpty())
            return all;
        all.append(*piece);
    }
}

} // namespace

TEST_CASE("update platform: a program that no installer or package put in "
          "place is an archive copy")
{
    // The test program runs from a build folder, so on every system it is
    // the copy that was unpacked, never the installed one.
    const auto probe =
        make_install_kind_probe(QCoreApplication::applicationFilePath());
    std::optional<file_kind> found;
    int calls = 0;
    probe->detect(
        [&](file_kind kind)
        {
            found = kind;
            ++calls;
        });
    REQUIRE(wait_until([&] { return found.has_value(); }));
    REQUIRE_EQ(calls, 1);
    if (!found.has_value())
        return;

    const operating_system system = built_system();
    if (system == operating_system::windows)
        REQUIRE(*found == file_kind::windows_zip);
    else if (system == operating_system::macos)
        REQUIRE(*found == file_kind::macos_archive);
    else
        REQUIRE(*found == file_kind::linux_archive);
}

TEST_CASE("update platform: a probe that is destroyed before it answers "
          "calls nothing")
{
    bool called = false;
    {
        const auto probe =
            make_install_kind_probe(QCoreApplication::applicationFilePath());
        probe->detect([&](file_kind) { called = true; });
        // Windows and macOS answer inside the call; Linux asks a process.
        called = false;
    }
    for (int turn = 0; turn < 20; ++turn)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    REQUIRE(!called);
}

TEST_CASE("update platform: the checked file gives the bytes of the file "
          "through the handle it holds")
{
    const QTemporaryDir folder;
    REQUIRE(folder.isValid());
    QByteArray bytes;
    for (int index = 0; index < 5000; ++index)
        bytes.append(static_cast<char>(index % 251));
    const QString path =
        write_file(folder, QStringLiteral("update.exe"), bytes);

    const auto file = open_checked_file(path);
    REQUIRE(file != nullptr);
    if (!file)
        return;
    REQUIRE(QDir::cleanPath(file->path()) == QDir::cleanPath(path));
    REQUIRE(read_all(*file) == bytes);
    // At the end there is nothing more, and that is not a failure.
    const std::optional<QByteArray> more = file->read(1000);
    REQUIRE(more.has_value());
    if (more.has_value())
        REQUIRE(more->isEmpty());
}

TEST_CASE("update platform: on Windows nothing can change, replace or "
          "delete a checked file while it is held")
{
    const QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString path = write_file(folder, QStringLiteral("update.exe"),
                                    QByteArrayLiteral("the checked bytes"));
    const QString other = write_file(folder, QStringLiteral("other.exe"),
                                     QByteArrayLiteral("something else"));
    const QString moved = QDir(folder.path()).filePath("moved.exe");

    auto file = open_checked_file(path);
    REQUIRE(file != nullptr);
    if (!file)
        return;

    if (built_system() == operating_system::windows)
    {
        QFile writer(path);
        REQUIRE(!writer.open(QIODevice::WriteOnly | QIODevice::Append));
        REQUIRE(!QFile::remove(path));
        REQUIRE(!QFile::rename(path, moved));
        // Reading is still allowed: that is all starting a program needs.
        QFile reader(path);
        REQUIRE(reader.open(QIODevice::ReadOnly));
        REQUIRE(reader.readAll() == QByteArrayLiteral("the checked bytes"));
        reader.close();
        REQUIRE(read_all(*file) == QByteArrayLiteral("the checked bytes"));
    }

    // Once it is let go of, the file is an ordinary file again.
    file.reset();
    REQUIRE(QFile::remove(path));
    REQUIRE(QFile::rename(other, path));
}

TEST_CASE("update platform: a folder, a missing file and a link are not "
          "opened as a checked file")
{
    const QTemporaryDir folder;
    REQUIRE(folder.isValid());
    REQUIRE(open_checked_file(folder.path()) == nullptr);
    REQUIRE(open_checked_file(QDir(folder.path()).filePath("absent.exe")) ==
            nullptr);
    REQUIRE(open_plain_checked_file(folder.path()) == nullptr);

    const QString target = write_file(folder, QStringLiteral("real.exe"),
                                      QByteArrayLiteral("real"));
    const QString link = QDir(folder.path()).filePath("link.exe");
    // Making a link needs a right that an ordinary Windows account may not
    // have. Where it cannot be made there is nothing to test.
    if (QFile::link(target, link) && QFileInfo(link).isSymLink())
    {
        REQUIRE(open_checked_file(link) == nullptr);
        REQUIRE(open_plain_checked_file(link) == nullptr);
    }
}

TEST_CASE("update platform: only an installer or a disk image is handed to "
          "the system")
{
    const QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString path = write_file(folder, QStringLiteral("studio.zip"),
                                    QByteArrayLiteral("an archive"));
    const auto file = open_checked_file(path);
    REQUIRE(file != nullptr);
    if (!file)
        return;
    // None of these starts anything on any system.
    for (const file_kind kind :
         {file_kind::windows_zip, file_kind::macos_archive,
          file_kind::linux_archive, file_kind::linux_deb, file_kind::linux_rpm})
        REQUIRE(start_install(kind, *file) == install_start::not_supported);
}

TEST_CASE("update platform: the command for a package is one a shell reads "
          "as written")
{
    REQUIRE(package_install_command(
                file_kind::linux_deb,
                QStringLiteral("/home/a/.cache/vss/updates/1f/studio.deb")) ==
            QStringLiteral(
                "sudo apt install '/home/a/.cache/vss/updates/1f/studio.deb'"));
    REQUIRE(package_install_command(
                file_kind::linux_rpm,
                QStringLiteral("/home/a b/it's here/studio.rpm")) ==
            QStringLiteral(
                "sudo dnf install '/home/a b/it'\\''s here/studio.rpm'"));
    for (const file_kind kind :
         {file_kind::windows_installer, file_kind::windows_zip,
          file_kind::macos_dmg, file_kind::macos_archive,
          file_kind::linux_archive})
        REQUIRE(
            package_install_command(kind, QStringLiteral("/a/b")).isEmpty());
}

TEST_CASE("update platform: a package is installed only where the system "
          "has the tools, and never for another kind of file")
{
    for (const file_kind kind :
         {file_kind::windows_installer, file_kind::windows_zip,
          file_kind::macos_dmg, file_kind::macos_archive,
          file_kind::linux_archive})
    {
        REQUIRE(!package_install_available(kind));
        bool done = false;
        package_install_result result = package_install_result::done;
        QObject context;
        install_package(kind, QStringLiteral("/nowhere/file"), {}, &context,
                        [&](package_install_result answer)
                        {
                            result = answer;
                            done = true;
                        });
        // Never from inside the call.
        REQUIRE(!done);
        REQUIRE(wait_until([&] { return done; }));
        REQUIRE(result == package_install_result::not_available);
    }
    if (built_system() != operating_system::linux)
    {
        REQUIRE(!package_install_available(file_kind::linux_deb));
        REQUIRE(!package_install_available(file_kind::linux_rpm));
    }
}
