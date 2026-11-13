//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Measures what drawing the canvas costs, in the renderer you ask for, on the
// platform you run it on (so a real window and the real GPU, unlike the
// tests, which are offscreen):
//
//   vss_gui_render_bench --render=software|hardware|auto [--ports=N] [--hold=S] [--grab=FILE]
//   [--size=WxH] [--profile]
//
// It opens the Studio window with N linked loopback pairs (default 4), then
// reports how long a repaint of the canvas takes, how long a resize takes, and
// how long a pan takes, as the mean and the slowest over a few hundred steps.
// A step is the time until the event loop has handled the change. With the
// GPU, the frame itself is submitted to the graphics driver and finishes
// later, so the figure is the CPU's cost of the step, which is what stalls the
// window; the GPU's own time is in the Tracy trace (`gui.gpu.frame`).
//

#include "backend/engine_studio_backend.hpp"
#include "rendering/render_state.hpp"
#include "shell/shell_window.hpp"
#include "theme/theme.hpp"
#include "topology/topology_view.hpp"

#include <vss/core/profile.hpp>
#include <vss/core/profiling.hpp>
#include <vss/studio/studio.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QSettings>
#include <QStringList>
#include <QString>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{

using vss::gui::EngineStudioBackend;
using vss::gui::ShellWindow;
using vss::gui::topology::TopologyView;

struct figures
{
    double mean_ms = 0.0;
    double slowest_ms = 0.0;
    /// Mean CPU time spent drawing the canvas in the step, and the GPU's.
    double paint_ms = 0.0;
    double gpu_ms = 0.0;
    int steps = 0;
};

void spin(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    do
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    } while (timer.elapsed() < milliseconds);
}

template <typename Step>
figures measure(TopologyView& canvas, int steps, Step&& step)
{
    figures result;
    double total = 0.0;
    double paint = 0.0;
    double gpu = 0.0;
    for (int index = 0; index < steps; ++index)
    {
        QElapsedTimer timer;
        timer.start();
        step(index);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        const double took = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        total += took;
        paint += canvas.last_paint_ms();
        gpu += canvas.last_gpu_ms();
        result.slowest_ms = std::max(result.slowest_ms, took);
        ++result.steps;
    }
    result.mean_ms = total / std::max(result.steps, 1);
    result.paint_ms = paint / std::max(result.steps, 1);
    result.gpu_ms = gpu / std::max(result.steps, 1);
    return result;
}

void report(const char* name, const figures& value)
{
    std::printf("%-8s %4d steps  step mean %7.2f ms  slowest %7.2f ms  "
                "canvas drawing %6.2f ms  gpu %6.2f ms\n",
                name, value.steps, value.mean_ms, value.slowest_ms,
                value.paint_ms, value.gpu_ms);
}

} // namespace

int main(int argc, char** argv)
{
    QString render;
    int pairs = 4;
    int hold = 0;
    int window_width = 1280;
    int window_height = 800;
    QString grab_path;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strncmp(argv[index], "--render=", 9) == 0)
            render = QString::fromLocal8Bit(argv[index] + 9);
        else if (std::strncmp(argv[index], "--ports=", 8) == 0)
            pairs = std::max(1, std::atoi(argv[index] + 8));
        else if (std::strncmp(argv[index], "--size=", 7) == 0)
        {
            const QStringList parts =
                QString::fromLocal8Bit(argv[index] + 7).split(QLatin1Char('x'));
            if (parts.size() == 2)
            {
                window_width = std::max(640, parts.at(0).toInt());
                window_height = std::max(480, parts.at(1).toInt());
            }
        }
        else if (std::strncmp(argv[index], "--grab=", 7) == 0)
            grab_path = QString::fromLocal8Bit(argv[index] + 7);
        else if (std::strncmp(argv[index], "--hold=", 7) == 0)
            hold = std::atoi(argv[index] + 7);
    }

    // --profile, --profile=PORT and --profile-wait=S start Tracy as the
    // application does, so a trace of the run has the render zones.
    auto profile = vss::profiling::request_from_environment(false);
    for (int index = 1; index < argc; ++index)
        static_cast<void>(vss::profiling::take_argument(argv[index], profile));
    if (profile.enable)
        static_cast<void>(vss::profiling::start(profile));

    // Ask the graphics device for GPU timings (see rendering/gpu_canvas.hpp).
    qputenv("QSG_RHI_PROFILE", QByteArrayLiteral("1"));
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VSS Render Bench"));
    QCoreApplication::setApplicationName(QStringLiteral("VSS Render Bench"));
    QSettings().clear();
    QSettings().setValue(QStringLiteral("ui/advanced"), true);
    vss::gui::Theme::instance().initialise();
    vss::gui::rendering::initialise(vss::gui::rendering::render_mode::automatic,
                                    {}, render);

    vss::studio studio;
    EngineStudioBackend backend(studio);
    ShellWindow window(backend);
    window.resize(window_width, window_height);
    window.show();
    spin(300);

    for (int pair = 0; pair < pairs; ++pair)
    {
        const auto first = backend.create_virtual_port(
            QStringLiteral("loop-%1").arg(pair),
            vss::virtual_port_kind::loopback, true);
        const auto second = backend.create_mock_port(
            QStringLiteral("fixture-%1").arg(pair), vss::serial_settings{},
            QStringLiteral("scripted"));
        if (!first || !second ||
            !backend.connect_ports(*second, *first, vss::link_options{}))
        {
            std::fputs("could not make the ports\n", stderr);
            return 2;
        }
    }
    spin(1000);

    auto* canvas = window.findChild<TopologyView*>();
    if (canvas == nullptr)
    {
        std::fputs("no canvas\n", stderr);
        return 2;
    }
    std::printf("renderer: %s\n",
                vss::gui::rendering::describe().toLocal8Bit().constData());
    std::printf("canvas:   %d x %d, %d ports\n", canvas->viewport()->width(),
                canvas->viewport()->height(), pairs * 2);

    // --grab=FILE.png saves the window as the application itself draws it.
    if (!grab_path.isEmpty())
    {
        spin(500);
        window.grab().save(grab_path);
    }

    // --hold=N keeps the window up for N seconds, to look at it.
    if (hold > 0)
        spin(hold * 1000);

    // Repaint: ask for a whole new frame and let the loop deliver it.
    report("repaint", measure(*canvas, 300,
                              [canvas](int)
                              {
                                  canvas->viewport()->update();
                              }));

    // Pan: scroll the canvas a few pixels each step.
    canvas->setSceneRect(-4000, -4000, 8000, 8000);
    report("pan", measure(*canvas, 300,
                          [canvas](int index)
                          {
                              auto* bar = canvas->horizontalScrollBar();
                              bar->setValue(bar->value() +
                                            (index % 2 == 0 ? 7 : -7));
                          }));

    // Resize: a drag of the corner out and back, as the responsiveness test
    // does.
    // The sweep runs from 70 percent of the window to the full size.
    report("resize", measure(*canvas, 240,
                             [&window, window_width, window_height](int index)
                             {
                                 const int along =
                                     (index / 40) % 2 == 0 ? index % 40
                                                           : 40 - index % 40;
                                 window.resize(
                                     window_width * 7 / 10 +
                                         window_width * 3 / 10 * along / 40,
                                     window_height * 7 / 10 +
                                         window_height * 3 / 10 * along / 40);
                             }));
    return 0;
}
