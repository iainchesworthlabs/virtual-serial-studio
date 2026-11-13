//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/settings.hpp"

#include <vss/core/profile.hpp>

#include <QSettings>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

constexpr const char* kAdvanced = "ui/advanced";
constexpr const char* kTheme = "ui/theme";
constexpr const char* kDensity = "ui/density";
constexpr const char* kLinkStyle = "ui/linkStyle";
constexpr const char* kRefreshMs = "ui/refreshMs";
constexpr const char* kDefaultBaud = "ui/defaultBaud";
constexpr const char* kAskOnDiscard = "ui/askOnDiscard";
constexpr const char* kCloseAction = "ui/closeAction";
constexpr const char* kProfileOnStart = "ui/profileOnStart";

} // namespace

ui_settings default_ui_settings()
{
    return ui_settings{};
}

ui_settings load_ui_settings()
{
    VSS_PROFILE_ZONE("gui.support.settings.load");
    const QSettings store;
    ui_settings settings;
    settings.advanced = store.value(QLatin1String(kAdvanced), false).toBool();
    settings.theme = theme_from_key(
        store.value(QLatin1String(kTheme), QStringLiteral("system"))
            .toString());
    settings.density = density_from_key(
        store.value(QLatin1String(kDensity), QStringLiteral("comfortable"))
            .toString());
    settings.links = link_style_from_key(
        store.value(QLatin1String(kLinkStyle), QStringLiteral("curved"))
            .toString());
    settings.refresh_ms = std::clamp(
        store.value(QLatin1String(kRefreshMs), kRefreshDefaultMs).toInt(),
        kRefreshMinMs, kRefreshMaxMs);
    settings.default_baud =
        store.value(QLatin1String(kDefaultBaud), kDefaultBaudRate).toInt();
    settings.ask_on_discard =
        store.value(QLatin1String(kAskOnDiscard), true).toBool();
    settings.keep_running_on_close =
        store.value(QLatin1String(kCloseAction), QStringLiteral("background"))
            .toString() != QLatin1String("quit");
    settings.profile_on_start =
        store.value(QLatin1String(kProfileOnStart), false).toBool();
    return settings;
}

void save_ui_settings(const ui_settings& settings)
{
    VSS_PROFILE_ZONE("gui.support.settings.save");
    QSettings store;
    store.setValue(QLatin1String(kAdvanced), settings.advanced);
    store.setValue(QLatin1String(kTheme), theme_key(settings.theme));
    store.setValue(QLatin1String(kDensity), density_key(settings.density));
    store.setValue(QLatin1String(kLinkStyle), link_style_key(settings.links));
    store.setValue(
        QLatin1String(kRefreshMs),
        std::clamp(settings.refresh_ms, kRefreshMinMs, kRefreshMaxMs));
    store.setValue(QLatin1String(kDefaultBaud), settings.default_baud);
    store.setValue(QLatin1String(kAskOnDiscard), settings.ask_on_discard);
    store.setValue(QLatin1String(kCloseAction),
                   settings.keep_running_on_close ? QStringLiteral("background")
                                                  : QStringLiteral("quit"));
    store.setValue(QLatin1String(kProfileOnStart), settings.profile_on_start);
}

QString theme_key(theme_mode mode)
{
    switch (mode)
    {
    case theme_mode::light:
        return QStringLiteral("light");
    case theme_mode::dark:
        return QStringLiteral("dark");
    case theme_mode::system:
        break;
    }
    return QStringLiteral("system");
}

theme_mode theme_from_key(const QString& key)
{
    if (key == QLatin1String("light"))
        return theme_mode::light;
    if (key == QLatin1String("dark"))
        return theme_mode::dark;
    return theme_mode::system;
}

QString density_key(density_mode mode)
{
    return mode == density_mode::compact ? QStringLiteral("compact")
                                         : QStringLiteral("comfortable");
}

density_mode density_from_key(const QString& key)
{
    return key == QLatin1String("compact") ? density_mode::compact
                                           : density_mode::comfortable;
}

QString link_style_key(link_style style)
{
    return style == link_style::elbow ? QStringLiteral("elbow")
                                      : QStringLiteral("curved");
}

link_style link_style_from_key(const QString& key)
{
    return key == QLatin1String("elbow") ? link_style::elbow
                                         : link_style::curved;
}

QStringList baud_rate_choices()
{
    return {QStringLiteral("115200"), QStringLiteral("9600"),
            QStringLiteral("19200"),  QStringLiteral("38400"),
            QStringLiteral("57600"),  QStringLiteral("230400")};
}

} // namespace vss::gui::support
