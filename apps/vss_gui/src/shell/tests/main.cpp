//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"
#include "qt_settings.hpp"

#include "theme/theme.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QSettings>
#include <QString>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace
{

bool matches(const std::string& name, const std::vector<std::string>& filters)
{
    if (filters.empty())
        return true;
    for (const auto& filter : filters)
    {
        if (name.find(filter) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
try
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#if defined(Q_OS_WIN)
    if (qgetenv("QT_QPA_FONTDIR").isEmpty())
        qputenv("QT_QPA_FONTDIR", QByteArrayLiteral("C:/Windows/Fonts"));
#endif
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VSS GUI Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("VSS GUI Tests"));
    // Settings in a directory of this process, so two GUI test programs
    // never share them (see tests/framework/qt_settings.hpp).
    vss_test::isolated_settings isolated;
    QSettings().clear();
#if defined(Q_OS_WIN)
    if (QFontDatabase::families().contains(QStringLiteral("Segoe UI")))
        QApplication::setFont(QFont(QStringLiteral("Segoe UI"), 9));
#endif
    vss::gui::Theme::instance().initialise();

    std::vector<std::string> filters;
    bool verbose = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--verbose" || argument == "-v")
            verbose = true;
        else if (argument.rfind("--filter=", 0) == 0)
            filters.push_back(argument.substr(std::strlen("--filter=")));
    }

    std::size_t selected = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;

    for (const auto& test : vss_test::registry())
    {
        if (!matches(test.name, filters))
            continue;
        ++selected;
        if (verbose)
            std::printf("[RUN ] %s\n", test.name);
        const auto case_started = std::chrono::steady_clock::now();
        try
        {
            test.function();
            ++passed;
            if (verbose)
            {
                // The time tells a slow case from a hung one.
                const auto milliseconds =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - case_started);
                std::printf("[PASS] %s (%lld ms)\n", test.name,
                            static_cast<long long>(milliseconds.count()));
            }
        }
        catch (const vss_test::skip_signal& skip)
        {
            ++skipped;
            std::printf("[SKIP] %s: %s\n", test.name, skip.what());
        }
        catch (const vss_test::failure& failure)
        {
            ++failed;
            std::printf("[FAIL] %s\n        %s\n", test.name, failure.what());
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::printf("[FAIL] %s\n        unexpected exception: %s\n",
                        test.name, error.what());
        }
        catch (...)
        {
            ++failed;
            std::printf("[FAIL] %s\n        unknown exception\n", test.name);
        }
    }

    std::printf("\n%zu tests, %zu passed, %zu failed, %zu skipped\n", selected,
                passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "error: %s\n", error.what());
    return 2;
}
catch (...)
{
    std::fputs("error: unknown exception\n", stderr);
    return 2;
}
