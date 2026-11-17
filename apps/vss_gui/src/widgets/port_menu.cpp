//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/port_menu.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QMenu>

namespace vss::gui
{
namespace
{

QString text(const char* source)
{
    return QCoreApplication::translate("PortMenu", source);
}

QString send_label()
{
    return text("Send bytes");
}

QString repeat_label(bool running)
{
    return running ? text("Stop repeat") : text("Start repeat");
}

QString monitor_label()
{
    return text("Open in monitor");
}

QString decode_label()
{
    return text("Decode as...");
}

QString generator_label(bool running)
{
    return running ? text("Stop generator") : text("Start generator");
}

QString echo_label()
{
    return text("Echo received bytes");
}

} // namespace

QStringList port_menu_labels(const port_menu_state& state)
{
    QStringList labels;
    labels << send_label();
    if (state.advanced)
        labels << repeat_label(state.repeating);
    labels << monitor_label();
    if (state.advanced && state.decode_shown)
        labels << decode_label();
    if (state.fixture)
        labels << generator_label(state.generating) << echo_label();
    return labels;
}

void add_port_menu_actions(QMenu* menu, QObject* context,
                           const port_menu_state& state,
                           std::function<void(port_menu_action)> trigger)
{
    const auto add = [&](const QString& label, port_menu_action action)
    {
        return menu->addAction(label, context, [trigger, action]
                               { trigger(action); });
    };

    add(send_label(), port_menu_action::send_bytes);
    if (state.advanced)
        add(repeat_label(state.repeating), port_menu_action::toggle_repeat);
    add(monitor_label(), port_menu_action::open_monitor);
    if (state.advanced && state.decode_shown)
    {
        QAction* decode = add(decode_label(), port_menu_action::decode_as);
        decode->setEnabled(state.decode_enabled);
        if (!state.decode_enabled)
            decode->setToolTip(state.decode_reason);
    }
    if (!state.fixture)
        return;

    add(generator_label(state.generating), port_menu_action::toggle_generator);
    QAction* echo = add(echo_label(), port_menu_action::toggle_echo);
    echo->setCheckable(true);
    echo->setChecked(state.echoing);
    if (!state.echo_available)
    {
        echo->setEnabled(false);
        echo->setToolTip(text("Not available in this build"));
    }
}

} // namespace vss::gui
