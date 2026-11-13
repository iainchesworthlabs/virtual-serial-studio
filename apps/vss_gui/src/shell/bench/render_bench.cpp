//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Measures what drawing the canvas costs, in the renderer you ask for, on the
// platform you run it on (so a real window and the real GPU, unlike the
// tests, which are offscreen):
//
//   vss_gui_render_bench --render=software|hardware|auto [--ports=N]
//   [--size=WxH] [--hold=S] [--grab=FILE] [--swap-interval=N] [--gpu-timing]
//   [--profile]
//
// It opens the Studio window with N linked loopback pairs (default 4), then
// reports how long a new frame of the canvas takes, how long a pan, a move of
// a node and a resize of the window take, as the mean, the median and the
// slowest over a few hundred steps. A step is the time until the event loop
// has handled the change, which includes presenting the window.
//
// --swap-interval=N sets the swap interval after Studio's own choice (0 with
// the GPU, so a present does not wait for the display; 1 waits). --gpu-timing
// asks the graphics device for the GPU's time of each frame, for the `gpu`
// column and the Tracy zone `gui.gpu.frame`; reading it makes every frame
// wait for the GPU, so the step times are only comparable without it.
//
// Then the window is left alone for two seconds (idle), and for four seconds
// every fixture sends random bytes into its loopback and echoes what comes
// back, so the Monitor fills (traffic). For those it reports how much of the
// time the GUI thread was working and the longest time it stayed away from
// its event loop.
//
// With --profile every event has a `gui.event` zone, as in Studio, and the
// phases are the zones `bench.repaint`, `bench.pan`, `bench.resize`,
// `bench.move`, `bench.idle` and `bench.traffic`, with one `bench.step` per
// step of the first four.
//

#include "backend/engine_studio_backend.hpp"
#include "panes/fixture_behaviour.hpp"
#include "profiling/profiled_application.hpp"
#include "rendering/render_state.hpp"
#include "shell/shell_window.hpp"
#include "support/stall_probe.hpp"
#include "theme/theme.hpp"
#include "topology/topology_view.hpp"

#include <vss/core/profile.hpp>
#include <vss/core/profiling.hpp>
#include <vss/studio/studio.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QSurfaceFormat>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace
{

using vss::gui::EngineStudioBackend;
using vss::gui::ShellWindow;
using vss::gui::support::StallProbe;
using vss::gui::topology::TopologyView;

struct figures
{
    double mean_ms = 0.0;
    double median_ms = 0.0;
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
    std::vector<double> each;
    each.reserve(static_cast<std::size_t>(std::max(steps, 0)));
    for (int index = 0; index < steps; ++index)
    {
        VSS_PROFILE_ZONE("bench.step");
        QElapsedTimer timer;
        timer.start();
        step(index);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        const double took = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        total += took;
        each.push_back(took);
        paint += canvas.last_paint_ms();
        gpu += canvas.last_gpu_ms();
        result.slowest_ms = std::max(result.slowest_ms, took);
        ++result.steps;
    }
    result.mean_ms = total / std::max(result.steps, 1);
    if (!each.empty())
    {
        const auto middle =
            each.begin() + static_cast<std::ptrdiff_t>(each.size() / 2);
        std::nth_element(each.begin(), middle, each.end());
        result.median_ms = *middle;
    }
    result.paint_ms = paint / std::max(result.steps, 1);
    result.gpu_ms = gpu / std::max(result.steps, 1);
    return result;
}

void report(const char* name, const figures& value)
{
    std::printf("%-8s %4d steps  step mean %7.2f ms  median %7.2f ms  "
                "slowest %7.2f ms  canvas drawing %6.2f ms  gpu %6.2f ms\n",
                name, value.steps, value.mean_ms, value.median_ms,
                value.slowest_ms, value.paint_ms, value.gpu_ms);
}

/// Lets the window run for a while and says how busy its thread was. This
/// one runs a real event loop, which sleeps when there is nothing to do;
/// `spin` polls and would count as work.
void watch(const char* name, int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    StallProbe probe;
    probe.start();
    const qint64 cpu_before = StallProbe::thread_cpu_ns();
    QElapsedTimer timer;
    timer.start();
    loop.exec();
    const double wall_ms = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    const double cpu_ms =
        static_cast<double>(StallProbe::thread_cpu_ns() - cpu_before) / 1e6;
    probe.stop();
    std::printf("%-8s %5.0f ms  GUI thread working %5.1f %%  longest away "
                "from the event loop %lld ms  most CPU between ticks %lld ms\n",
                name, wall_ms, wall_ms > 0.0 ? cpu_ms * 100.0 / wall_ms : 0.0,
                static_cast<long long>(probe.longest_ms()),
                static_cast<long long>(probe.longest_busy_ms()));
}

} // namespace

int main(int argc, char** argv)
try
{
    QString render;
    int pairs = 4;
    int hold = 0;
    int window_width = 1280;
    int window_height = 800;
    int swap_interval = -1;
    bool gpu_timing = false;
    QString grab_path;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strncmp(argv[index], "--render=", 9) == 0)
            render = QString::fromLocal8Bit(argv[index] + 9);
        else if (std::strncmp(argv[index], "--ports=", 8) == 0)
            pairs =
                std::max(1, QString::fromLocal8Bit(argv[index] + 8).toInt());
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
            hold = QString::fromLocal8Bit(argv[index] + 7).toInt();
        else if (std::strncmp(argv[index], "--swap-interval=", 16) == 0)
            swap_interval = QString::fromLocal8Bit(argv[index] + 16).toInt();
        else if (std::strcmp(argv[index], "--gpu-timing") == 0)
            gpu_timing = true;
    }

    // --profile, --profile=PORT and --profile-wait=S start Tracy as the
    // application does, so a trace of the run has the render zones.
    auto profile = vss::profiling::request_from_environment(false);
    for (int index = 1; index < argc; ++index)
        static_cast<void>(vss::profiling::take_argument(argv[index], profile));
    if (profile.enable)
        static_cast<void>(vss::profiling::start(profile));

    // Ask the graphics device for GPU timings (see rendering/gpu_canvas.hpp).
    if (gpu_timing)
        qputenv("QSG_RHI_PROFILE", QByteArrayLiteral("1"));
    vss::gui::ProfiledApplication application(argc, argv);
    vss::gui::ProfiledApplication::install_loop_marks();
    QCoreApplication::setOrganizationName(QStringLiteral("VSS Render Bench"));
    QCoreApplication::setApplicationName(QStringLiteral("VSS Render Bench"));
    QSettings().clear();
    QSettings().setValue(QStringLiteral("ui/advanced"), true);
    vss::gui::Theme::instance().initialise();
    vss::gui::rendering::initialise(vss::gui::rendering::render_mode::automatic,
                                    {}, render);
    if (swap_interval >= 0)
    {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(swap_interval);
        QSurfaceFormat::setDefaultFormat(format);
    }

    QElapsedTimer start_up;
    start_up.start();
    vss::studio studio;
    EngineStudioBackend backend(studio);
    double made_ms = 0.0;
    double shown_ms = 0.0;
    ShellWindow window(backend);
    {
        VSS_PROFILE_ZONE("bench.show");
        made_ms = static_cast<double>(start_up.nsecsElapsed()) / 1e6;
        window.resize(window_width, window_height);
        window.show();
        // Until the first frame is on the screen and the loop is quiet.
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        shown_ms = static_cast<double>(start_up.nsecsElapsed()) / 1e6;
    }
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
    const auto made = backend.ports();
    const vss::endpoint_id moved = made.empty() ? 0 : made.front().id;

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
    std::printf("start-up: window made in %.0f ms, first frame shown after "
                "%.0f ms\n",
                made_ms, shown_ms);
    if (const QScreen* screen = window.screen())
    {
        std::printf("display:  %.0f Hz, swap interval %d\n",
                    screen->refreshRate(),
                    QSurfaceFormat::defaultFormat().swapInterval());
    }

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
    {
        VSS_PROFILE_ZONE("bench.repaint");
        report("repaint", measure(*canvas, 300,
                                  [canvas](int) { canvas->request_frame(); }));
    }

    // Pan: scroll the canvas a few pixels each step. Zooming in first takes
    // the view out of automatic fitting and gives the scroll bars a range.
    canvas->zoom_in();
    canvas->zoom_in();
    canvas->zoom_in();
    spin(300);
    {
        VSS_PROFILE_ZONE("bench.pan");
        report("pan", measure(*canvas, 300,
                              [canvas](int index)
                              {
                                  auto* bar = canvas->horizontalScrollBar();
                                  bar->setValue(bar->value() +
                                                (index % 2 == 0 ? 7 : -7));
                              }));
    }

    // Resize: a drag of the corner out and back, as the responsiveness test
    // does.
    // The sweep runs from 70 percent of the window to the full size. Fitting
    // first puts the view back to following the window, as it was before the
    // pan.
    canvas->fit_content();
    spin(300);
    {
        VSS_PROFILE_ZONE("bench.resize");
        report("resize", measure(
                             *canvas, 240,
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
    }

    // Move: a node goes right and back, as a drag does, with its links.
    {
        VSS_PROFILE_ZONE("bench.move");
        const QRectF home = canvas->node_rect(moved);
        report("move",
               measure(*canvas, 300,
                       [canvas, moved, home](int index)
                       {
                           canvas->move_node(
                               moved,
                               home.topLeft() +
                                   QPointF(index % 2 == 0 ? 8.0 : 0.0, 0.0));
                       }));
    }

    // Idle: the ports exist and nothing is sent.
    {
        VSS_PROFILE_ZONE("bench.idle");
        watch("idle", 2000);
    }

    // Traffic: every fixture sends and echoes, as the responsiveness test's
    // busy pair does.
    auto* behaviours = window.findChild<vss::gui::FixtureBehaviours*>();
    if (behaviours != nullptr)
    {
        std::vector<vss::endpoint_id> fixtures;
        for (const auto& port : made)
        {
            if (port.kind == vss::endpoint_kind::mock)
                fixtures.push_back(port.id);
        }
        vss::gui::generator_config config;
        config.kind = vss::gui::generator_kind::random_bytes;
        config.per_second = 50;
        config.chunk = 256;
        for (const vss::endpoint_id id : fixtures)
        {
            behaviours->set_config(id, config);
            static_cast<void>(behaviours->set_echo(id, true));
            static_cast<void>(behaviours->start_generator(id));
        }
        spin(400);
        {
            VSS_PROFILE_ZONE("bench.traffic");
            watch("traffic", 4000);
        }
        for (const vss::endpoint_id id : fixtures)
        {
            behaviours->stop_generator(id);
            static_cast<void>(behaviours->set_echo(id, false));
        }
        spin(200);
    }
    return 0;
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
