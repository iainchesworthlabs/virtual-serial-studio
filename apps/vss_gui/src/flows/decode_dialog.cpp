//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/decode_dialog.hpp"

#include "theme/theme.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace vss::gui
{

DecodeDialog::DecodeDialog(const QString& place_name,
                           std::vector<support::decoder_choice> choices,
                           QWidget* parent)
    : FlowDialog(
          tr("Decode %1").arg(place_name),
          tr("Shows the data on %1 as messages of a protocol, and reports "
             "what does not fit. Decoding only reads. It never changes or "
             "delays the data.")
              .arg(place_name),
          parent),
      place_name_(place_name), choices_(std::move(choices))
{
    setObjectName(QStringLiteral("decodeDialog"));
    auto* layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    protocol_ = new QComboBox(this);
    protocol_->setObjectName(QStringLiteral("decodeProtocol"));
    protocol_->setAccessibleName(tr("Protocol"));
    for (const support::decoder_choice& each : choices_)
    {
        protocol_->addItem(each.type_name);
        if (!each.usable)
        {
            auto* model =
                qobject_cast<QStandardItemModel*>(protocol_->model());
            if (model != nullptr)
            {
                if (auto* item = model->item(protocol_->count() - 1))
                {
                    item->setEnabled(false);
                    item->setToolTip(each.reason);
                }
            }
        }
    }
    // Start on the first decoder that can be used.
    for (std::size_t index = 0; index < choices_.size(); ++index)
    {
        if (choices_[index].usable)
        {
            protocol_->setCurrentIndex(static_cast<int>(index));
            break;
        }
    }
    layout->addWidget(new FormRow(tr("Protocol"), protocol_, {}, this));

    from_ = new QLabel(this);
    from_->setObjectName(QStringLiteral("decodeFrom"));
    from_->setFont(Theme::instance().font(text_style::caption));
    from_->setProperty("muted", true);
    from_->setWordWrap(true);
    layout->addWidget(from_);

    note_ = new NoteBanner(this);
    note_->setObjectName(QStringLiteral("decodeNote"));
    layout->addWidget(note_);
    layout->addStretch(1);
    body_layout()->addLayout(layout);

    auto* cancel = add_footer_button(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* start =
        add_footer_button(tr("Start decoding"), QStringLiteral("primary"));
    connect(start, &QPushButton::clicked, this, &QDialog::accept);
    connect(protocol_, &QComboBox::currentIndexChanged, this,
            [this] { update_state(); });
    update_state();
    finish_layout(420);
    focus_first(protocol_);
}

const support::decoder_choice* DecodeDialog::chosen() const
{
    const int index = protocol_->currentIndex();
    if (index < 0 || index >= static_cast<int>(choices_.size()) ||
        !choices_[static_cast<std::size_t>(index)].usable)
        return nullptr;
    return &choices_[static_cast<std::size_t>(index)];
}

void DecodeDialog::update_state()
{
    const support::decoder_choice* choice = chosen();
    set_primary_enabled(choice != nullptr);
    if (choice != nullptr)
    {
        from_->setText(tr("From the plugin %1.").arg(choice->plugin_name));
        note_->set_kind(NoteBanner::kind::info);
        note_->set_text(tr("%1 will read the data that passes through %2.")
                            .arg(choice->plugin_name, place_name_));
        return;
    }
    from_->clear();
    // Nothing can be started: say why for the one that is chosen, or for the
    // first when there is none.
    const int index = std::max(0, protocol_->currentIndex());
    QString reason;
    if (index < static_cast<int>(choices_.size()))
        reason = choices_[static_cast<std::size_t>(index)].reason;
    note_->set_kind(NoteBanner::kind::warn);
    note_->set_text(reason.isEmpty() ? tr("Not available in this build")
                                     : reason);
}

} // namespace vss::gui
