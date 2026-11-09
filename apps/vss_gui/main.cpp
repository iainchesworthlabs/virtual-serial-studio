//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/engine_studio_backend.hpp"
#include "language/language_choice.hpp"
#include "profiling/profiled_application.hpp"
#include "profiling/profiling_controller.hpp"
#include "shell/shell_window.hpp"
#include "shell/single_instance.hpp"
#include "shell/studio_tray.hpp"
#include "support/console.hpp"
#include "support/running_setup.hpp"
#include "support/stall_probe.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/gallery.hpp"

#include <vss/core/profile.hpp>
#include <vss/core/profiling.hpp>
#include <vss/studio/studio.hpp>

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPixmap>
#include <QSettings>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>

namespace
{

/// Ends the Tracy session when the program is about to exit. It sits after the
/// application object and before everything that owns threads, so by the time
/// it runs the studio and the window are gone.
struct ProfilingSession
{
    ~ProfilingSession() { vss::profiling::shutdown(); }
};

} // namespace

int main(int argc, char* argv[])
try
{
    // First of all, so that anything printed from here on reaches the
    // terminal that ran us (Windows builds the program without a console).
    vss::gui::support::attach_parent_console();

    bool selftest = false;
    bool screenshot = false;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--selftest") == 0)
            selftest = true;
        if (std::strncmp(argv[index], "--screenshot", 12) == 0)
            screenshot = true;
    }

    // Profiling is off unless asked for: --profile, --profile=PORT,
    // --profile-remote, VSS_PROFILE=1. Debug builds ask for it themselves,
    // except for the unattended self-test and screenshot runs, which start it
    // only when it was asked for in so many words.
    auto profile = vss::profiling::request_from_environment(
        vss::profiling::default_on() && !selftest && !screenshot);
    for (int index = 1; index < argc; ++index)
        static_cast<void>(vss::profiling::take_argument(argv[index], profile));

    // The self-test and the documentation screenshot are headless by
    // contract: set the platform plugin before QApplication reads it.  The
    // accessibility pass also runs at 200 percent scale through a separate
    // CTest entry, which sets QT_SCALE_FACTOR before this process starts.
    if (selftest || screenshot)
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    // Fractional display scales (125, 150 percent) must reach the renderer
    // unrounded.  Rounding 150 percent up to 200 or down to 100 resamples
    // every glyph and icon, which is the main cause of soft text.  Qt 6
    // defaults to this; say so explicitly so a Qt default change or an
    // environment override cannot bring the rounding back.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    vss::gui::ProfiledApplication application(argc, argv);
    const ProfilingSession profiling_session;
    vss::gui::ProfiledApplication::install_loop_marks();
    VSS_PROFILE_THREAD("gui.main");
    if (profile.enable)
    {
        const auto started = vss::profiling::start(profile);
        if (started)
        {
            std::fprintf(stderr, "%s\n",
                         vss::profiling::describe(*started).c_str());
            VSS_PROFILE_MESSAGE("gui.startup");
        }
        else
        {
            std::fprintf(stderr, "vss_gui: cannot start profiling: %s\n",
                         started.error().message().c_str());
        }
    }
    QApplication::setApplicationName(QStringLiteral("Virtual Serial Studio"));
    QApplication::setApplicationDisplayName(
        QStringLiteral("Virtual Serial Studio"));
    QApplication::setApplicationVersion(QStringLiteral(VSS_GUI_VERSION));
    QApplication::setOrganizationName(QStringLiteral("Virtual Serial Studio"));
    QApplication::setOrganizationDomain(
        QStringLiteral("virtualserial.example"));
    // The icon of every window and of the taskbar or dock entry, from the Qt
    // resource. The desktop file name lets a Linux desktop pair the window with
    // the icon in the hicolor theme that vss_gui.desktop names.
    QGuiApplication::setDesktopFileName(QStringLiteral("vss_gui"));
    QGuiApplication::setWindowIcon(
        vss::gui::IconProvider::instance().app_icon());
    vss::gui::apply_language();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Virtual Serial Studio - a playground for real, virtual "
                       "and networked serial ports."));
    const QCommandLineOption help_option = parser.addHelpOption();
    const QCommandLineOption version_option = parser.addVersionOption();
    const QCommandLineOption selftest_option(
        QStringLiteral("selftest"),
        QStringLiteral("Run the headless end-to-end check (offscreen "
                       "platform), print the result and exit."));
    parser.addOption(selftest_option);
    const QCommandLineOption screenshot_option(
        QStringLiteral("screenshot"),
        QStringLiteral("Render the main window to <file> and exit (headless)."),
        QStringLiteral("file"));
    parser.addOption(screenshot_option);
    const QCommandLineOption background_option(
        QStringLiteral("background"),
        QStringLiteral("Start in the notification area and start the ports "
                       "that were running before. Quits at once when there "
                       "are none. This is what start at sign-in runs."));
    parser.addOption(background_option);
    // A restart as administrator starts the new Studio before the old one has
    // quit; this waits for the old one instead of handing it the window.
    QCommandLineOption replace_option(QStringLiteral("replace"),
                                      QStringLiteral("Wait for a running "
                                                     "Studio to quit, then "
                                                     "take its place."));
    replace_option.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(replace_option);
    // The profiling options are read before the parser runs; they are declared
    // here so --help lists them, and filtered out of what the parser sees.
    const QCommandLineOption profile_option(
        QStringLiteral("profile"),
        QStringLiteral("Start the Tracy profiler listener on this computer "
                       "only (--profile=<port> picks the port; VSS_PROFILE=1 "
                       "does the same)."));
    parser.addOption(profile_option);
    const QCommandLineOption profile_remote_option(
        QStringLiteral("profile-remote"),
        QStringLiteral("Start the profiler and let other computers connect."));
    parser.addOption(profile_remote_option);
    const QCommandLineOption no_profile_option(
        QStringLiteral("no-profile"),
        QStringLiteral("Do not start the profiler, even where it is on by "
                       "default."));
    parser.addOption(no_profile_option);
    {
        QStringList arguments = application.arguments();
        arguments.erase(std::remove_if(arguments.begin() + 1, arguments.end(),
                                       [](const QString& argument)
                                       {
                                           vss::profiling::request ignored;
                                           return vss::profiling::take_argument(
                                               argument.toStdString(), ignored);
                                       }),
                        arguments.end());
        // process() would show --help, --version and a bad argument in a
        // message box on Windows when no console is attached (a pipe in CI, a
        // double click), and a message box has nobody to click it in a script.
        // They are printed to the standard streams instead, whatever the
        // platform, with the exit codes process() gives them.
        if (!parser.parse(arguments))
        {
            std::fprintf(stderr, "%s\n",
                         parser.errorText().toLocal8Bit().constData());
            return 1;
        }
        if (parser.isSet(version_option))
        {
            std::fprintf(stdout, "%s %s\n",
                         QCoreApplication::applicationName()
                             .toLocal8Bit()
                             .constData(),
                         QCoreApplication::applicationVersion()
                             .toLocal8Bit()
                             .constData());
            std::fflush(stdout);
            return 0;
        }
        if (parser.isSet(help_option))
        {
            std::fprintf(stdout, "%s",
                         parser.helpText().toLocal8Bit().constData());
            std::fflush(stdout);
            return 0;
        }
    }

    const bool headless = parser.isSet(selftest_option) ||
                          parser.isSet(screenshot_option) ||
                          qEnvironmentVariableIsSet("VSS_WIDGET_GALLERY");
    const bool hidden_start = parser.isSet(background_option);

    // One Studio per user.  A second one shows the first and leaves, so the
    // Start menu brings back a Studio that runs in the notification area.
    vss::gui::SingleInstance instance(vss::gui::SingleInstance::default_name());
    if (!headless)
    {
        const bool replace = parser.isSet(replace_option);
        if (!replace && instance.activate_running())
            return 0;
        static_cast<void>(instance.listen(replace ? 15000 : 0));
    }

    if (qEnvironmentVariableIsSet("VSS_WIDGET_GALLERY"))
    {
        vss::gui::Theme::instance().initialise();
        vss::gui::WidgetGallery gallery;
        gallery.resize(1240, 900);
        gallery.show();
        return application.exec();
    }

    try
    {
        vss::studio_options options;
        auto studio = std::make_unique<vss::studio>(options);
        vss::gui::EngineStudioBackend backend(*studio);
        if (parser.isSet(screenshot_option))
        {
            // Review images must be repeatable regardless of the user's saved
            // mode; this path never runs on normal startup.
            QSettings().setValue(
                QStringLiteral("ui/advanced"),
                qEnvironmentVariableIsSet("VSS_SCREENSHOT_ADVANCED"));
        }
        vss::gui::ShellWindow window(backend);

        // The probe plots every stretch in which the GUI thread did not return
        // to its event loop (see docs/profiling.md). It follows the profiler:
        // it runs while Tracy is recording and stops when it is paused, so a
        // dormant program carries no extra timer.
        vss::gui::support::StallProbe stall_probe;
        bool stall_probe_running = false;
        const auto follow_profiler = [&stall_probe, &stall_probe_running]
        {
            const bool recording =
                vss::gui::ProfilingController::instance().status().current ==
                vss::profiling::state::recording;
            if (recording == stall_probe_running)
                return;
            stall_probe_running = recording;
            if (recording)
                stall_probe.start();
            else
                stall_probe.stop();
        };
        QObject::connect(&vss::gui::ProfilingController::instance(),
                         &vss::gui::ProfilingController::changed, &stall_probe,
                         follow_profiler);
        follow_profiler();

        if (parser.isSet(selftest_option))
        {
            const bool passed = window.run_selftest();
            if (passed)
            {
                std::fputs("selftest ok\n", stdout);
                std::fflush(stdout);
                return 0;
            }
            std::fflush(stderr);
            return 1;
        }

        if (parser.isSet(screenshot_option))
        {
            if (qEnvironmentVariable("VSS_SCREENSHOT_THEME") ==
                QLatin1String("dark"))
                vss::gui::Theme::instance().set_mode(
                    vss::gui::theme_mode::dark);
            else if (qEnvironmentVariable("VSS_SCREENSHOT_THEME") ==
                     QLatin1String("light"))
                vss::gui::Theme::instance().set_mode(
                    vss::gui::theme_mode::light);

            // A representative topology so the documentation image shows the
            // real application rather than an empty window.
            if (!qEnvironmentVariableIsSet("VSS_SCREENSHOT_EMPTY"))
            {
                const auto console = backend.create_virtual_port(
                    QStringLiteral("console"), vss::virtual_port_kind::loopback,
                    true);
                if (!qEnvironmentVariableIsSet("VSS_SCREENSHOT_ONE"))
                {
                    const auto logger = backend.create_virtual_port(
                        QStringLiteral("logger"),
                        vss::virtual_port_kind::loopback, true);
                    if (console && logger)
                    {
                        static_cast<void>(
                            backend.connect_ports(*console, *logger, {}));
                        static_cast<void>(backend.send(
                            *console, QByteArrayLiteral("AT+VERSION\r\n")));
                    }
                    static_cast<void>(
                        backend.create_mock_port(QStringLiteral("modem"), {},
                                                 QStringLiteral("scripted")));
                }
            }

            if (qEnvironmentVariableIsSet("VSS_SCREENSHOT_COMPACT"))
                window.resize(960, 640);
            else
                window.resize(1280, 800);
            window.show();
            for (int frame = 0; frame < 8; ++frame)
            {
                application.processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            application.processEvents();

            const QPixmap image = window.grab();
            const QString path = parser.value(screenshot_option);
            if (!image.save(path))
            {
                std::fprintf(stderr, "cannot write %s\n",
                             path.toUtf8().constData());
                return 1;
            }
            std::printf("wrote %s (%dx%d)\n", path.toUtf8().constData(),
                        image.width(), image.height());
            return 0;
        }

        const bool use_tray = vss::gui::StudioTray::available();
        window.enable_background(vss::gui::support::running_setup_path(),
                                 use_tray);
        QObject::connect(&instance,
                         &vss::gui::SingleInstance::activation_requested,
                         &window, &vss::gui::ShellWindow::bring_to_front);
        if (hidden_start)
        {
            // Start at sign-in with nothing to run has nothing to do.
            if (backend.ports().empty())
                return 0;
            if (!use_tray)
                window.showMinimized();
        }
        else
        {
            window.show();
            VSS_PROFILE_MESSAGE("gui.window_shown");
        }
        const int exit_code = application.exec();
        VSS_PROFILE_MESSAGE("gui.shutdown");
        return exit_code;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "vss_gui: fatal: %s\n", error.what());
        if (!selftest)
        {
            QMessageBox::critical(nullptr,
                                  QStringLiteral("Virtual Serial Studio"),
                                  QString::fromUtf8(error.what()));
        }
        return 1;
    }
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
