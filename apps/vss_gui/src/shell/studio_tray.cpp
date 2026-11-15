//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/studio_tray.hpp"

#include "theme/icon_provider.hpp"

#include <vss/core/profile.hpp>

#include <QAction>
#include <QMenu>
#include <QSignalBlocker>
#include <QSystemTrayIcon>

namespace vss::gui
{

StudioTray::StudioTray(QObject* parent) : QObject(parent)
{
    menu_ = new QMenu();
    menu_->setObjectName(QStringLiteral("trayMenu"));
    QAction* open = menu_->addAction(tr("Open Studio"));
    open->setObjectName(QStringLiteral("trayOpen"));
    connect(open, &QAction::triggered, this, &StudioTray::open_requested);
    status_ = menu_->addAction(QString());
    status_->setObjectName(QStringLiteral("trayStatus"));
    status_->setEnabled(false);
    menu_->addSeparator();
    sign_in_ = menu_->addAction(tr("Start when I sign in"));
    sign_in_->setObjectName(QStringLiteral("traySignIn"));
    sign_in_->setCheckable(true);
    connect(sign_in_, &QAction::toggled, this, &StudioTray::sign_in_toggled);
    menu_->addSeparator();
    QAction* quit = menu_->addAction(tr("Stop the ports and quit"));
    quit->setObjectName(QStringLiteral("trayQuit"));
    connect(quit, &QAction::triggered, this, &StudioTray::quit_requested);

    icon_ = new QSystemTrayIcon(this);
    icon_->setContextMenu(menu_);
    connect(icon_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason)
            {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick)
                    Q_EMIT open_requested();
            });
    connect(icon_, &QSystemTrayIcon::messageClicked, this,
            &StudioTray::open_requested);
    set_state(0, 0, false);
}

StudioTray::~StudioTray()
{
    delete menu_;
}

bool StudioTray::available()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void StudioTray::show()
{
    icon_->show();
}

void StudioTray::hide()
{
    icon_->hide();
}

bool StudioTray::visible() const
{
    return icon_->isVisible();
}

void StudioTray::set_state(int ports, int links, bool warn)
{
    VSS_PROFILE_ZONE("gui.shell.tray.set_state");
    text_ = tr("%1 ports, %2 links running").arg(ports).arg(links);
    status_->setText(text_);
    icon_->setToolTip(tr("Virtual Serial Studio: %1").arg(text_));
    if (warn != warn_ || icon_->icon().isNull())
    {
        warn_ = warn;
        apply_icon();
    }
}

QString StudioTray::status_text() const
{
    return text_;
}

void StudioTray::set_sign_in(bool available, bool enabled)
{
    const QSignalBlocker blocker(sign_in_);
    sign_in_->setEnabled(available);
    sign_in_->setChecked(available && enabled);
    sign_in_->setToolTip(available ? QString()
                                   : tr("Not available in this build"));
}

void StudioTray::notify(const QString& title, const QString& text, bool warn)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.shell.tray.notify");
    VSS_PROFILE_ZONE_TEXT(zone, title.toStdString());
    if (!QSystemTrayIcon::supportsMessages() || !icon_->isVisible())
        return;
    icon_->showMessage(title, text,
                       warn ? QSystemTrayIcon::Warning
                            : QSystemTrayIcon::Information);
}

void StudioTray::apply_icon()
{
    // The notification area wants a small, clear mark; the theme's icons are
    // drawn in its colours, so the warn state needs no literal colour.
    icon_->setIcon(
        warn_ ? IconProvider::instance().icon(QStringLiteral("warning"),
                                              colour_role::amber, 32)
              : IconProvider::instance().icon(QStringLiteral("connect"),
                                              colour_role::accent, 32));
}

} // namespace vss::gui
