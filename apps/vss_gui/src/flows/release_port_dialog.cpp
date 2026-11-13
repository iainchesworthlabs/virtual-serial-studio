//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/release_port_dialog.hpp"

#include "flows/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>
#include <vss/core/types.hpp>

#include <QButtonGroup>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace vss::gui
{

ReleasePortDialog::ReleasePortDialog(StudioBackend& backend,
                                     vss::endpoint_id port, QWidget* parent)
    : FlowDialog(tr("Release port"),
                 tr("Make the program release this port."),
                 parent),
      backend_(backend), port_(port)
{
    const auto view = backend_.port(port);
    port_name_ = view ? port_display_name(*view) : QString();
    holder_view_ = backend_.port_holder(port);

    auto* layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    holder_ = new NoteBanner(NoteBanner::kind::info, program_text(), this);
    holder_->setObjectName(QStringLiteral("releaseHolder"));
    layout->addWidget(holder_);

    // A fact, not a verdict: Studio cannot tell a stuck program from a quiet
    // one, so it says only how long the port has been silent.
    QString idle = tr("No data has passed on %1 yet.").arg(port_name_);
    if (view && view->stats.last_activity != vss::time_point{})
    {
        const auto since = std::chrono::duration_cast<std::chrono::seconds>(
            vss::clock_type::now() - view->stats.last_activity);
        idle = tr("No data has passed on %1 for %2.")
                   .arg(port_name_, idle_text(since));
    }
    idle_ = new NoteBanner(NoteBanner::kind::info, idle, this);
    idle_->setObjectName(QStringLiteral("releaseIdle"));
    layout->addWidget(idle_);

    take_back_ = new PickCard(
        tr("Take the port back"),
        tr("The program keeps running, but its connection to the port stops "
           "working. Another program can open the port at once, with the same "
           "COM number. Try this first."),
        this);
    take_back_->setObjectName(QStringLiteral("releaseTakeBack"));
    end_program_ = new PickCard(
        tr("End the program"),
        tr("Windows closes the program straight away, as End task in Task "
           "Manager does. The program does not get to save anything."),
        this);
    end_program_->setObjectName(QStringLiteral("releaseEndProgram"));
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    group->addButton(take_back_->radio());
    group->addButton(end_program_->radio());
    take_back_->set_selected(true);
    connect(take_back_, &PickCard::selected, this, [this] { update_state(); });
    connect(end_program_, &PickCard::selected, this,
            [this] { update_state(); });
    layout->addWidget(take_back_);
    layout->addWidget(end_program_);

    if (holder_view_ && !holder_view_->can_end)
    {
        end_program_->radio()->setEnabled(false);
        end_program_->setToolTip(
            tr("Studio ends only your own programs. Run Studio as "
               "administrator to end this one."));
        end_program_->set_meta(
            tr("This program belongs to another user. Run Studio as "
               "administrator to end it."));
    }

    layout->addStretch(1);
    body_layout()->addLayout(layout);

    auto* cancel = add_footer_button(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* apply =
        add_footer_button(tr("Release port"), QStringLiteral("primary"));
    connect(apply, &QPushButton::clicked, this, &ReleasePortDialog::release);

    update_state();
    finish_layout(520);
    focus_first(take_back_->radio());

    QWidget::setTabOrder(take_back_->radio(), end_program_->radio());
    QWidget::setTabOrder(end_program_->radio(), cancel);
    QWidget::setTabOrder(cancel, apply);
}

QString ReleasePortDialog::idle_text(std::chrono::seconds idle)
{
    const int seconds =
        idle.count() < 0 ? 0 : static_cast<int>(idle.count());
    if (seconds < 120)
    {
        return seconds == 1 ? tr("%1 second").arg(seconds)
                            : tr("%1 seconds").arg(seconds);
    }
    if (seconds < 7200)
        return tr("%1 minutes").arg(seconds / 60);
    return seconds / 3600 == 1 ? tr("%1 hour").arg(seconds / 3600)
                               : tr("%1 hours").arg(seconds / 3600);
}

QString ReleasePortDialog::program_text() const
{
    if (!holder_view_)
        return tr("No program is known to have %1 open.").arg(port_name_);
    if (holder_view_->process_name.isEmpty())
    {
        return tr("A program (process %1) has %2 open.")
            .arg(holder_view_->process_id)
            .arg(port_name_);
    }
    return tr("%1 (process %2) has %3 open.")
        .arg(holder_view_->process_name)
        .arg(holder_view_->process_id)
        .arg(port_name_);
}

release_method ReleasePortDialog::method() const
{
    return end_program_ != nullptr && end_program_->is_selected()
               ? release_method::end_program
               : release_method::take_back;
}

void ReleasePortDialog::update_state()
{
    const bool known =
        holder_view_.has_value() && backend_.caps().release_port;
    const bool allowed =
        known &&
        (method() == release_method::take_back || holder_view_->can_end);
    set_primary_enabled(allowed && !released_);
    set_primary_text(method() == release_method::end_program
                         ? tr("End program")
                         : tr("Release port"));
}

void ReleasePortDialog::release()
{
    VSS_PROFILE_ZONE("gui.flows.release_port.release");
    if (!primary_button()->isEnabled())
        return;
    const release_method chosen = method();
    const auto outcome = backend_.release_port(port_, chosen);
    if (!outcome)
    {
        QString body;
        if (outcome.error() == vss::errc::permission_denied)
        {
            body = tr("Windows did not allow this. The program belongs to "
                      "another user. Nothing was changed. Run Studio as "
                      "administrator and try again.");
        }
        else if (outcome.error() == vss::errc::not_found)
        {
            body = tr("No program has the port open any more. Nothing was "
                      "changed.");
        }
        else
        {
            body = tr("Studio could not release the port. Nothing was "
                      "changed. Try again.");
        }
        flow_error error;
        error.title = tr("Could not release %1").arg(port_name_);
        error.body = std::move(body);
        error.details = error_text(outcome.error());
        Q_EMIT error_shown(error.title);
        ErrorDialog dialog(error.title, error.body, error.details, this);
        dialog.exec();
        return;
    }
    released_ = true;
    backend_.refresh();
    accept();
}

} // namespace vss::gui
