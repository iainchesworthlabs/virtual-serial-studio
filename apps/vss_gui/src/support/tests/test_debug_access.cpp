//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Debug access in the window: the Settings switches, the approval dialog, the
// chip text, what a program that was let in can read of the window, and that
// turning it off removes the pipe.  The audit log and the instance file go to
// a folder of the test's own.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/debug_access.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "widgets/dialogs.hpp"

#include <vss/debug/client.hpp>
#include <vss/studio/studio.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace support = vss::gui::support;
namespace dbg = vss::debug;

namespace
{

/// A debug client in a thread of its own, since the window answers it only
/// while the test's event loop runs.
struct asking_client
{
    explicit asking_client(std::string instance, dbg::scope_set scopes)
        : worker(
              [this, instance = std::move(instance), scopes]
              {
                  dbg::client_options options;
                  options.instance = instance;
                  options.scopes = scopes;
                  options.timeout = std::chrono::seconds(30);
                  auto made = dbg::client::connect(options);
                  connected = made.has_value();
                  if (made)
                  {
                      const auto status = (*made)->call("debug.status");
                      status_ok = status && status->ok;
                      const auto state = (*made)->call("debug.gui.state");
                      state_ok = state && state->ok;
                      state_json = state ? state->json : std::string{};
                      const auto snapshot = (*made)->call("debug.snapshot");
                      snapshot_ok = snapshot && snapshot->ok;
                  }
                  finished = true;
              })
    {
    }
    ~asking_client()
    {
        if (worker.joinable())
            worker.join();
    }
    std::atomic<bool> connected{false};
    std::atomic<bool> status_ok{false};
    std::atomic<bool> state_ok{false};
    std::atomic<bool> snapshot_ok{false};
    std::atomic<bool> finished{false};
    std::string state_json;
    std::thread worker;
};

QPushButton* approval_button(int index)
{
    for (QWidget* top : QApplication::topLevelWidgets())
    {
        if (top->objectName() != QStringLiteral("debugApprovalDialog"))
            continue;
        if (auto* dialog = qobject_cast<vss::gui::ConfirmDialog*>(top))
            return dialog->choice_button(index);
    }
    return nullptr;
}

} // namespace

TEST_CASE("debug access: it is off, and the Settings switches show that")
{
    QSettings().clear();
    vss::gui::FakeStudioBackend backend;
    auto& access = support::DebugAccess::instance();
    REQUIRE(!access.enabled());
    REQUIRE(access.chip_text().isEmpty());

    support::SettingsDialog dialog(backend, support::load_ui_settings());
    auto* on =
        dialog.findChild<QCheckBox*>(QStringLiteral("debugAccessSwitch"));
    auto* traffic =
        dialog.findChild<QCheckBox*>(QStringLiteral("debugTrafficSwitch"));
    auto* status =
        dialog.findChild<QLabel*>(QStringLiteral("debugAccessStatus"));
    REQUIRE(on != nullptr);
    REQUIRE(traffic != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(!on->isChecked());
    REQUIRE(!traffic->isEnabled());
    // Without a studio there is nothing to ask, and the switch says so.
    if (!access.available())
        REQUIRE(!on->isEnabled());
    REQUIRE(status->text().contains(QStringLiteral("off")));
}

TEST_CASE(
    "debug access: a program is asked, let in, and read; off removes the pipe")
{
    QSettings().clear();
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const QString base = folder.path();
    const std::filesystem::path audit =
        std::filesystem::path(base.toStdString()) / "audit";
    const std::filesystem::path instances =
        std::filesystem::path(base.toStdString()) / "instances";

    vss::studio engine;
    auto& access = support::DebugAccess::instance();
    access.attach(engine, nullptr);
    access.use_directories(audit, instances);
    REQUIRE(access.available());
    REQUIRE(access.enable());
    REQUIRE(access.enabled());
    REQUIRE(access.chip_text().contains(QStringLiteral("Debug access on")));

    // The Settings switches follow it.
    vss::gui::FakeStudioBackend backend;
    support::SettingsDialog dialog(backend, support::load_ui_settings());
    auto* on =
        dialog.findChild<QCheckBox*>(QStringLiteral("debugAccessSwitch"));
    REQUIRE(on != nullptr);
    REQUIRE(on->isChecked());

    const auto found = dbg::find_instances(instances);
    REQUIRE_EQ(found.size(), std::size_t{1});

    asking_client client(found[0].instance, dbg::bit(dbg::scope::read) |
                                                dbg::bit(dbg::scope::gui));
    // The dialog appears, and the person says Allow once.
    REQUIRE(vss_test::wait_until([] { return approval_button(1) != nullptr; }));
    approval_button(1)->click();
    REQUIRE(vss_test::wait_until([&] { return client.finished.load(); }));
    REQUIRE(client.connected.load());
    REQUIRE(client.status_ok.load());
    REQUIRE(client.snapshot_ok.load());
    // The window answered for itself, on its own thread.
    REQUIRE(client.state_ok.load());
    REQUIRE(client.state_json.find("windows") != std::string::npos);

    access.disable();
    REQUIRE(!access.enabled());
    REQUIRE(access.chip_text().isEmpty());
    REQUIRE(dbg::find_instances(instances).empty());
}

TEST_CASE("debug access: Don't allow refuses the program")
{
    QSettings().clear();
    QTemporaryDir folder;
    REQUIRE(folder.isValid());
    const auto base = std::filesystem::path(folder.path().toStdString());

    vss::studio engine;
    auto& access = support::DebugAccess::instance();
    access.attach(engine, nullptr);
    access.use_directories(base / "audit", base / "instances");
    REQUIRE(access.enable());
    const auto found = dbg::find_instances(base / "instances");
    REQUIRE_EQ(found.size(), std::size_t{1});

    asking_client client(found[0].instance, dbg::bit(dbg::scope::read));
    REQUIRE(vss_test::wait_until([] { return approval_button(0) != nullptr; }));
    approval_button(0)->click();
    REQUIRE(vss_test::wait_until([&] { return client.finished.load(); }));
    REQUIRE(!client.connected.load());
    access.disable();
}
