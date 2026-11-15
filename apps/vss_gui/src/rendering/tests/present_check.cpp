//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Does a window that holds the GPU canvas reach the screen? A window drawn
// through the graphics API is not in QWidget::grab(), so the other GUI tests
// cannot see a window that the system never shows. This one shows a window
// with the GPU canvas on the real platform, copies what the screen shows at
// that place, and compares it with the colour the canvas fills.
//
// Exit codes: 0 the window shows the canvas; 1 the window is blank (white) or
// the canvas has no graphics device; 77 the check cannot be made here (no
// display, or something else covers the window), which CTest reports as
// skipped.
//
// --old-present asks for swap interval 0 whatever the environment says, as
// Studio did before the fix, to show that the check fails with
// QT_D3D_NO_FLIP=1 on a system that allows tearing.
//

#include "rendering/gpu_canvas.hpp"
#include "rendering/render_mode.hpp"
#include "rendering/render_state.hpp"
#include "theme/theme.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QScreen>
#include <QSurfaceFormat>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

namespace rendering = vss::gui::rendering;

constexpr int kSkip = 77;

bool close_to(QColor a, QColor b)
{
    constexpr int kTolerance = 24;
    return std::abs(a.red() - b.red()) <= kTolerance &&
           std::abs(a.green() - b.green()) <= kTolerance &&
           std::abs(a.blue() - b.blue()) <= kTolerance;
}

void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    bool old_present = false;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--old-present") == 0)
            old_present = true;
    }

    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("vss-present-check"));
    const auto state =
        rendering::initialise(rendering::render_mode::hardware, {}, {});
    if (state.active != rendering::renderer::hardware)
    {
        std::fputs("present check: skipped, no display on this platform\n",
                   stdout);
        return kSkip;
    }
    if (old_present)
    {
        QSurfaceFormat format = QSurfaceFormat::defaultFormat();
        format.setSwapInterval(0);
        QSurfaceFormat::setDefaultFormat(format);
    }

    auto& theme = vss::gui::Theme::instance();
    theme.initialise();
    theme.set_mode(vss::gui::theme_mode::dark);
    const QColor ground = theme.colour(vss::gui::colour_role::sunken);

    QWidget window(nullptr, Qt::Window | Qt::WindowStaysOnTopHint);
    window.setWindowTitle(QStringLiteral("vss present check"));
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    QGraphicsScene scene;
    auto* view = new QGraphicsView(&scene);
    view->setFrameShape(QFrame::NoFrame);
    layout->addWidget(view);
    auto* canvas = new rendering::GpuCanvas(*view);
    rendering::GpuCanvas::style style;
    style.ground = ground;
    style.dots = ground;
    canvas->set_style(style);
    bool device_ready = false;
    rendering::gpu_report report;
    QObject::connect(canvas, &rendering::GpuCanvas::device_ready, &window,
                     [&device_ready, &report](const rendering::gpu_report& got)
                     {
                         device_ready = true;
                         report = got;
                     });

    window.resize(480, 320);
    window.show();
    window.raise();
    window.activateWindow();
    pump(2000);
    window.raise();
    pump(500);

    if (!device_ready || !report.device_created)
    {
        std::fprintf(stdout, "present check: the canvas has no device (%s)\n",
                     report.error.toLocal8Bit().constData());
        return EXIT_FAILURE;
    }

    QScreen* screen = window.screen();
    if (screen == nullptr)
    {
        std::fputs("present check: skipped, no screen\n", stdout);
        return kSkip;
    }
    // From the whole screen, not the window: only the screen holds what the
    // graphics API presented.
    const QPoint origin = view->mapToGlobal(QPoint(0, 0));
    const QImage shot = screen
                            ->grabWindow(0, origin.x(), origin.y(),
                                         view->width(), view->height())
                            .toImage();
    if (shot.isNull())
    {
        std::fputs("present check: skipped, the screen cannot be read\n",
                   stdout);
        return kSkip;
    }

    int samples = 0;
    int matching = 0;
    int white = 0;
    const QColor blank(Qt::white);
    for (int y = shot.height() / 8; y < shot.height() * 7 / 8; y += 7)
    {
        for (int x = shot.width() / 8; x < shot.width() * 7 / 8; x += 7)
        {
            const QColor pixel = shot.pixelColor(x, y);
            ++samples;
            matching += close_to(pixel, ground) ? 1 : 0;
            white += close_to(pixel, blank) ? 1 : 0;
        }
    }
    std::fprintf(stdout,
                 "present check: %s on %s, %d samples, %d the canvas colour, "
                 "%d white\n",
                 report.backend.toLocal8Bit().constData(),
                 report.device.toLocal8Bit().constData(), samples, matching,
                 white);
    if (samples > 0 && matching * 10 >= samples * 9)
        return EXIT_SUCCESS;
    if (samples > 0 && white * 10 >= samples * 9)
    {
        std::fputs("present check: FAILED, the window is blank\n", stdout);
        return EXIT_FAILURE;
    }
    std::fputs("present check: skipped, something else covers the window\n",
               stdout);
    return kSkip;
}
