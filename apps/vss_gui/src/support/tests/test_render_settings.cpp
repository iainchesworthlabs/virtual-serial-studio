//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Rendering preference in Settings, Performance, and the Renderer line in
// About and in the diagnostic bundle.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/about_dialog.hpp"
#include "rendering/render_state.hpp"
#include "support/build_info.hpp"
#include "support/bundle.hpp"
#include "support/event_log.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "widgets/controls.hpp"

#include <QLabel>
#include <QSettings>

namespace
{

namespace support = vss::gui::support;
namespace rendering = vss::gui::rendering;

// The process-wide state is whatever an earlier test left; start from a
// known one: automatic, from the setting, drawing in software.
void use_automatic_state()
{
    rendering::render_state state;
    state.requested = rendering::render_mode::automatic;
    state.source = rendering::request_source::setting;
    state.active = rendering::renderer::software;
    rendering::set_state(state);
}

int id_of(rendering::render_mode mode)
{
    return static_cast<int>(mode);
}

} // namespace

TEST_CASE("render settings: the control has a name, three items and the "
          "saved mode")
{
    QSettings().clear();
    vss::gui::FakeStudioBackend backend;
    support::ui_settings saved = support::load_ui_settings();
    saved.render_mode = rendering::render_mode::hardware;
    support::SettingsDialog dialog(backend, saved);

    auto* control =
        dialog.findChild<vss::gui::SegmentedControl*>(
            QStringLiteral("renderControl"));
    REQUIRE(control != nullptr);
    REQUIRE_EQ(control->accessibleName(), QStringLiteral("Rendering"));
    REQUIRE_EQ(control->count(), 3);
    REQUIRE_EQ(control->current(), id_of(rendering::render_mode::hardware));
}

TEST_CASE("render settings: choosing Software saves it and says to restart")
{
    QSettings().clear();
    use_automatic_state();
    vss::gui::FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings());
    auto* control =
        dialog.findChild<vss::gui::SegmentedControl*>(
            QStringLiteral("renderControl"));
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("renderStatus"));
    REQUIRE(control != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(status->text().contains(QStringLiteral("Drawing now:")));
    REQUIRE(!status->text().contains(QStringLiteral("Restart Studio")));

    int changes = 0;
    QObject::connect(&dialog, &support::SettingsDialog::settings_changed,
                     &dialog, [&changes] { ++changes; });
    control->set_current(id_of(rendering::render_mode::software));
    REQUIRE(changes >= 1);
    REQUIRE(dialog.settings().render_mode == rendering::render_mode::software);
    REQUIRE_EQ(QSettings().value(QStringLiteral("ui/renderMode")).toString(),
               QStringLiteral("software"));
    REQUIRE(status->text().contains(QStringLiteral("Drawing now:")));
    REQUIRE(status->text().contains(
        QStringLiteral("Restart Studio to use this choice.")));
}

TEST_CASE("render settings: the environment override is mentioned")
{
    QSettings().clear();
    rendering::render_state state;
    state.requested = rendering::render_mode::software;
    state.source = rendering::request_source::environment;
    rendering::set_state(state);
    vss::gui::FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings());
    auto* status = dialog.findChild<QLabel*>(QStringLiteral("renderStatus"));
    REQUIRE(status != nullptr);
    REQUIRE(status->text().contains(QStringLiteral(
        "VSS_RENDER or --render is set, so it overrides this choice.")));
    use_automatic_state();
}

TEST_CASE("render settings: reset to defaults returns to Automatic")
{
    QSettings().clear();
    vss::gui::FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings());
    auto* control =
        dialog.findChild<vss::gui::SegmentedControl*>(
            QStringLiteral("renderControl"));
    REQUIRE(control != nullptr);
    control->set_current(id_of(rendering::render_mode::hardware));
    REQUIRE(dialog.settings().render_mode == rendering::render_mode::hardware);

    dialog.reset_to_defaults();
    REQUIRE_EQ(control->current(), id_of(rendering::render_mode::automatic));
    REQUIRE(dialog.settings().render_mode ==
            rendering::render_mode::automatic);
    REQUIRE_EQ(QSettings().value(QStringLiteral("ui/renderMode")).toString(),
               QStringLiteral("auto"));
}

TEST_CASE("render settings: About and the bundle name the renderer")
{
    const support::build_info info = support::current_build_info();
    REQUIRE(!info.renderer.isEmpty());
    REQUIRE_EQ(info.renderer, rendering::describe());

    support::AboutDialog about;
    const QString environment = about.environment_text();
    REQUIRE(environment.contains(QStringLiteral("Renderer:")));
    REQUIRE(environment.contains(info.renderer));

    vss::gui::FakeStudioBackend backend;
    support::EventLog log;
    support::bundle_selection selection;
    const auto files = support::build_bundle_files(backend, log, selection);
    bool found = false;
    for (const auto& file : files)
    {
        if (file.name == QStringLiteral("environment.txt"))
        {
            found = true;
            REQUIRE(file.data.contains("Renderer:"));
        }
    }
    REQUIRE(found);
}
