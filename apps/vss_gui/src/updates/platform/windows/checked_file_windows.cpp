//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Windows: the file is opened with reading shared and nothing else. While
// the handle is open no other program can write to the file, delete it,
// rename it or put another in its place, so the bytes that were hashed are
// the bytes Windows runs. Starting the program needs only to read the
// file, which the handle allows.

#include "updates/checked_file.hpp"

#include <QDir>
#include <QFileInfo>

#include <QtCore/qt_windows.h>

#include <shellapi.h>

#include <string>
#include <utility>

namespace vss::gui::updates
{
namespace
{

std::wstring native(const QString& path)
{
    return QDir::toNativeSeparators(path).toStdWString();
}

class windows_checked_file final : public checked_file
{
public:
    windows_checked_file(HANDLE handle, QString path)
        : handle_(handle), path_(std::move(path))
    {
    }

    ~windows_checked_file() override { CloseHandle(handle_); }

    QString path() const override { return path_; }

    std::optional<QByteArray> read(qint64 max) override
    {
        QByteArray bytes(max, Qt::Uninitialized);
        DWORD got = 0;
        if (ReadFile(handle_, bytes.data(), static_cast<DWORD>(max), &got,
                     nullptr) == 0)
            return std::nullopt;
        bytes.truncate(static_cast<qsizetype>(got));
        return bytes;
    }

private:
    HANDLE handle_;
    QString path_;
};

} // namespace

std::unique_ptr<checked_file> open_checked_file(const QString& path)
{
    // FILE_FLAG_OPEN_REPARSE_POINT opens a link itself and not what it
    // points to, so the test below sees it.
    const HANDLE handle =
        CreateFileW(native(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                        FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return nullptr;
    BY_HANDLE_FILE_INFORMATION information{};
    const DWORD not_plain =
        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    if (GetFileInformationByHandle(handle, &information) == 0 ||
        (information.dwFileAttributes & not_plain) != 0)
    {
        CloseHandle(handle);
        return nullptr;
    }
    return std::make_unique<windows_checked_file>(handle, path);
}

install_start start_install(vss::update::file_kind kind,
                            const checked_file& file)
{
    if (kind != vss::update::file_kind::windows_installer)
        return install_start::not_supported;

    // The installer's own manifest asks for an administrator, so Windows
    // shows its prompt for exactly this file. `CreateProcess` cannot start
    // such a program; the shell can.
    const std::wstring program = native(file.path());
    const std::wstring folder = native(QFileInfo(file.path()).absolutePath());
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask =
        SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    request.lpVerb = L"open";
    request.lpFile = program.c_str();
    request.lpDirectory = folder.c_str();
    request.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&request) == 0)
        return GetLastError() == ERROR_CANCELLED ? install_start::declined
                                                 : install_start::failed;
    if (request.hProcess != nullptr)
        CloseHandle(request.hProcess);
    return install_start::started;
}

} // namespace vss::gui::updates
