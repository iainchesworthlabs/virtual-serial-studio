//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the canvas is drawn: the Rendering preference, the request from the
// environment and the command line, and the fallback rule. None of this needs
// a GPU: the rule takes a description of the graphics device.
//

#include "test_framework.hpp"

#include "rendering/render_mode.hpp"
#include "rendering/render_state.hpp"
#include "support/settings.hpp"

#include <QByteArray>
#include <QSettings>
#include <QSurfaceFormat>

namespace
{

namespace rendering = vss::gui::rendering;
namespace support = vss::gui::support;
using rendering::render_mode;
using rendering::renderer;
using rendering::request_source;

rendering::gpu_report real_gpu()
{
    rendering::gpu_report report;
    report.device_created = true;
    report.backend = QStringLiteral("Direct3D 11");
    report.device = QStringLiteral("Test GPU");
    return report;
}

rendering::gpu_report cpu_renderer()
{
    auto report = real_gpu();
    report.device = QStringLiteral("Microsoft Basic Render Driver");
    report.cpu_device = true;
    return report;
}

rendering::gpu_report no_device()
{
    rendering::gpu_report report;
    report.error = QStringLiteral("test");
    return report;
}

} // namespace

TEST_CASE("rendering: the keys round trip and unknown keys mean automatic")
{
    for (const auto mode :
         {render_mode::automatic, render_mode::hardware, render_mode::software})
    {
        bool ok = false;
        REQUIRE(rendering::mode_from_key(rendering::mode_key(mode), &ok) ==
                mode);
        REQUIRE(ok);
    }
    bool ok = true;
    REQUIRE(rendering::mode_from_key(QStringLiteral("fast"), &ok) ==
            render_mode::automatic);
    REQUIRE(!ok);
    REQUIRE(rendering::mode_from_key(QStringLiteral(" GPU ")) ==
            render_mode::hardware);
    REQUIRE(rendering::mode_from_key(QStringLiteral("CPU")) ==
            render_mode::software);
}

TEST_CASE("rendering: the command line beats the environment beats the setting")
{
    auto request = rendering::choose_request(
        render_mode::hardware, QStringLiteral("software"), QStringLiteral("auto"));
    REQUIRE(request.mode == render_mode::automatic);
    REQUIRE(request.source == request_source::command_line);

    request = rendering::choose_request(render_mode::hardware,
                                        QStringLiteral("software"), {});
    REQUIRE(request.mode == render_mode::software);
    REQUIRE(request.source == request_source::environment);

    request = rendering::choose_request(render_mode::hardware, {}, {});
    REQUIRE(request.mode == render_mode::hardware);
    REQUIRE(request.source == request_source::setting);

    // A value nobody understands is ignored, not an error.
    request = rendering::choose_request(
        render_mode::software, QStringLiteral("turbo"), QStringLiteral("fast"));
    REQUIRE(request.mode == render_mode::software);
    REQUIRE(request.source == request_source::setting);
}

TEST_CASE("rendering: software is never a fallback")
{
    const auto result = rendering::decide(render_mode::software, real_gpu());
    REQUIRE(result.active == renderer::software);
    REQUIRE(!result.fell_back);
    REQUIRE(result.reason.isEmpty());
}

TEST_CASE("rendering: a graphics device means hardware")
{
    for (const auto mode : {render_mode::automatic, render_mode::hardware})
    {
        const auto result = rendering::decide(mode, real_gpu());
        REQUIRE(result.active == renderer::hardware);
        REQUIRE(!result.fell_back);
    }
}

TEST_CASE("rendering: no device falls back to software and says why")
{
    for (const auto mode : {render_mode::automatic, render_mode::hardware})
    {
        const auto result = rendering::decide(mode, no_device());
        REQUIRE(result.active == renderer::software);
        REQUIRE(result.fell_back);
        REQUIRE(!result.reason.isEmpty());
    }
}

TEST_CASE("rendering: automatic refuses a CPU renderer, hardware accepts it")
{
    const auto automatic =
        rendering::decide(render_mode::automatic, cpu_renderer());
    REQUIRE(automatic.active == renderer::software);
    REQUIRE(automatic.fell_back);
    REQUIRE(automatic.reason.contains(QStringLiteral("Basic Render Driver")));

    const auto forced =
        rendering::decide(render_mode::hardware, cpu_renderer());
    REQUIRE(forced.active == renderer::hardware);
    REQUIRE(!forced.fell_back);
}

TEST_CASE("rendering: confirming a missing device switches to software")
{
    rendering::render_state state;
    state.requested = render_mode::automatic;
    state.active = renderer::hardware;
    rendering::set_state(state);

    REQUIRE(rendering::confirm_gpu(real_gpu()));
    REQUIRE(rendering::current().active == renderer::hardware);
    REQUIRE(rendering::describe().contains(QStringLiteral("Test GPU")));

    REQUIRE(!rendering::confirm_gpu(no_device()));
    REQUIRE(rendering::current().active == renderer::software);
    REQUIRE(rendering::current().fell_back);
    REQUIRE(rendering::describe().contains(QStringLiteral("Software")));
    rendering::set_state({});
}

TEST_CASE("rendering: the offscreen platform is software")
{
    // The test programs run on the offscreen platform.
    const auto state = rendering::initialise(render_mode::hardware, {}, {});
    REQUIRE(state.requested == render_mode::hardware);
    REQUIRE(state.active == renderer::software);
    REQUIRE(!state.fell_back);
    rendering::set_state({});
}

TEST_CASE("rendering: no wait for the display unless it is asked for")
{
    REQUIRE(rendering::choose_present({}, {}).no_wait);
    REQUIRE(rendering::choose_present({}, QStringLiteral("0")).no_wait);
    REQUIRE(!rendering::choose_present(QStringLiteral("1"), {}).no_wait);
    REQUIRE(rendering::choose_present(QStringLiteral("0"), {}).no_wait);
}

TEST_CASE("rendering: the copying swap chain is never asked for without a wait")
{
    // Regression: Studio set QT_D3D_NO_FLIP=1 and swap interval 0 together.
    // Direct3D will not make a copying swap chain that allows tearing, which
    // is what Qt asks for at swap interval 0 where tearing is offered, so the
    // window had no swap chain and stayed white.
    for (const auto* value : {"1", " 1", "2"})
    {
        REQUIRE(
            !rendering::choose_present({}, QString::fromLatin1(value)).no_wait);
    }
}

TEST_CASE("rendering: preparing the GPU window leaves the swap chain to Qt")
{
    const QSurfaceFormat saved = QSurfaceFormat::defaultFormat();
    const bool had_flip = qEnvironmentVariableIsSet("QT_D3D_NO_FLIP");
    const QByteArray flip = qgetenv("QT_D3D_NO_FLIP");
    const bool had_vsync = qEnvironmentVariableIsSet("VSS_RENDER_VSYNC");
    const QByteArray vsync = qgetenv("VSS_RENDER_VSYNC");
    const auto reset = [&saved]
    {
        QSurfaceFormat format = saved;
        format.setSwapInterval(1);
        QSurfaceFormat::setDefaultFormat(format);
    };

    qunsetenv("QT_D3D_NO_FLIP");
    qunsetenv("VSS_RENDER_VSYNC");
    reset();
    rendering::prepare_present();
    REQUIRE(QSurfaceFormat::defaultFormat().swapInterval() == 0);
    // The blank window came from Studio setting this variable itself.
    REQUIRE(!qEnvironmentVariableIsSet("QT_D3D_NO_FLIP"));

    qputenv("QT_D3D_NO_FLIP", QByteArrayLiteral("1"));
    reset();
    rendering::prepare_present();
    REQUIRE(QSurfaceFormat::defaultFormat().swapInterval() == 1);

    qunsetenv("QT_D3D_NO_FLIP");
    qputenv("VSS_RENDER_VSYNC", QByteArrayLiteral("1"));
    reset();
    rendering::prepare_present();
    REQUIRE(QSurfaceFormat::defaultFormat().swapInterval() == 1);

    if (had_flip)
        qputenv("QT_D3D_NO_FLIP", flip);
    else
        qunsetenv("QT_D3D_NO_FLIP");
    if (had_vsync)
        qputenv("VSS_RENDER_VSYNC", vsync);
    else
        qunsetenv("VSS_RENDER_VSYNC");
    QSurfaceFormat::setDefaultFormat(saved);
}

TEST_CASE("rendering: the preference is remembered")
{
    QSettings().clear();
    REQUIRE(support::load_ui_settings().render_mode == render_mode::automatic);

    auto settings = support::load_ui_settings();
    settings.render_mode = render_mode::software;
    support::save_ui_settings(settings);
    REQUIRE(support::load_ui_settings().render_mode == render_mode::software);
    REQUIRE(QSettings().value(QStringLiteral("ui/renderMode")).toString() ==
            QStringLiteral("software"));

    settings.render_mode = render_mode::hardware;
    support::save_ui_settings(settings);
    REQUIRE(support::load_ui_settings().render_mode == render_mode::hardware);
    QSettings().clear();
}
