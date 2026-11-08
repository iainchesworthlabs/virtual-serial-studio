//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/watch_dialog.hpp"

#include "backend/other_ports.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <iterator>

namespace vss::gui
{

WatchDialog::WatchDialog(StudioBackend& backend, vss::endpoint_id selected,
                         platform_kind platform, QWidget* parent)
    : FlowDialog(tr("Watch traffic"),
                 tr("See what flows through a port without changing it."),
                 parent),
      backend_(backend), platform_(platform)
{
    build();

    auto* cancel = add_footer_button(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* start =
        add_footer_button(tr("Start watching"), QStringLiteral("primary"));
    start->setIcon(IconProvider::instance().icon(QStringLiteral("watch"),
                                                 colour_role::ink, 16));
    connect(start, &QPushButton::clicked, this, &WatchDialog::start_watching);

    const auto ports = backend_.ports();
    const auto initial = std::find_if(ports.begin(), ports.end(),
                                      [selected](const port_view& port)
                                      { return port.id == selected; });
    if (initial != ports.end())
        port_->setCurrentIndex(
            static_cast<int>(std::distance(ports.begin(), initial)));
    if (port_->currentIndex() >= 0)
        update_for_source();
    update_primary_state();
    finish_layout(600);
    focus_first(port_);

    QWidget::setTabOrder(port_, name_);
    QWidget::setTabOrder(name_, open_monitor_);
    QWidget::setTabOrder(open_monitor_, cancel);
    QWidget::setTabOrder(cancel, start);
}

void WatchDialog::build()
{
    VSS_PROFILE_ZONE("gui.flows.watch.build");
    VSS_PROFILE_MESSAGE("gui.dialog.watch.opened");
    auto* layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    // Port picker: every port the engine reports, labelled by path or as
    // Studio only.
    port_ = new QComboBox(this);
    port_->setObjectName(QStringLiteral("watchPort"));
    port_->setAccessibleName(tr("Port to watch"));
    const auto ports = backend_.ports();
    for (const auto& port : ports)
    {
        const QString label = port.studio_only
                                  ? tr("%1 (Studio only)").arg(port.name)
                                  : tr("%1 (%2)").arg(port.name, port.path);
        port_->addItem(label, static_cast<qulonglong>(port.id));
    }
    // Ports another program made are not in the list.  Say so beside the
    // list when there are some, so the list does not look complete.
    const auto others = backend_.other_program_ports();
    port_->setPlaceholderText(tr("No ports available"));
    layout->addWidget(new FormRow(
        tr("Port to watch"), port_,
        ports.empty() ? QString() : other_ports_reason(others), this));

    // The watcher is a new port. Its name is suggested from the chosen port
    // until the user types their own (see name_auto_).
    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("watchName"));
    name_->setAccessibleName(tr("Name of the watcher"));
    name_row_ = new FormRow(
        tr("Name of the watcher"), name_,
        tr("A new Studio-only port that receives a read-only copy."), this);
    layout->addWidget(name_row_);

    open_monitor_ =
        new QCheckBox(tr("Open the monitor when it is ready"), this);
    open_monitor_->setObjectName(QStringLiteral("watchOpenMonitor"));
    open_monitor_->setChecked(true);
    open_monitor_->setAccessibleName(tr("Open the monitor when it is ready"));
    layout->addWidget(open_monitor_);

    // Explanatory text; update_for_source() sets it for the chosen port.
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("watchInfo"));
    info_->setTextFormat(Qt::RichText);
    info_->setFont(Theme::instance().font(text_style::body));
    info_->setWordWrap(true);
    layout->addWidget(info_);

    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("watchCard"));
    card->setProperty("card", true);
    polish(card);
    auto* card_layout = new QHBoxLayout(card);
    card_layout->setContentsMargins(12, 10, 14, 10);
    card_layout->setSpacing(12);
    auto* chip =
        new StatusChip(tr("Studio only"), StatusChip::kind::studio_only, card);
    card_layout->addWidget(chip, 0, Qt::AlignLeft | Qt::AlignVCenter);
    auto* card_text = new QLabel(
        tr("The watcher is shown in the ports list so you can remove it when "
           "you are finished."),
        card);
    card_text->setFont(Theme::instance().font(text_style::body));
    card_text->setProperty("muted", true);
    card_text->setWordWrap(true);
    card_layout->addWidget(card_text, 1);
    layout->addWidget(card);

    // Warning shown only when this build cannot link ports.
    unavailable_ =
        new NoteBanner(NoteBanner::kind::warn,
                       tr("Watching is not available in this build."), this);
    unavailable_->setObjectName(QStringLiteral("watchUnavailable"));
    unavailable_->setVisible(false);
    layout->addWidget(unavailable_);
    layout->addStretch(1);

    body_layout()->addLayout(layout);

    // Changing the port refreshes the suggested name and info. Typing in the
    // name stops the automatic naming.
    connect(port_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                update_for_source();
                update_primary_state();
            });
    connect(name_, &QLineEdit::textEdited, this,
            [this]
            {
                name_auto_ = false;
                name_row_->clear_error();
                update_primary_state();
            });
    connect(name_, &QLineEdit::textChanged, this,
            [this]
            {
                name_row_->clear_error();
                update_primary_state();
            });

    // Explain an empty list, and warn when this build has no link support,
    // which watching needs.
    if (ports.empty())
        show_empty_state(others, layout);
    if (!backend_.caps().links)
    {
        unavailable_->setVisible(true);
        unavailable_->set_text(
            tr("Watching needs link support in this build."));
    }
    update_for_source();
    update_primary_state();
}

void WatchDialog::show_empty_state(const std::vector<device_view>& others,
                                   QVBoxLayout* layout)
{
    // Say why the list is empty and what to do next.  The reason is either
    // that nothing exists yet or that what exists belongs to another program,
    // which the engine can neither list with live state nor watch.
    port_->setEnabled(false);
    const QString reason = other_ports_reason(others);
    auto* empty = new NoteBanner(
        reason.isEmpty() ? NoteBanner::kind::info : NoteBanner::kind::warn,
        reason.isEmpty()
            ? tr("No ports to watch yet. Create a port first.")
            : tr("%1 Create a port in Studio instead, then watch that.").arg(reason),
        this);
    empty->setObjectName(QStringLiteral("watchEmpty"));
    empty->set_action(tr("Create a port"),
                      [this]
                      {
                          create_port_requested_ = true;
                          Q_EMIT create_port_requested();
                          reject();
                      });
    empty_ = empty;
    layout->insertWidget(0, empty);
}

void WatchDialog::update_for_source()
{
    VSS_PROFILE_ZONE("gui.flows.watch.update_for_source");
    if (port_ == nullptr || info_ == nullptr)
        return;
    const auto id =
        static_cast<vss::endpoint_id>(port_->currentData().toULongLong());
    const auto view = backend_.port(id);
    if (!view)
    {
        info_->clear();
        info_->setAccessibleName({});
        return;
    }
    source_ = id;
    if (name_auto_ || name_->text().isEmpty())
        name_->setText(tr("%1-monitor").arg(view->name));

    info_->setText(
        tr("Watching never changes what <b>%1</b> sees. The watcher is a "
           "read-only copy, and it never drives modem lines.")
            .arg(view->name.toHtmlEscaped()));
    info_->setAccessibleName(
        tr("Watching never changes what %1 sees. The watcher is a read-only "
           "copy, and it never drives modem lines.")
            .arg(view->name));
    info_->setVisible(true);
}

void WatchDialog::update_primary_state()
{
    const bool has_port = port_ != nullptr && port_->currentIndex() >= 0;
    bool enabled = has_port && name_ != nullptr &&
                   !name_->text().trimmed().isEmpty() &&
                   backend_.caps().links && backend_.caps().create_ports;
    set_primary_enabled(enabled);
}

void WatchDialog::start_watching()
{
    VSS_PROFILE_ZONE("gui.flows.watch.start");
    name_row_->clear_error();
    const auto id =
        static_cast<vss::endpoint_id>(port_->currentData().toULongLong());
    const auto source = backend_.port(id);
    if (!source)
    {
        flow_error error;
        error.title = tr("Could not watch this port");
        error.body = tr("The port is no longer available. Nothing was "
                        "changed. Choose another port and try again.");
        Q_EMIT error_shown(error.title);
        ErrorDialog dialog(error.title, error.body, {}, this);
        dialog.exec();
        return;
    }
    if (name_->text().trimmed().isEmpty())
    {
        name_row_->set_error(tr("Enter a name for the watcher."));
        name_->setFocus(Qt::OtherFocusReason);
        return;
    }

    const auto created = backend_.create_virtual_port(
        name_->text().trimmed(), vss::virtual_port_kind::loopback, true);
    if (!created)
    {
        flow_error error;
        error.title = error_title(tr("watch"), source->path);
        error.body = error_body(
            created.error(),
            tr("Studio could not create the watcher. Nothing was changed."),
            tr(" Try again."));
        error.details = error_text(created.error());
        Q_EMIT error_shown(error.title);
        ErrorDialog dialog(error.title, error.body, error.details, this);
        dialog.exec();
        return;
    }

    const auto linked = backend_.monitor(id, *created);
    if (!linked)
    {
        static_cast<void>(backend_.remove_port(*created));
        flow_error error;
        error.title = error_title(tr("watch"), source->path);
        error.body = error_body(
            linked.error(),
            tr("Studio could not create the read-only monitor link. Nothing "
               "was changed."),
            tr(" Try again."));
        error.details = error_text(linked.error());
        Q_EMIT error_shown(error.title);
        ErrorDialog dialog(error.title, error.body, error.details, this);
        dialog.exec();
        return;
    }

    source_ = id;
    watcher_ = *created;
    link_ = *linked;
    backend_.refresh();
    accept();
}

} // namespace vss::gui
