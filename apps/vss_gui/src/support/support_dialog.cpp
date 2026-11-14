//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/support_dialog.hpp"

#include "support/licence_dialog.hpp"
#include "support/licence_text.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

namespace lic = vss::license;

QLabel* text_label(const QString& text, QWidget* parent,
                   text_style style = text_style::body)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(style));
    label->setWordWrap(true);
    return label;
}

QLabel* muted_label(const QString& text, QWidget* parent,
                    text_style style = text_style::body)
{
    QLabel* label = text_label(text, parent, style);
    label->setProperty("muted", true);
    polish(label);
    return label;
}

QLabel* fixed_label(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setFont(Theme::instance().font(text_style::data));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

/// A card with its heading. The caller adds the rest to the layout.
QFrame* card(QWidget* parent, const QString& heading, QVBoxLayout*& layout)
{
    auto* frame = new QFrame(parent);
    frame->setProperty("card", true);
    polish(frame);
    layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    layout->addWidget(text_label(heading, frame, text_style::heading));
    return frame;
}

QPushButton* card_button(const QString& text, QWidget* parent,
                         const char* role = nullptr)
{
    QPushButton* button = make_dialog_button(text, role, parent);
    button->setAutoDefault(false);
    button->setAccessibleName(text);
    return button;
}

bool has_support_feature(const lic::status& status)
{
    if (!status.value)
        return false;
    return std::ranges::any_of(
        status.value->features, [](const std::string& entry)
        { return lic::feature_matches(entry, "support"); });
}

} // namespace

support_variant support_variant_of(const lic::status& status,
                                   const lic::entitlement& support)
{
    // A licence in its grace period has ended: its plugins run on for a
    // while, and support does not.
    if (support.granted && status.state != lic::state::grace)
        return support_variant::included;
    const bool ended = status.state == lic::state::grace ||
                       status.state == lic::state::expired;
    if (ended && has_support_feature(status))
        return support_variant::ended;
    return support_variant::community;
}

SupportDialog::SupportDialog(StudioBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend), info_(current_build_info())
{
    build();
    refresh();
    connect(&backend_, &StudioBackend::licence_changed, this,
            &SupportDialog::refresh);
}

void SupportDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.support_dialog.build");
    VSS_PROFILE_MESSAGE("gui.dialog.support.opened");
    const QString title =
        QCoreApplication::translate("SupportDialog", "Support");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    const int width = Theme::instance().metric(
        QStringLiteral("layout.dialogWidths.standard"));
    resize(width, 700);
    setMinimumWidth(width);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    header_layout->setSpacing(4);
    add_dialog_heading(
        header_layout, title,
        QCoreApplication::translate(
            "SupportDialog",
            "Where to get help, and what to send so a helper can understand "
            "your setup."),
        header);
    outer->addWidget(header);

    // The body scrolls inside the dialog, so it stays usable at 200 percent
    // text.
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFocusPolicy(Qt::NoFocus);
    auto* body = new QWidget(scroll);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 4, 24, 16);
    body_layout->setSpacing(14);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // -- Your support --------------------------------------------------------
    QVBoxLayout* layout = nullptr;
    QFrame* support =
        card(body, QCoreApplication::translate("SupportDialog", "Your support"),
             layout);
    support->setObjectName(QStringLiteral("supportCard"));
    chip_ = new StatusChip(support);
    chip_->setObjectName(QStringLiteral("supportChip"));
    auto* chip_row = new QHBoxLayout();
    chip_row->addWidget(chip_, 0, Qt::AlignVCenter);
    chip_row->addStretch(1);
    layout->addLayout(chip_row);
    sentence_ = text_label({}, support);
    sentence_->setObjectName(QStringLiteral("supportSentence"));
    sentence_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(sentence_);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    address_ = card_button(
        QCoreApplication::translate("SupportDialog", "Copy address"), support);
    address_->setObjectName(QStringLiteral("supportCopyAddress"));
    connect(address_, &QPushButton::clicked, this,
            [this] { copy_with_toast(support_address()); });
    buttons->addWidget(address_);
    number_ = card_button(
        QCoreApplication::translate("SupportDialog", "Copy licence number"),
        support);
    number_->setObjectName(QStringLiteral("supportCopyNumber"));
    connect(number_, &QPushButton::clicked, this,
            [this] { copy_with_toast(licence_id_); });
    buttons->addWidget(number_);
    documents_ = card_button(
        QCoreApplication::translate("SupportDialog", "Copy documents address"),
        support);
    documents_->setObjectName(QStringLiteral("supportCopyDocuments"));
    connect(documents_, &QPushButton::clicked, this,
            [this] { copy_with_toast(documents_address()); });
    buttons->addWidget(documents_);
    issues_ = card_button(
        QCoreApplication::translate("SupportDialog", "Copy issues address"),
        support);
    issues_->setObjectName(QStringLiteral("supportCopyIssues"));
    connect(issues_, &QPushButton::clicked, this,
            [this] { copy_with_toast(issues_address()); });
    buttons->addWidget(issues_);
    renew_ = card_button(
        QCoreApplication::translate("SupportDialog", "How to renew"), support);
    renew_->setObjectName(QStringLiteral("supportRenew"));
    connect(renew_, &QPushButton::clicked, this, [this] { show_renew(); });
    buttons->addWidget(renew_);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    body_layout->addWidget(support);

    body_layout->addWidget(muted_label(
        QCoreApplication::translate(
            "SupportDialog",
            "Studio never sends anything itself. You choose what to share."),
        body, text_style::caption));

    // -- Before you write ----------------------------------------------------
    QFrame* before = card(
        body, QCoreApplication::translate("SupportDialog", "Before you write"),
        layout);
    before->setObjectName(QStringLiteral("supportBefore"));
    auto* steps = new QGridLayout();
    steps->setHorizontalSpacing(10);
    steps->setVerticalSpacing(6);
    steps->setColumnStretch(1, 1);
    const QString lines[] = {
        QCoreApplication::translate(
            "SupportDialog",
            "Say what you expected and what happened instead."),
        QCoreApplication::translate(
            "SupportDialog", "Say which port or program it happened with."),
        QCoreApplication::translate(
            "SupportDialog",
            "Attach a support bundle. It answers most first questions."),
    };
    int row = 0;
    for (const QString& line : lines)
    {
        ++row;
        auto* number = muted_label(QLocale().toString(row) + QLatin1Char('.'),
                                   before, text_style::body);
        number->setWordWrap(false);
        number->setFont(Theme::instance().font(text_style::data));
        steps->addWidget(number, row - 1, 0, Qt::AlignTop);
        steps->addWidget(text_label(line, before), row - 1, 1);
    }
    layout->addLayout(steps);
    body_layout->addWidget(before);

    // -- Support bundle ------------------------------------------------------
    QFrame* bundle = card(
        body, QCoreApplication::translate("SupportDialog", "Support bundle"),
        layout);
    bundle->setObjectName(QStringLiteral("supportBundleCard"));
    layout->addWidget(text_label(
        QCoreApplication::translate(
            "SupportDialog",
            "A support bundle is one zip file with the version, the driver "
            "state, your setup, the log and the list of serial ports on this "
            "computer. You see everything in it before you save it."),
        bundle));
    auto* bundle_row = new QHBoxLayout();
    bundle_ = card_button(
        QCoreApplication::translate("SupportDialog", "Create support bundle"),
        bundle, "primary");
    bundle_->setObjectName(QStringLiteral("supportCreateBundle"));
    connect(bundle_, &QPushButton::clicked, this,
            &SupportDialog::bundle_requested);
    bundle_row->addWidget(bundle_);
    bundle_row->addStretch(1);
    layout->addLayout(bundle_row);
    body_layout->addWidget(bundle);

    // -- This copy of Studio -------------------------------------------------
    QFrame* copy = card(
        body,
        QCoreApplication::translate("SupportDialog", "This copy of Studio"),
        layout);
    copy->setObjectName(QStringLiteral("supportCopyCard"));
    form_ = new QFormLayout();
    form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form_->setHorizontalSpacing(16);
    form_->setVerticalSpacing(6);
    const auto add_value = [this, copy](const QString& label,
                                        const QString& name) -> QLabel*
    {
        QLabel* value = fixed_label(copy);
        value->setObjectName(name);
        form_->addRow(muted_label(label, copy), value);
        return value;
    };
    version_ =
        add_value(QCoreApplication::translate("SupportDialog", "Version"),
                  QStringLiteral("supportVersion"));
    edition_ =
        add_value(QCoreApplication::translate("SupportDialog", "Edition"),
                  QStringLiteral("supportEdition"));
    licence_ = add_value(
        QCoreApplication::translate("SupportDialog", "Licence number"),
        QStringLiteral("supportLicence"));
    system_ = add_value(
        QCoreApplication::translate("SupportDialog", "Operating system"),
        QStringLiteral("supportSystem"));
    layout->addLayout(form_);
    auto* copy_row = new QHBoxLayout();
    details_ =
        card_button(QCoreApplication::translate("SupportDialog", "Copy"), copy);
    details_->setObjectName(QStringLiteral("supportCopyDetails"));
    // The name says what is copied; it still contains the visible word.
    details_->setAccessibleName(QCoreApplication::translate(
        "SupportDialog", "Copy the version, edition and operating system"));
    connect(details_, &QPushButton::clicked, this,
            [this]
            {
                QGuiApplication::clipboard()->setText(details_text());
                ToastManager::instance().show(
                    this, QCoreApplication::translate("SupportDialog",
                                                      "Details copied."));
            });
    copy_row->addWidget(details_);
    copy_row->addStretch(1);
    layout->addLayout(copy_row);
    body_layout->addWidget(copy);
    body_layout->addStretch(1);

    // -- Footer --------------------------------------------------------------
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    help_ = make_dialog_button(
        QCoreApplication::translate("SupportDialog", "Help contents"), "quiet",
        footer);
    help_->setObjectName(QStringLiteral("supportHelp"));
    help_->setAutoDefault(false);
    connect(help_, &QPushButton::clicked, this, &SupportDialog::help_requested);
    footer_layout->insertWidget(0, help_);
    troubleshoot_ =
        make_dialog_button(QCoreApplication::translate(
                               "SupportDialog", "Why is nothing arriving?"),
                           "quiet", footer);
    troubleshoot_->setObjectName(QStringLiteral("supportTroubleshoot"));
    troubleshoot_->setAutoDefault(false);
    connect(troubleshoot_, &QPushButton::clicked, this,
            &SupportDialog::troubleshoot_requested);
    footer_layout->insertWidget(1, troubleshoot_);
    close_ = make_dialog_button(
        QCoreApplication::translate("SupportDialog", "Close"), "primary",
        footer);
    close_->setObjectName(QStringLiteral("supportClose"));
    close_->setDefault(true);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close_);
    outer->addWidget(footer);

    // Initial focus is on the safest control.
    close_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close_,
                       [this] { close_->setFocus(Qt::OtherFocusReason); });
}

void SupportDialog::refresh()
{
    VSS_PROFILE_ZONE("gui.support.support_dialog.refresh");
    const licence_view view = backend_.licence();
    const lic::status& status = view.status;
    licence_id_ = status.value
                      ? QString::fromStdString(status.value->licence_id)
                      : QString();
    variant_ = support_variant_of(status, backend_.entitlement("support"));

    QString sentence;
    switch (variant_)
    {
    case support_variant::included:
        chip_->set_kind(StatusChip::kind::healthy);
        chip_->set_text(
            QCoreApplication::translate("SupportDialog", "Support included"));
        sentence =
            QCoreApplication::translate(
                "SupportDialog",
                "Your %1 licence includes support until %2. Write to "
                "%3 and quote your licence number, %4.")
                .arg(edition_label(status),
                     status.value ? licence_date(status.value->expires_at)
                                  : QString(),
                     support_address(), licence_id_);
        break;
    case support_variant::community:
        chip_->set_kind(StatusChip::kind::neutral);
        chip_->set_text(
            QCoreApplication::translate("SupportDialog", "Community support"));
        sentence = QCoreApplication::translate(
            "SupportDialog",
            "The Community edition is supported by its users. Search the "
            "documents and the issue list first, then open an issue. Support "
            "from the developer comes with a Pro or Enterprise licence.");
        break;
    case support_variant::ended:
        chip_->set_kind(StatusChip::kind::waiting);
        chip_->set_text(
            QCoreApplication::translate("SupportDialog", "Support ended"));
        sentence =
            QCoreApplication::translate(
                "SupportDialog", "Support ended with your licence on %1.")
                .arg(status.value ? licence_date(status.value->expires_at)
                                  : QString());
        break;
    }
    sentence_->setText(sentence);
    sentence_->setAccessibleName(sentence);
    chip_->setAccessibleName(chip_->text());

    const bool included = variant_ == support_variant::included;
    const bool community = variant_ == support_variant::community;
    const bool ended = variant_ == support_variant::ended;
    address_->setVisible(included);
    number_->setVisible(included);
    documents_->setVisible(community);
    issues_->setVisible(community);
    renew_->setVisible(ended);

    version_->setText(info_.full_version());
    edition_->setText(edition_value(status));
    licence_->setText(licence_id_);
    form_->setRowVisible(licence_, !licence_id_.isEmpty());
    system_->setText(info_.operating_system);
}

QString SupportDialog::details_text() const
{
    // For the person who answers, so plain English on purpose.
    const lic::status status = backend_.licence().status;
    QString text = QStringLiteral("Version: %1\nEdition: %2\n")
                       .arg(info_.full_version(), edition_value(status));
    if (!licence_id_.isEmpty())
        text += QStringLiteral("Licence number: %1\n").arg(licence_id_);
    text +=
        QStringLiteral("Operating system: %1\n").arg(info_.operating_system);
    return text;
}

void SupportDialog::copy_with_toast(const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
    ToastManager::instance().show(
        this,
        QCoreApplication::translate("SupportDialog", "%1 copied.").arg(text));
}

void SupportDialog::show_renew()
{
    RenewDialog dialog(licence_id_, this);
    dialog.exec();
}

} // namespace vss::gui::support
