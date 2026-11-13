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

#include <QSettings>

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
