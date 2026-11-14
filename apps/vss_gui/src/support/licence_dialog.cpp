//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/licence_dialog.hpp"

#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QAccessible>
#include <QClipboard>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

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

QFrame* card(QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setProperty("card", true);
    polish(frame);
    return frame;
}

/// Tells assistive technology about a change, politely: it waits for the
/// person to finish what they were hearing.
void announce_politely(QWidget* widget, const QString& text)
{
    if (widget == nullptr || text.isEmpty())
        return;
    QAccessibleAnnouncementEvent event(widget, text);
    event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

void set_primary(QPushButton* button, bool primary)
{
    button->setProperty("role", primary ? QVariant(QStringLiteral("primary"))
                                        : QVariant());
    polish(button);
}

bool is_licence_file(const QString& path)
{
    return path.endsWith(QLatin1String(".vsslic"), Qt::CaseInsensitive);
}

QString local_path_of(const QMimeData* mime)
{
    if (mime == nullptr || !mime->hasUrls())
        return {};
    for (const QUrl& url : mime->urls())
    {
        if (url.isLocalFile() && is_licence_file(url.toLocalFile()))
            return url.toLocalFile();
    }
    return {};
}

void copy_with_toast(QWidget* anchor, const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
    ToastManager::instance().show(
        anchor,
        QCoreApplication::translate("LicenceDialog", "%1 copied.").arg(text));
}

} // namespace

StatusChip::kind chip_kind(licence_tone tone)
{
    switch (tone)
    {
    case licence_tone::neutral:
        return StatusChip::kind::neutral;
    case licence_tone::neutral_hollow:
        return StatusChip::kind::closed;
    case licence_tone::ok:
        return StatusChip::kind::healthy;
    case licence_tone::warn:
        return StatusChip::kind::waiting;
    case licence_tone::danger:
        return StatusChip::kind::failed;
    }
    return StatusChip::kind::neutral;
}

NoteBanner::kind banner_kind(licence_note_tone tone)
{
    return tone == licence_note_tone::warn ? NoteBanner::kind::warn
                                           : NoteBanner::kind::info;
}

// -- RenewDialog
// -----------------------------------------------------------------

RenewDialog::RenewDialog(const QString& licence_id, QWidget* parent)
    : QDialog(parent), licence_id_(licence_id)
{
    setWindowTitle(renew_title());
    setAccessibleName(renew_title());
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 0);
    layout->setSpacing(12);
    add_dialog_heading(layout, renew_title(), {}, this);
    auto* text = text_label(renew_text(licence_id_), this);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(text);
    layout->addStretch(1);

    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    copy_number_ = make_dialog_button(
        QCoreApplication::translate("LicenceDialog", "Copy licence number"),
        nullptr, footer);
    copy_number_->setAutoDefault(false);
    connect(copy_number_, &QPushButton::clicked, this,
            [this] { copy_with_toast(this, licence_id_); });
    footer_layout->addWidget(copy_number_);
    copy_address_ = make_dialog_button(
        QCoreApplication::translate("LicenceDialog", "Copy address"), nullptr,
        footer);
    copy_address_->setAutoDefault(false);
    connect(copy_address_, &QPushButton::clicked, this,
            [this] { copy_with_toast(this, support_address()); });
    footer_layout->addWidget(copy_address_);
    close_ = make_dialog_button(
        QCoreApplication::translate("LicenceDialog", "Close"), "primary",
        footer);
    close_->setDefault(true);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close_);
    layout->addWidget(footer);

    close_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close_,
                       [this] { close_->setFocus(Qt::OtherFocusReason); });
}

// -- LicenceDialog
// ---------------------------------------------------------------

LicenceDialog::LicenceDialog(StudioBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend)
{
    setAcceptDrops(true);
    build();
    refresh();
    connect(&backend_, &StudioBackend::licence_changed, this,
            &LicenceDialog::on_licence_changed);
}

void LicenceDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.licence.build");
    VSS_PROFILE_MESSAGE("gui.dialog.licence.opened");
    const QString title =
        QCoreApplication::translate("LicenceDialog", "Licence");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    const int width = Theme::instance().metric(
        QStringLiteral("layout.dialogWidths.standard"));
    resize(width, 680);
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
            "LicenceDialog",
            "Shows which edition this is and what your licence includes. "
            "Studio checks a licence on this computer and never contacts a "
            "server."),
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

    // -- The summary card
    // -------------------------------------------------------
    QFrame* summary = card(body);
    summary->setObjectName(QStringLiteral("licenceSummary"));
    auto* summary_layout = new QVBoxLayout(summary);
    summary_layout->setContentsMargins(16, 14, 16, 14);
    summary_layout->setSpacing(10);

    auto* top = new QHBoxLayout();
    top->setSpacing(12);
    edition_ = new QLabel(summary);
    edition_->setObjectName(QStringLiteral("licenceEdition"));
    edition_->setFont(Theme::instance().font(text_style::display));
    top->addWidget(edition_, 0, Qt::AlignVCenter);
    state_chip_ = new StatusChip(summary);
    state_chip_->setObjectName(QStringLiteral("licenceStateChip"));
    top->addWidget(state_chip_, 0, Qt::AlignVCenter);
    top->addStretch(1);
    summary_layout->addLayout(top);

    form_ = new QFormLayout();
    form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form_->setHorizontalSpacing(16);
    form_->setVerticalSpacing(8);
    summary_layout->addLayout(form_);

    const auto row_label = [summary](const QString& text)
    { return muted_label(text, summary); };

    edition_value_ = text_label({}, summary);
    edition_value_->setObjectName(QStringLiteral("licenceRowEdition"));
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Edition")),
        edition_value_);

    licensee_ = text_label({}, summary);
    licensee_->setObjectName(QStringLiteral("licenceRowLicensee"));
    licensee_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Licensed to")),
        licensee_);

    number_row_ = new QWidget(summary);
    auto* number_layout = new QHBoxLayout(number_row_);
    number_layout->setContentsMargins(0, 0, 0, 0);
    number_layout->setSpacing(6);
    number_ = fixed_label(number_row_);
    number_->setObjectName(QStringLiteral("licenceRowNumber"));
    // A licence number is one word: never wrapped.
    number_->setWordWrap(false);
    number_layout->addWidget(number_, 0, Qt::AlignVCenter);
    number_copy_ = new CopyButton(number_row_);
    number_copy_->setObjectName(QStringLiteral("licenceCopyNumber"));
    number_layout->addWidget(number_copy_, 0, Qt::AlignVCenter);
    number_layout->addStretch(1);
    connect(number_copy_, &CopyButton::copied, this,
            [this]
            {
                ToastManager::instance().show(
                    this,
                    QCoreApplication::translate("LicenceDialog", "%1 copied.")
                        .arg(licence_id_));
            });
    form_->addRow(row_label(QCoreApplication::translate("LicenceDialog",
                                                        "Licence number")),
                  number_row_);

    term_ = text_label({}, summary);
    term_->setObjectName(QStringLiteral("licenceRowTerm"));
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Term")), term_);

    status_ = text_label({}, summary);
    status_->setObjectName(QStringLiteral("licenceRowStatus"));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Status")),
        status_);

    seats_row_ = new QWidget(summary);
    auto* seats_layout = new QVBoxLayout(seats_row_);
    seats_layout->setContentsMargins(0, 0, 0, 0);
    seats_layout->setSpacing(2);
    seats_ = text_label({}, seats_row_);
    seats_->setObjectName(QStringLiteral("licenceRowSeats"));
    seats_layout->addWidget(seats_);
    seats_note_ = muted_label({}, seats_row_, text_style::caption);
    seats_note_->setObjectName(QStringLiteral("licenceRowSeatsNote"));
    seats_layout->addWidget(seats_note_);
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Seats")),
        seats_row_);

    computers_row_ = new QWidget(summary);
    auto* computers_layout = new QVBoxLayout(computers_row_);
    computers_layout->setContentsMargins(0, 0, 0, 0);
    computers_layout->setSpacing(2);
    computers_ = fixed_label(computers_row_);
    computers_->setObjectName(QStringLiteral("licenceRowComputers"));
    computers_layout->addWidget(computers_);
    this_computer_ = text_label({}, computers_row_);
    this_computer_->setObjectName(QStringLiteral("licenceRowThisComputer"));
    computers_layout->addWidget(this_computer_);
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Computers")),
        computers_row_);

    includes_ = text_label({}, summary);
    includes_->setObjectName(QStringLiteral("licenceRowIncludes"));
    form_->addRow(
        row_label(QCoreApplication::translate("LicenceDialog", "Includes")),
        includes_);
    body_layout->addWidget(summary);

    // -- Notes under the card
    // ---------------------------------------------------
    state_note_ = new NoteBanner(body);
    state_note_->setObjectName(QStringLiteral("licenceStateNote"));
    state_note_->set_action(
        QCoreApplication::translate("LicenceDialog", "How to renew"),
        [this] { show_renew(); });
    state_note_->hide();
    body_layout->addWidget(state_note_);

    import_note_ = new NoteBanner(body);
    import_note_->setObjectName(QStringLiteral("licenceImportNote"));
    import_note_->hide();
    body_layout->addWidget(import_note_);

    // -- What this edition includes
    // ----------------------------------------------
    body_layout->addWidget(
        text_label(QCoreApplication::translate("LicenceDialog",
                                               "What this edition includes"),
                   body, text_style::heading));
    QFrame* table = card(body);
    table->setObjectName(QStringLiteral("licenceFeatures"));
    auto* grid = new QGridLayout(table);
    grid->setContentsMargins(16, 12, 16, 12);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(10);
    grid->setColumnStretch(2, 1);
    grid->addWidget(
        row_label(QCoreApplication::translate("LicenceDialog", "Feature")), 0,
        0);
    grid->addWidget(
        row_label(QCoreApplication::translate("LicenceDialog", "Status")), 0,
        1);
    grid->addWidget(
        row_label(QCoreApplication::translate("LicenceDialog", "Why")), 0, 2);
    int grid_row = 1;
    for (const QString& feature : known_features())
    {
        auto* name = text_label(feature_name(feature), table);
        name->setObjectName(QStringLiteral("featureName.") + feature);
        grid->addWidget(name, grid_row, 0, Qt::AlignTop);
        auto* chip = new StatusChip(table);
        chip->setObjectName(QStringLiteral("featureChip.") + feature);
        grid->addWidget(chip, grid_row, 1, Qt::AlignTop);
        auto* why = text_label({}, table);
        why->setObjectName(QStringLiteral("featureWhy.") + feature);
        grid->addWidget(why, grid_row, 2, Qt::AlignTop);
        features_.push_back({feature, chip, why});
        ++grid_row;
    }
    body_layout->addWidget(table);

    // -- Other licences
    // ----------------------------------------------------------
    others_ = new Disclosure(QString(), body);
    others_->setObjectName(QStringLiteral("licenceOthers"));
    others_->hide();
    body_layout->addWidget(others_);
    body_layout->addStretch(1);

    // -- Footer
    // ------------------------------------------------------------------
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    remove_ = make_dialog_button(
        QCoreApplication::translate("LicenceDialog", "Remove licence"), "quiet",
        footer);
    remove_->setObjectName(QStringLiteral("licenceRemove"));
    remove_->setAutoDefault(false);
    connect(remove_, &QPushButton::clicked, this,
            [this]
            {
                if (!removable_file_.isEmpty())
                    remove_file(removable_file_);
            });
    // Remove sits at the left of the footer, apart from the buttons that
    // finish the dialog.
    footer_layout->insertWidget(0, remove_);
    import_ = make_dialog_button(
        QCoreApplication::translate("LicenceDialog", "Import licence..."),
        nullptr, footer);
    import_->setObjectName(QStringLiteral("licenceImport"));
    import_->setAutoDefault(false);
    connect(import_, &QPushButton::clicked, this,
            [this] { choose_and_import(); });
    footer_layout->addWidget(import_);
    close_ = make_dialog_button(
        QCoreApplication::translate("LicenceDialog", "Close"), "primary",
        footer);
    close_->setObjectName(QStringLiteral("licenceClose"));
    close_->setDefault(true);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close_);
    outer->addWidget(footer);

    // Initial focus is on the safest control.
    close_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close_,
                       [this] { close_->setFocus(Qt::OtherFocusReason); });
}

void LicenceDialog::set_row_visible(QWidget* field, bool visible)
{
    form_->setRowVisible(field, visible);
}

void LicenceDialog::on_licence_changed()
{
    refresh();
    // A note about an import describes the licence just imported. Once the
    // licence changes for another reason it no longer applies.
    if (!importing_)
        set_import_note(std::nullopt);
}

void LicenceDialog::refresh()
{
    VSS_PROFILE_ZONE("gui.support.licence.refresh");
    const licence_view view = backend_.licence();
    const lic::status& status = view.status;
    computer_name_ = view.computer_name;
    licence_id_ = status.value
                      ? QString::fromStdString(status.value->licence_id)
                      : QString();

    edition_->setText(edition_value(status));
    edition_->setAccessibleName(edition_->text());
    const licence_chip chip = state_chip(status);
    state_chip_->set_kind(chip_kind(chip.tone));
    state_chip_->set_text(chip.text);

    edition_value_->setText(edition_value(status));

    // Rows with no value are left out, not shown empty.
    const bool has_value = status.value.has_value();
    const QString licensee =
        has_value ? QString::fromStdString(status.value->licensee_name)
                  : QString();
    licensee_->setText(licensee);
    set_row_visible(licensee_, !licensee.isEmpty());

    number_->setText(licence_id_);
    number_->setAccessibleName(licence_id_);
    number_copy_->set_copy_text(
        licence_id_,
        QCoreApplication::translate("LicenceDialog", "licence number"));
    set_row_visible(number_row_, !licence_id_.isEmpty());

    const bool dated = has_value;
    term_->setText(dated ? term_text(*status.value) : QString());
    set_row_visible(term_, dated);

    // The sentence is a polite live region: it is announced when it changes,
    // without moving focus.
    const QString sentence = state_sentence(status, computer_name_);
    const bool sentence_changed = status_->text() != sentence;
    status_->setText(sentence);
    status_->setAccessibleName(sentence);
    if (sentence_changed && isVisible())
        announce_politely(status_, sentence);

    const bool seated = has_value;
    seats_->setText(seated ? seats_text(status.value->seats) : QString());
    seats_note_->setText(seated && status.value->seats > 0 ? seat_explanation()
                                                           : QString());
    seats_note_->setVisible(!seats_note_->text().isEmpty());
    set_row_visible(seats_row_, seated);

    const bool names_computers = has_value && !status.value->machines.empty();
    QStringList machines;
    if (names_computers)
    {
        for (const std::string& machine : status.value->machines)
            machines << QString::fromStdString(machine);
    }
    computers_->setText(machines.join(QLatin1Char('\n')));
    this_computer_->setText(
        QCoreApplication::translate("LicenceDialog", "This computer is %1.")
            .arg(computer_name_));
    set_row_visible(computers_row_, names_computers);

    // Includes: one line for each feature the licence turns on now.
    QStringList included;
    for (const QString& feature : known_features())
    {
        if (backend_.entitlement(feature).granted)
            included << feature_name(feature);
    }
    includes_->setText(included.join(QLatin1Char('\n')));
    set_row_visible(includes_, !included.isEmpty());

    set_state_note(state_note(status));

    // The table: one row for each feature the licence service knows.
    for (const feature_row& row : features_)
    {
        const lic::entitlement answer = backend_.entitlement(row.feature);
        const licence_chip look = feature_chip(answer.granted);
        row.chip->set_kind(chip_kind(look.tone));
        row.chip->set_text(look.text);
        row.why->setText(answer.granted
                             ? QString()
                             : reason_sentence(answer.reason, status));
    }

    refresh_others(view);

    // The footer. Remove only applies to a licence of the user's own.
    removable_file_.clear();
    for (const licence_file_view& file : view.files)
    {
        if (file.in_use && !file.machine_wide)
        {
            removable_file_ = file.file_name;
            break;
        }
    }
    const bool had_focus = remove_->hasFocus();
    remove_->setVisible(!removable_file_.isEmpty());
    if (had_focus && removable_file_.isEmpty())
        close_->setFocus(Qt::OtherFocusReason);

    const bool can_import = backend_.caps().licences;
    import_->setEnabled(can_import);
    import_->setToolTip(can_import ? QString() : not_available_text());
    import_->setAccessibleDescription(can_import ? QString()
                                                 : not_available_text());
    const bool wants_import = status.state == lic::state::none ||
                              status.state == lic::state::expired ||
                              status.state == lic::state::invalid;
    set_primary(import_, wants_import);
    set_primary(close_, !wants_import);
}

void LicenceDialog::refresh_others(const licence_view& view)
{
    std::vector<const licence_file_view*> others;
    for (const licence_file_view& file : view.files)
    {
        if (!file.in_use)
            others.push_back(&file);
    }
    // Only when more than one file is installed.
    if (view.files.size() < 2 || others.empty())
    {
        others_->hide();
        return;
    }

    others_->set_title(
        QCoreApplication::translate("LicenceDialog",
                                    "Other licences on this computer (%1)")
            .arg(others.size()));
    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(8);
    for (const licence_file_view* file : others)
    {
        const lic::status& status = file->status;
        QFrame* row = card(content);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(12, 8, 12, 8);
        row_layout->setSpacing(12);

        auto* details = new QVBoxLayout();
        details->setSpacing(2);
        auto* number = fixed_label(row);
        number->setText(status.value
                            ? QString::fromStdString(status.value->licence_id)
                            : file->file_name);
        details->addWidget(number);
        auto* facts = new QHBoxLayout();
        facts->setSpacing(12);
        facts->addWidget(
            muted_label(edition_value(status), row, text_style::caption));
        if (status.value)
            facts->addWidget(muted_label(term_text(*status.value), row,
                                         text_style::caption));
        facts->addStretch(1);
        details->addLayout(facts);
        row_layout->addLayout(details, 1);

        const licence_chip look = state_chip(status);
        auto* chip = new StatusChip(look.text, chip_kind(look.tone), row);
        chip->set_small(true);
        row_layout->addWidget(chip, 0, Qt::AlignVCenter);

        if (file->machine_wide)
        {
            auto* admin = new StatusChip(
                QCoreApplication::translate("LicenceDialog",
                                            "Installed by an administrator"),
                StatusChip::kind::neutral, row);
            admin->setObjectName(QStringLiteral("licenceAdminChip"));
            admin->set_small(true);
            row_layout->addWidget(admin, 0, Qt::AlignVCenter);
        }
        else
        {
            auto* remove = make_dialog_button(
                QCoreApplication::translate("LicenceDialog", "Remove"), "quiet",
                row);
            remove->setObjectName(QStringLiteral("licenceRemoveOther"));
            remove->setAutoDefault(false);
            remove->setAccessibleDescription(number->text());
            const QString file_name = file->file_name;
            connect(remove, &QPushButton::clicked, this,
                    [this, file_name] { remove_file(file_name); });
            row_layout->addWidget(remove, 0, Qt::AlignVCenter);
        }
        layout->addWidget(row);
    }
    layout->addWidget(
        muted_label(QCoreApplication::translate(
                        "LicenceDialog",
                        "Studio uses the licence that gives you the most."),
                    content, text_style::caption));
    others_->set_content(content);
    others_->show();
}

void LicenceDialog::set_state_note(const std::optional<licence_banner>& note)
{
    if (!note)
    {
        state_note_->hide();
        shown_state_note_.reset();
        return;
    }
    // Setting a banner's text announces it, so only do it when it changed.
    const bool changed = !shown_state_note_ ||
                         shown_state_note_->text != note->text ||
                         shown_state_note_->tone != note->tone;
    if (changed)
    {
        state_note_->set_kind(banner_kind(note->tone));
        state_note_->set_text(note->text);
    }
    if (state_note_->action_button() != nullptr)
        state_note_->action_button()->setVisible(note->renew);
    state_note_->show();
    shown_state_note_ = note;
}

void LicenceDialog::set_import_note(const std::optional<licence_banner>& note)
{
    if (!note)
    {
        import_note_->hide();
        return;
    }
    import_note_->set_kind(banner_kind(note->tone));
    import_note_->set_text(note->text);
    import_note_->show();
}

void LicenceDialog::choose_and_import()
{
    if (!backend_.caps().licences)
        return;
    QString path;
    if (picker_)
    {
        path = picker_();
    }
    else
    {
        // Two filters, as the spec lists them.
        const QString filter =
            QCoreApplication::translate("LicenceImport",
                                        "Licence files (*.vsslic)") +
            QStringLiteral(";;") +
            QCoreApplication::translate("LicenceImport", "All files (*)");
        path = QFileDialog::getOpenFileName(
            this,
            QCoreApplication::translate("LicenceImport", "Import a licence"),
            QString(), filter);
    }
    if (!path.isEmpty())
        import_file(path);
}

void LicenceDialog::import_file(const QString& path)
{
    VSS_PROFILE_ZONE("gui.support.licence.import");
    if (!backend_.caps().licences)
        return;
    importing_ = true;
    const licence_import_result result = backend_.import_licence(path);
    importing_ = false;

    switch (result.outcome)
    {
    case licence_import_outcome::imported:
    {
        // The backend announced the change; read it again in case it did not.
        refresh();
        set_import_note(import_note(result.status));
        // Valid and ending-soon licences are announced by a toast. An ended
        // one is explained by the banner, and a licence that cannot work now
        // by the status sentence.
        if (result.status.state == lic::state::valid ||
            result.status.state == lic::state::expiring)
        {
            ToastManager::instance().show(this, imported_toast(result.status));
        }
        break;
    }
    case licence_import_outcome::already_installed:
        ToastManager::instance().show(this, already_installed_toast());
        break;
    case licence_import_outcome::not_valid:
    case licence_import_outcome::unreadable:
        // Nothing was copied, and nothing shown changes.
        show_import_failure(result, path);
        break;
    case licence_import_outcome::unsupported:
        ToastManager::instance().show(this, not_available_text());
        break;
    }
}

void LicenceDialog::show_import_failure(const licence_import_result& result,
                                        const QString& path)
{
    const QString name = QFileInfo(path).fileName();
    bool again = false;
    ErrorDialog error(QCoreApplication::translate(
                          "LicenceImport", "Could not import the licence"),
                      import_failure_body(result, name, computer_name_),
                      import_failure_details(result, name), this);
    error.add_action(
        QCoreApplication::translate("LicenceImport", "Choose another file"),
        [&again, &error]
        {
            again = true;
            error.accept();
        });
    error.exec();
    if (again)
        choose_and_import();
}

void LicenceDialog::remove_file(const QString& file_name)
{
    ConfirmDialog dialog(
        QCoreApplication::translate("LicenceDialog", "Remove this licence?"),
        QCoreApplication::translate(
            "LicenceDialog",
            "Studio returns to the Community edition. Your ports, setups and "
            "files are unchanged. Keep the licence file so you can import it "
            "again."),
        QCoreApplication::translate("LicenceDialog", "Remove licence"),
        QCoreApplication::translate("LicenceDialog", "Cancel"), true, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    static_cast<void>(backend_.remove_licence(file_name));
    refresh();
    set_import_note(std::nullopt);
}

void LicenceDialog::show_renew()
{
    RenewDialog dialog(licence_id_, this);
    dialog.exec();
}

void LicenceDialog::dragEnterEvent(QDragEnterEvent* event)
{
    if (backend_.caps().licences && !local_path_of(event->mimeData()).isEmpty())
        event->acceptProposedAction();
    else
        QDialog::dragEnterEvent(event);
}

void LicenceDialog::dropEvent(QDropEvent* event)
{
    const QString path = local_path_of(event->mimeData());
    if (path.isEmpty() || !backend_.caps().licences)
    {
        QDialog::dropEvent(event);
        return;
    }
    event->acceptProposedAction();
    // Import after the drop has finished: an error dialog must not run inside
    // the drag and drop loop.
    QTimer::singleShot(0, this, [this, path] { import_file(path); });
}

} // namespace vss::gui::support
