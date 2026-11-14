//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The profiling switch in Settings, Performance: it starts dormant, turning it
// on opens the Tracy listener on this computer only and says so, turning it
// off pauses recording, and the state is what the status bar and About show.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "profiling/profiling_controller.hpp"
#include "profiling/stall_watchdog.hpp"
#include "support/build_info.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "widgets/controls.hpp"

#include <vss/core/profiling.hpp>

#include <QCheckBox>
#include <QEvent>
#include <QLabel>
#include <QSettings>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace support = vss::gui::support;
using vss::gui::ProfilingController;

} // namespace

TEST_CASE("profiling: the Settings switch starts it, pauses it and resumes it")
{
    QSettings().clear();
    vss::gui::FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings());

    auto* record =
        dialog.findChild<QCheckBox*>(QStringLiteral("profilingSwitch"));
    auto* remote =
        dialog.findChild<QCheckBox*>(QStringLiteral("profilingRemoteSwitch"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("profilingStatus"));
    REQUIRE(record != nullptr);
    REQUIRE(remote != nullptr);
    REQUIRE(status != nullptr);

    auto& controller = ProfilingController::instance();
    if (!controller.available())
    {
        // A build without the profiler says so and offers no switch.
        REQUIRE(!record->isEnabled());
        REQUIRE(status->text().contains(QStringLiteral("does not include")));
        return;
    }

    // Begin from whichever state this process is in.
    controller.disable();
    record->setChecked(false);

    record->setChecked(true);
    REQUIRE(vss::profiling::current().current ==
            vss::profiling::state::recording);
    REQUIRE(controller.status().port != 0);
    // Nothing allowed another computer, so the listener is local.
    REQUIRE(!controller.status().remote);
    REQUIRE(controller.address().startsWith(QStringLiteral("127.0.0.1:")));
    REQUIRE(status->text().contains(QStringLiteral("this computer")));
    REQUIRE(!controller.chip_text().isEmpty());
    // About and the support bundle report it.
    REQUIRE(support::current_build_info().profiling.contains(
        QStringLiteral("listening on 127.0.0.1")));

    // Off pauses recording; the port stays, and says so.
    record->setChecked(false);
    REQUIRE(vss::profiling::current().current == vss::profiling::state::paused);
    REQUIRE(status->text().contains(QStringLiteral("stays open")));
    REQUIRE(controller.chip_text().contains(QStringLiteral("paused")));

    // And it can be turned on again, on the same port.
    const auto port = controller.status().port;
    record->setChecked(true);
    REQUIRE(vss::profiling::current().current ==
            vss::profiling::state::recording);
    REQUIRE_EQ(controller.status().port, port);

    // Wanting other computers after a local start needs a restart.
    REQUIRE(!remote->isEnabled());
    record->setChecked(false);
}

TEST_CASE("profiling: the stall watchdog names the event a frozen GUI thread "
          "was in and says when it recovers")
{
    std::mutex mutex;
    std::vector<std::string> lines;
    vss::gui::StallWatchdog watchdog(
        100,
        [&](const std::string& line)
        {
            const std::lock_guard lock(mutex);
            lines.push_back(line);
        });
    vss_test::spin(250);
    {
        const std::lock_guard lock(mutex);
        REQUIRE(lines.empty());
    }
    REQUIRE_EQ(watchdog.reported(), 0);

    // Hold the GUI thread without running its event loop, as a slow resize
    // would, with an event published as the one being handled.
    vss::gui::publish_current_event("FrozenWidget", QEvent::Resize);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    vss::gui::publish_current_event(nullptr, -1);
    vss_test::spin(400);

    REQUIRE_EQ(watchdog.reported(), 1);
    const std::lock_guard lock(mutex);
    REQUIRE(lines.size() >= 2);
    REQUIRE(lines.front().find("FrozenWidget / Resize") != std::string::npos);
    REQUIRE(lines.back().find("running again") != std::string::npos);
}

TEST_CASE("profiling: the choice to record from the next start is remembered")
{
    QSettings().clear();
    REQUIRE(!support::load_ui_settings().profile_on_start);

    auto settings = support::load_ui_settings();
    settings.profile_on_start = true;
    support::save_ui_settings(settings);
    REQUIRE(support::load_ui_settings().profile_on_start);

    settings.profile_on_start = false;
    support::save_ui_settings(settings);
    REQUIRE(!support::load_ui_settings().profile_on_start);
    QSettings().clear();
}
