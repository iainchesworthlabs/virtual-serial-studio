//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/pair_dialog.hpp"

#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace vss::gui
{

PairDialog::PairDialog(StudioBackend& backend, bool advanced,
                       platform_kind platform, QWidget* parent)
    : FlowDialog(tr("Connect two programs"),
                 tr("Create a linked pair of ports. Whatever one program "
                    "sends, the other receives."),
                 parent)
{
    panel_ = new PairPanel(backend, advanced, platform, this);
    body_layout()->addWidget(panel_);

    auto* cancel = add_footer_button(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* create =
        add_footer_button(tr("Create pair"), QStringLiteral("primary"));
    connect(create, &QPushButton::clicked, this, &PairDialog::create_pair);

    connect(panel_, &PairPanel::state_changed, this,
            [this] { set_primary_enabled(panel_->can_create()); });
    connect(panel_, &PairPanel::advanced_requested, this,
            &PairDialog::advanced_requested);
    connect(panel_, &PairPanel::setup_check_requested, this,
            &PairDialog::setup_check_requested);
    set_primary_enabled(panel_->can_create());

    finish_layout(700);
    focus_first(panel_->pair_name_edit());
}

void PairDialog::create_pair()
{
    VSS_PROFILE_ZONE("gui.flows.pair.create");
    flow_error error;
    auto created = panel_->create_pair(&error);
    if (!created)
    {
        if (!error.title.isEmpty())
        {
            Q_EMIT error_shown(error.title);
            ErrorDialog dialog(error.title, error.body, error.details, this);
            bool set_up = false;
            if (error.offer_setup)
            {
                dialog.add_action(
                    tr("Set up Virtual Serial Studio"),
                    [&dialog, &set_up]
                    {
                        set_up = true;
                        dialog.accept();
                    },
                    QStringLiteral("primary"));
            }
            dialog.exec();
            if (set_up)
                Q_EMIT driver_setup_requested();
        }
        return;
    }
    outcome_ = std::move(created);
    accept();
}

} // namespace vss::gui
