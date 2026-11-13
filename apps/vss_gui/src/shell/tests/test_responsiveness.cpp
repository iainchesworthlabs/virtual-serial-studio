//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window must stay responsive while ports are busy. This test builds the
// setup of a bug report (a loopback linked to a fixture that generates bytes
// and echoes them, in Advanced mode, with the canvas showing it), keeps it
// busy, opens the context menus of both ports again and again, and measures
// the longest stretch in which the GUI thread did not return to its event
// loop. It uses the real engine, because the freeze was in how the window
// consumed the engine's events: every batch of traffic made the window ask
// the operating system about the driver and the firewall, rebuild its panes
// and reset the monitor.
//
// The budget is on the CPU time the GUI thread used, not on the wall clock: a
// build machine that is busy with other jobs delays the thread without the
// window doing any more work, and the test must give the same answer there.
// The wall-clock stall is printed beside it for a person reading the log.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include "backend/engine_studio_backend.hpp"
#include "panes/fixture_behaviour.hpp"
#include "shell/shell_window.hpp"
#include "support/stall_probe.hpp"
#include "topology/topology_view.hpp"

#include <vss/studio/studio.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QMenu>
#include <QSettings>
#include <QString>
#include <QWidget>

#include <algorithm>
#include <cstdio>

namespace
{

using vss::gui::EngineStudioBackend;
using vss::gui::ShellWindow;
using vss::gui::support::StallProbe;
using vss::gui::topology::TopologyView;

/// The most CPU time the GUI thread may use without going back to its event
/// loop. A person notices a pause from about 100 ms, and that is the goal on
/// a quiet computer: set `VSS_GUI_STALL_BUDGET_MS=100` to hold the window to
/// it. The default is looser because a build machine running other jobs makes
/// the same work cost two to three times the CPU time (shared cores and
/// caches). The failure this test was written for held the thread for over a
/// second, which no machine's load explains.
int stall_budget_ms()
{
    bool ok = false;
    const int given =
        qEnvironmentVariableIntValue("VSS_GUI_STALL_BUDGET_MS", &ok);
    return ok && given > 0 ? given : 250;
}

/// While context menus are opened and closed Qt also builds, styles and paints
/// a popup, which is not ours to bound, so the budget is half as large again.
int menu_budget_ms()
{
    return stall_budget_ms() * 3 / 2;
}

/// Right-clicks the node of `id` on the canvas, as a person does, and closes
/// the menu it opened.
void right_click_node(TopologyView& view, vss::endpoint_id id)
{
    const QPoint local = view.mapFromScene(view.node_rect(id).center());
    QWidget* viewport = view.viewport();
    QContextMenuEvent event(QContextMenuEvent::Mouse, local,
                            viewport->mapToGlobal(local));
    QApplication::sendEvent(viewport, &event);
    vss_test::spin(1);
    if (QWidget* popup = QApplication::activePopupWidget())
        popup->close();
    vss_test::spin(1);
}

/// Counts the resize events a widget receives, so a test can tell a resize
/// that settles from one that sets off another.
class ResizeCounter : public QObject
{
public:
    explicit ResizeCounter(QObject* parent) : QObject(parent) {}
    int count() const noexcept { return count_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Resize)
            ++count_;
        return QObject::eventFilter(watched, event);
    }

private:
    int count_ = 0;
};

} // namespace

TEST_CASE("responsive: a busy loopback linked to a fixture does not freeze "
          "the window")
{
    QSettings().clear();
    QSettings().setValue(QStringLiteral("ui/advanced"), true);

    vss::studio studio;
    EngineStudioBackend backend(studio);
    ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    vss_test::spin(50);

    // What the report did: a loopback, a fixture, and a link between them.
    const auto loopback = backend.create_virtual_port(
        QStringLiteral("loop"), vss::virtual_port_kind::loopback, true);
    const auto fixture = backend.create_mock_port(QStringLiteral("fixture"),
                                                  vss::serial_settings{},
                                                  QStringLiteral("scripted"));
    REQUIRE(loopback.has_value());
    REQUIRE(fixture.has_value());
    REQUIRE(backend.connect_ports(*fixture, *loopback, vss::link_options{})
                .has_value());
    vss_test::spin(100);

    auto* behaviours = window.findChild<vss::gui::FixtureBehaviours*>();
    auto* canvas = window.findChild<TopologyView*>();
    REQUIRE(behaviours != nullptr);
    REQUIRE(canvas != nullptr);

    // The busy part: the fixture sends as fast as the generator allows and
    // echoes whatever the loopback sends back, so the pair keeps trading
    // bytes for as long as the test runs.
    vss::gui::generator_config config;
    config.kind = vss::gui::generator_kind::random_bytes;
    config.per_second = 50;
    config.chunk = 256;
    behaviours->set_config(*fixture, config);
    REQUIRE(behaviours->set_echo(*fixture, true));
    REQUIRE(behaviours->start_generator(*fixture));

    // Let the traffic build, then measure. First the traffic alone: nothing
    // the engine sends may keep the GUI thread from its event loop for more
    // than the budget.
    vss_test::spin(400);
    StallProbe probe;
    probe.start();
    vss_test::spin(2000);
    probe.stop();
    const qint64 traffic_busy = probe.longest_busy_ms();
    const qint64 traffic_wall = probe.longest_ms();
    const int traffic_over = probe.busy_over(stall_budget_ms());
    const int traffic_ticks = probe.ticks();

    // Then a person right-clicking the fixture and the loopback while the
    // traffic goes on. A menu has costs of its own (Qt builds and paints a
    // styled popup), so it is held to a looser budget; the freeze this guards
    // against was over a second.
    probe.reset();
    probe.start();
    QElapsedTimer run;
    run.start();
    int menus = 0;
    while (run.elapsed() < 3000)
    {
        right_click_node(*canvas, *fixture);
        right_click_node(*canvas, *loopback);
        menus += 2;
        vss_test::spin(50);
    }
    probe.stop();

    // Stop the traffic before anything is torn down.
    behaviours->stop_generator(*fixture);
    static_cast<void>(behaviours->set_echo(*fixture, false));

    std::printf("        traffic alone: most CPU between ticks %lld ms (budget "
                "%d), longest wall-clock stall %lld ms, %d ticks\n",
                static_cast<long long>(traffic_busy), stall_budget_ms(),
                static_cast<long long>(traffic_wall), traffic_ticks);
    std::printf(
        "        with %d menus: most CPU between ticks %lld ms "
        "(budget %d), longest wall-clock stall %lld ms, %d ticks; "
        "%llu bytes generated\n",
        menus, static_cast<long long>(probe.longest_busy_ms()),
        menu_budget_ms(), static_cast<long long>(probe.longest_ms()),
        probe.ticks(),
        static_cast<unsigned long long>(behaviours->generated_bytes(*fixture)));

    REQUIRE(menus > 0);
    REQUIRE(traffic_ticks > 0);
    REQUIRE(probe.ticks() > 0);
    REQUIRE_EQ(traffic_over, 0);
    REQUIRE_EQ(probe.busy_over(menu_budget_ms()), 0);
}

TEST_CASE("responsive: dragging the window corner again and again stays cheap")
{
    QSettings().clear();
    QSettings().setValue(QStringLiteral("ui/advanced"), true);

    vss::studio studio;
    EngineStudioBackend backend(studio);
    ShellWindow window(backend);
    window.resize(1280, 800);
    window.show();
    vss_test::spin(50);

    const auto first = backend.create_virtual_port(
        QStringLiteral("port-1"), vss::virtual_port_kind::loopback, true);
    const auto second = backend.create_mock_port(
        QStringLiteral("port-2"), vss::serial_settings{},
        QStringLiteral("scripted"));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(backend.connect_ports(*second, *first, vss::link_options{})
                .has_value());
    vss_test::spin(100);

    auto* canvas = window.findChild<TopologyView*>();
    REQUIRE(canvas != nullptr);
    auto* resizes = new ResizeCounter(canvas);
    canvas->installEventFilter(resizes);

    // A drag of the bottom-right corner sends a resize for every few pixels,
    // out to a large window and back below the point where the layout turns
    // compact. Every step is followed by the event loop, as it is for a person.
    qint64 slowest = 0;
    qint64 total = 0;
    int steps = 0;
    for (int sweep = 0; sweep < 6; ++sweep)
    {
        for (int i = 0; i <= 40; ++i)
        {
            const int along = sweep % 2 == 0 ? i : 40 - i;
            const int width = 900 + along * 20;
            const int height = 560 + along * 12;
            QElapsedTimer step;
            step.start();
            window.resize(width, height);
            vss_test::spin(1);
            const qint64 took = step.elapsed();
            slowest = std::max(slowest, took);
            total += took;
            ++steps;
        }
    }
    std::printf("        %d resizes: slowest %lld ms, mean %lld ms\n", steps,
                static_cast<long long>(slowest),
                static_cast<long long>(total / std::max(steps, 1)));
    std::printf("        the canvas received %d resize events for %d steps\n",
                resizes->count(), steps);
    REQUIRE(steps > 0);
    // A window resize reaches the canvas once. More than two per step means
    // a resize is setting off another (a layout feedback loop).
    REQUIRE(resizes->count() <= steps * 2);
    REQUIRE(slowest < 1000);
}
