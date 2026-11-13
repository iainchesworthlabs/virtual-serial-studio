//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/support_bundle_dialog.hpp"

#include "support/build_info.hpp"
#include "support/setup_facts.hpp"
#include "support/zip_writer.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

/// The size of a file as the list shows it: "1.2 kB". The traditional format
/// counts 1024 bytes to the kB, as the 256 kB limit of the preview does.
QString size_text(qsizetype bytes)
{
    return QLocale().formattedDataSize(bytes, 1,
                                       QLocale::DataSizeTraditionalFormat);
}

QString count_text(int files)
{
    if (files == 1)
        return QCoreApplication::translate("SupportBundleDialog", "1 file");
    return QCoreApplication::translate("SupportBundleDialog", "%n files",
                                       nullptr, files);
}

/// A row of the checklist. A click anywhere on it, on the words as well as on
/// the box, ticks or unticks the box.
class click_row final : public QFrame
{
public:
    using QFrame::QFrame;

    void set_check(QCheckBox* check) { check_ = check; }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (check_ != nullptr && check_->isEnabled() &&
            event->button() == Qt::LeftButton &&
            rect().contains(event->position().toPoint()))
        {
            check_->setFocus(Qt::MouseFocusReason);
            check_->click();
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    QCheckBox* check_ = nullptr;
};

constexpr int size_role = Qt::UserRole + 1;

/// The file name in the fixed font at the left of a row, and its size at the
/// right in the muted ink. Colours and fonts come from the theme.
class file_row_delegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        const QString name = item.text;
        item.text.clear();
        item.icon = QIcon();
        const QWidget* widget = item.widget;
        QStyle* style =
            widget != nullptr ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

        const Theme& theme = Theme::instance();
        const QRect area = item.rect.adjusted(10, 0, -10, 0);
        painter->save();
        painter->setFont(theme.font(text_style::caption));
        painter->setPen(theme.colour(colour_role::ink3));
        const QString size = index.data(size_role).toString();
        const int size_width =
            painter->fontMetrics().horizontalAdvance(size) + 12;
        painter->drawText(area, Qt::AlignRight | Qt::AlignVCenter, size);
        painter->setFont(theme.font(text_style::data));
        painter->setPen(theme.colour(colour_role::ink));
        const QRect name_area = area.adjusted(0, 0, -size_width, 0);
        painter->drawText(name_area, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(
                              name, Qt::ElideMiddle, name_area.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), Theme::instance().row_height()));
        return size;
    }
};

QLabel* caption_label(const QString& text, QWidget* parent, bool semibold)
{
    auto* label = new QLabel(text, parent);
    QFont font = Theme::instance().font(text_style::caption);
    if (semibold)
        font.setWeight(QFont::DemiBold);
    label->setFont(font);
    label->setWordWrap(true);
    return label;
}

/// "Could not save the bundle". The spec lists the buttons in this order:
/// "Copy details" (quiet), "Choose another place" (primary), Cancel.
class save_failed_dialog final : public QDialog
{
public:
    save_failed_dialog(const QString& details, QWidget* parent)
        : QDialog(parent), details_(details)
    {
        const QString title = QCoreApplication::translate(
            "SupportBundleDialog", "Could not save the bundle");
        setObjectName(QStringLiteral("bundleSaveFailed"));
        setWindowTitle(title);
        setAccessibleName(title);
        setProperty("surface", QStringLiteral("panel"));
        polish(this);
        setMinimumWidth(Theme::instance().metric(
            QStringLiteral("layout.dialogWidths.small")));

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 20, 20, 0);
        layout->setSpacing(12);
        add_dialog_heading(
            layout, title,
            QCoreApplication::translate(
                "SupportBundleDialog",
                "Studio could not write to that folder. Choose another place, "
                "such as your Documents folder."),
            this);
        layout->addStretch(1);

        QHBoxLayout* footer_layout = nullptr;
        QFrame* footer = make_dialog_footer(this, footer_layout);
        auto* copy = make_dialog_button(
            QCoreApplication::translate("SupportBundleDialog", "Copy details"),
            "quiet", footer);
        copy->setAutoDefault(false);
        connect(copy, &QPushButton::clicked, this,
                [this] { QGuiApplication::clipboard()->setText(details_); });
        footer_layout->addWidget(copy);
        auto* again = make_dialog_button(
            QCoreApplication::translate("SupportBundleDialog",
                                        "Choose another place"),
            "primary", footer);
        again->setDefault(true);
        connect(again, &QPushButton::clicked, this,
                [this]
                {
                    another_ = true;
                    accept();
                });
        footer_layout->addWidget(again);
        auto* cancel = make_dialog_button(
            QCoreApplication::translate("SupportBundleDialog", "Cancel"),
            nullptr, footer);
        cancel->setAutoDefault(false);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        footer_layout->addWidget(cancel);
        layout->addWidget(footer);

        again->setFocus(Qt::OtherFocusReason);
        QTimer::singleShot(0, again,
                           [again] { again->setFocus(Qt::OtherFocusReason); });
    }

    bool another_place() const noexcept { return another_; }

private:
    QString details_;
    bool another_ = false;
};

} // namespace

SupportBundleDialog::SupportBundleDialog(StudioBackend& backend,
                                         const EventLog& log, QWidget* parent)
    : QDialog(parent), backend_(backend), log_(log)
{
    available_ = backend_.caps().diagnostics;
    has_licence_ = bundle_has_licence(backend_);
    names_computers_ = bundle_licence_names_computers(backend_);
    opener_ = [](const QString& folder)
    { static_cast<void>(open_folder(folder)); };
    build();
    rebuild();
}

QFrame* SupportBundleDialog::add_row(QWidget* parent,
                                     const QString& object_name,
                                     bool bundle_selection::* member,
                                     const QString& title,
                                     const QString& description,
                                     const QString& tag, bool amber)
{
    auto* frame = new click_row(parent);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    if (amber)
        frame->setProperty("note", QStringLiteral("warn"));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(12);

    auto* check = new QCheckBox(frame);
    check->setObjectName(object_name);
    check->setAccessibleName(title);
    check->setAccessibleDescription(description);
    check->setChecked(selection_.*member);
    frame->set_check(check);
    layout->addWidget(check, 0, Qt::AlignTop);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    auto* heading = new QLabel(title, frame);
    QFont font = Theme::instance().font(text_style::body);
    font.setWeight(QFont::DemiBold);
    heading->setFont(font);
    heading->setWordWrap(true);
    column->addWidget(heading);
    auto* detail = new QLabel(description, frame);
    detail->setFont(Theme::instance().font(text_style::caption));
    detail->setWordWrap(true);
    if (!amber)
    {
        detail->setProperty("muted", true);
        polish(detail);
    }
    column->addWidget(detail);
    layout->addLayout(column, 1);

    if (!tag.isEmpty())
    {
        auto* tag_label = new QLabel(tag, frame);
        tag_label->setFont(Theme::instance().font(text_style::data));
        tag_label->setProperty("muted", true);
        polish(tag_label);
        layout->addWidget(tag_label, 0, Qt::AlignTop);
    }
    if (amber)
    {
        // The amber row is never colour alone: it carries these words too.
        auto* chip =
            new StatusChip(QCoreApplication::translate("SupportBundleDialog",
                                                       "Off by default"),
                           StatusChip::kind::waiting, frame);
        chip->set_small(true);
        chip->setAccessibleName(chip->text());
        layout->addWidget(chip, 0, Qt::AlignTop);
    }
    polish(frame);

    rows_.push_back({check, member});
    return frame;
}

void SupportBundleDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.bundle_dialog.build");
    VSS_PROFILE_MESSAGE("gui.dialog.support_bundle.opened");
    const QString title = QCoreApplication::translate(
        "SupportBundleDialog", "Create a support bundle");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    const int width =
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.wide"));
    resize(width, 720);
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
            "SupportBundleDialog",
            "Gathers what a helper needs to understand your setup. Nothing is "
            "sent anywhere. You choose where to save it."),
        header);
    outer->addWidget(header);

    // The two columns scroll together inside the dialog, so they stay usable
    // at 200 percent text.
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFocusPolicy(Qt::NoFocus);
    auto* body = new QWidget(scroll);
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(24, 4, 24, 16);
    body_layout->setSpacing(20);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // -- Left: the checklist -------------------------------------------------
    auto* left = new QWidget(body);
    left->setMinimumWidth(330);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(12);

    // Builds without diagnostics support get a banner here and disabled
    // controls further down.
    if (!available_)
    {
        left_layout->addWidget(new NoteBanner(
            NoteBanner::kind::warn,
            QCoreApplication::translate(
                "SupportBundleDialog",
                "Creating a support bundle is not available in this build."),
            left));
    }

    left_layout->addWidget(caption_label(
        QCoreApplication::translate("SupportBundleDialog", "Include"), left,
        true));

    auto* list = new QFrame(left);
    list->setProperty("card", true);
    polish(list);
    auto* list_layout = new QVBoxLayout(list);
    list_layout->setContentsMargins(0, 0, 0, 0);
    list_layout->setSpacing(0);

    list_layout->addWidget(add_row(
        list, QStringLiteral("bundleEnvironment"),
        &bundle_selection::environment,
        QCoreApplication::translate("SupportBundleDialog",
                                    "Version and environment"),
        QCoreApplication::translate(
            "SupportBundleDialog",
            "Build, compiler, Qt version, operating system, architecture and "
            "edition."),
        QStringLiteral("environment.txt"), false));
    list_layout->addWidget(add_row(
        list, QStringLiteral("bundleDriver"), &bundle_selection::driver,
        QCoreApplication::translate("SupportBundleDialog", "Driver state"),
        QCoreApplication::translate(
            "SupportBundleDialog",
            "Whether the Studio driver and com0com are present, their "
            "versions, how the driver is signed, the helper service's state "
            "and the results of the setup check."),
        QStringLiteral("driver.txt"), false));
    list_layout->addWidget(
        add_row(list, QStringLiteral("bundleSetup"), &bundle_selection::setup,
                QCoreApplication::translate("SupportBundleDialog", "Setup"),
                QCoreApplication::translate(
                    "SupportBundleDialog",
                    "Ports, links and their settings, as a topology file."),
                QStringLiteral("setup.json"), false));
    list_layout->addWidget(
        add_row(list, QStringLiteral("bundleLog"), &bundle_selection::event_log,
                QCoreApplication::translate("SupportBundleDialog", "Event log"),
                QCoreApplication::translate(
                    "SupportBundleDialog",
                    "State changes and errors since the setup was opened."),
                QStringLiteral("log.txt"), false));
    list_layout->addWidget(add_row(
        list, QStringLiteral("bundleCounters"), &bundle_selection::counters,
        QCoreApplication::translate("SupportBundleDialog", "Port counters"),
        QCoreApplication::translate(
            "SupportBundleDialog",
            "State, bytes in and out, and bytes dropped for each port."),
        QStringLiteral("counters.csv"), false));
    list_layout->addWidget(add_row(
        list, QStringLiteral("bundleSerialPorts"),
        &bundle_selection::serial_ports,
        QCoreApplication::translate("SupportBundleDialog",
                                    "Serial ports on this computer"),
        QCoreApplication::translate(
            "SupportBundleDialog",
            "One line for each serial port: name, description, vendor and "
            "product ids, and driver."),
        QStringLiteral("serial-ports.txt"), false));
    if (has_licence_)
    {
        list_layout->addWidget(add_row(
            list, QStringLiteral("bundleLicence"), &bundle_selection::licence,
            QCoreApplication::translate("SupportBundleDialog", "Licence"),
            QCoreApplication::translate(
                "SupportBundleDialog",
                "Licence number, edition, term and state. Not the licence "
                "file."),
            QStringLiteral("licence.txt"), false));
        list_layout->addWidget(add_row(
            list, QStringLiteral("bundleLicenseeName"),
            &bundle_selection::licensee_name,
            QCoreApplication::translate("SupportBundleDialog", "Licensee name"),
            QCoreApplication::translate(
                "SupportBundleDialog",
                "The name your licence was issued to. Turn this on if you are "
                "writing to support about the licence itself."),
            QStringLiteral("licence.txt"), true));
    }
    if (names_computers_)
    {
        list_layout->addWidget(
            add_row(list, QStringLiteral("bundleComputerName"),
                    &bundle_selection::computer_name,
                    QCoreApplication::translate("SupportBundleDialog",
                                                "This computer's name"),
                    QCoreApplication::translate("SupportBundleDialog",
                                                "Needed when a licence names "
                                                "computers."),
                    QStringLiteral("licence.txt"), true));
    }
    left_layout->addWidget(list);

    // What is always left out, as a muted block.
    auto* left_out = new QFrame(left);
    left_out->setProperty("group", true);
    polish(left_out);
    auto* left_out_layout = new QVBoxLayout(left_out);
    left_out_layout->setContentsMargins(12, 10, 12, 10);
    left_out_layout->setSpacing(4);
    left_out_layout->addWidget(caption_label(
        QCoreApplication::translate("SupportBundleDialog", "Always left out"),
        left_out, true));
    auto* left_out_text = caption_label(
        QCoreApplication::translate(
            "SupportBundleDialog",
            "The bytes your ports sent and received are never included. "
            "Passwords, tokens and keys are replaced with [redacted]. The "
            "path of your home folder is shortened to ~. The name of this "
            "computer and your user name are not included. Serial numbers of "
            "devices are not included. The licence file itself is never "
            "included."),
        left_out, false);
    left_out_text->setProperty("muted", true);
    polish(left_out_text);
    left_out_layout->addWidget(left_out_text);
    left_layout->addWidget(left_out);

    left_layout->addWidget(new NoteBanner(
        NoteBanner::kind::info,
        QCoreApplication::translate(
            "SupportBundleDialog",
            "Nothing leaves this computer until you share the file yourself."),
        left));
    left_layout->addStretch(1);
    body_layout->addWidget(left, 10);

    // -- Right: the preview --------------------------------------------------
    auto* right = new QWidget(body);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(8);
    auto* preview_heading = new QLabel(
        QCoreApplication::translate("SupportBundleDialog", "Preview"), right);
    preview_heading->setFont(Theme::instance().font(text_style::heading));
    right_layout->addWidget(preview_heading);

    list_ = new QListWidget(right);
    list_->setObjectName(QStringLiteral("bundleFiles"));
    list_->setAccessibleName(QCoreApplication::translate(
        "SupportBundleDialog", "Files in the bundle"));
    list_->setItemDelegate(new file_row_delegate(list_));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setMinimumHeight(Theme::instance().row_height() * 4 + 4);
    list_->setMaximumHeight(Theme::instance().row_height() * 6 + 4);
    right_layout->addWidget(list_);

    auto* exact =
        caption_label(QCoreApplication::translate(
                          "SupportBundleDialog",
                          "This is exactly what the file will contain."),
                      right, false);
    exact->setProperty("muted", true);
    polish(exact);
    right_layout->addWidget(exact);

    preview_ = new QPlainTextEdit(right);
    preview_->setObjectName(QStringLiteral("bundlePreview"));
    preview_->setReadOnly(true);
    preview_->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview_->setFont(Theme::instance().font(text_style::data));
    preview_->setMinimumHeight(220);
    right_layout->addWidget(preview_, 1);

    shortened_ =
        caption_label(QCoreApplication::translate(
                          "SupportBundleDialog",
                          "Preview shortened. The saved file is complete."),
                      right, false);
    shortened_->setObjectName(QStringLiteral("bundleShortened"));
    shortened_->setProperty("muted", true);
    polish(shortened_);
    shortened_->hide();
    right_layout->addWidget(shortened_);
    body_layout->addWidget(right, 11);

    connect(list_, &QListWidget::currentRowChanged, this,
            &SupportBundleDialog::show_file);

    // -- Footer --------------------------------------------------------------
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* file_name = new QLabel(bundle_default_name(), footer);
    file_name->setObjectName(QStringLiteral("bundleName"));
    file_name->setFont(Theme::instance().font(text_style::data));
    file_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer_layout->insertWidget(0, file_name, 0, Qt::AlignVCenter);
    count_ = new QLabel(footer);
    count_->setObjectName(QStringLiteral("bundleCount"));
    count_->setFont(Theme::instance().font(text_style::caption));
    count_->setProperty("muted", true);
    polish(count_);
    footer_layout->insertWidget(1, count_, 0, Qt::AlignVCenter);

    copy_ =
        make_dialog_button(QCoreApplication::translate("SupportBundleDialog",
                                                       "Copy environment only"),
                           "quiet", footer);
    copy_->setObjectName(QStringLiteral("bundleCopyEnvironment"));
    copy_->setAutoDefault(false);
    connect(copy_, &QPushButton::clicked, this,
            &SupportBundleDialog::copy_environment);
    footer_layout->addWidget(copy_);
    cancel_ = make_dialog_button(
        QCoreApplication::translate("SupportBundleDialog", "Cancel"), nullptr,
        footer);
    cancel_->setObjectName(QStringLiteral("bundleCancel"));
    cancel_->setAutoDefault(false);
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(cancel_);
    save_ = make_dialog_button(
        QCoreApplication::translate("SupportBundleDialog", "Save bundle"),
        "primary", footer);
    save_->setObjectName(QStringLiteral("saveBundle"));
    save_->setDefault(true);
    save_->setEnabled(available_);
    if (!available_)
        save_->setToolTip(QCoreApplication::translate(
            "SupportBundleDialog", "Not available in this build"));
    connect(save_, &QPushButton::clicked, this, &SupportBundleDialog::save);
    footer_layout->addWidget(save_);
    outer->addWidget(footer);

    // Each toggle writes its member of the selection and rebuilds the bundle.
    // The boxes were set before this point, so the first state does not fire.
    for (const row& entry : rows_)
    {
        QCheckBox* box = entry.check;
        bool bundle_selection::* member = entry.member;
        connect(box, &QCheckBox::toggled, this,
                [this, member](bool on)
                {
                    selection_.*member = on;
                    update_amber_rows();
                    rebuild();
                });
        if (!available_)
        {
            box->setEnabled(false);
            box->setToolTip(QCoreApplication::translate(
                "SupportBundleDialog", "Not available in this build"));
        }
    }
    update_amber_rows();

    // Keyboard order: the checklist, the list of files, the preview, then the
    // buttons.
    QWidget* previous = nullptr;
    const auto chain = [&previous](QWidget* next)
    {
        if (previous != nullptr)
            setTabOrder(previous, next);
        previous = next;
    };
    for (const row& entry : rows_)
        chain(entry.check);
    chain(list_);
    chain(preview_);
    chain(copy_);
    chain(cancel_);
    chain(save_);

    // The checklist takes the first focus, now and again once the event loop
    // runs, in case showing the dialog moves focus elsewhere first. Without
    // diagnostics support nothing can be ticked, so Cancel takes it.
    QWidget* first = available_ && !rows_.isEmpty()
                         ? static_cast<QWidget*>(rows_.first().check)
                         : static_cast<QWidget*>(cancel_);
    first->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, first,
                       [first] { first->setFocus(Qt::OtherFocusReason); });
}

void SupportBundleDialog::update_amber_rows()
{
    // The two amber rows add to licence.txt, so they do nothing while the
    // Licence row is off. They are greyed then, and keep their state.
    if (!available_)
        return;
    for (const row& entry : rows_)
    {
        const bool amber = entry.member == &bundle_selection::licensee_name ||
                           entry.member == &bundle_selection::computer_name;
        if (amber)
            entry.check->setEnabled(selection_.licence);
    }
}

QVector<QCheckBox*> SupportBundleDialog::checks() const
{
    QVector<QCheckBox*> all;
    for (const row& entry : rows_)
        all.push_back(entry.check);
    return all;
}

QCheckBox* SupportBundleDialog::check(const QString& object_name) const
{
    for (const row& entry : rows_)
    {
        if (entry.check->objectName() == object_name)
            return entry.check;
    }
    return nullptr;
}

bundle_selection SupportBundleDialog::selection() const
{
    return selection_;
}

int SupportBundleDialog::selected_file_count() const
{
    return static_cast<int>(files_.size());
}

QString SupportBundleDialog::bundle_name() const
{
    return bundle_default_name();
}

QString SupportBundleDialog::suggested_path() const
{
    return QDir(QDir::homePath()).filePath(bundle_name());
}

void SupportBundleDialog::rebuild()
{
    VSS_PROFILE_ZONE("gui.support.bundle_dialog.rebuild");
    // The one place the contents are made: the preview shows these bytes and
    // `write_bundle` writes them.
    if (available_)
        files_ = build_bundle(backend_, log_, selection_).files();
    else
        files_.clear();
    refresh_list();
    update_counts();
}

void SupportBundleDialog::refresh_list()
{
    QString keep;
    if (const QListWidgetItem* item = list_->currentItem())
        keep = item->data(Qt::DisplayRole).toString();

    // Changing the rows must not preview each one in turn.
    const QSignalBlocker blocker(list_);
    list_->clear();
    int select = 0;
    for (qsizetype index = 0; index < files_.size(); ++index)
    {
        const bundle_file& file = files_.at(index);
        auto* item = new QListWidgetItem(file.name, list_);
        const QString size = size_text(file.data.size());
        item->setData(size_role, size);
        item->setData(Qt::AccessibleTextRole,
                      QStringLiteral("%1, %2").arg(file.name, size));
        if (file.name == keep)
            select = static_cast<int>(index);
    }
    if (!files_.isEmpty())
        list_->setCurrentRow(select);
    show_file(list_->currentRow());
}

void SupportBundleDialog::show_file(int index)
{
    if (index < 0 || index >= files_.size())
    {
        preview_->clear();
        preview_->setAccessibleName(
            QCoreApplication::translate("SupportBundleDialog", "Preview"));
        shortened_->hide();
        return;
    }
    const bundle_file& file = files_.at(index);
    QByteArray shown = file.data;
    const bool cut = shown.size() > bundle_preview_limit;
    if (cut)
    {
        // Stop at the start of a character, not in the middle of one.
        qsizetype end = bundle_preview_limit;
        while (end > 0 &&
               (static_cast<unsigned char>(shown.at(end)) & 0xC0) == 0x80)
            --end;
        shown.truncate(end);
    }
    preview_->setPlainText(QString::fromUtf8(shown));
    preview_->moveCursor(QTextCursor::Start);
    preview_->setAccessibleName(
        QCoreApplication::translate("SupportBundleDialog", "Preview of %1")
            .arg(file.name));
    shortened_->setVisible(cut);
}

void SupportBundleDialog::update_counts()
{
    VSS_PROFILE_ZONE("gui.support.bundle_dialog.update_counts");
    if (count_ == nullptr)
        return;
    count_->setText(count_text(selected_file_count()));
    count_->setAccessibleName(count_->text());
}

bool SupportBundleDialog::write_bundle(const QString& path, QString* error)
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone, "gui.support.bundle_dialog.write");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, path.toStdString());
    // The files the preview shows, so the zip cannot differ from what the
    // person looked at.
    StoredZipWriter writer;
    for (const bundle_file& file : files_)
    {
        if (!writer.add_file(file.name, file.data))
        {
            if (error != nullptr)
                *error = QCoreApplication::translate(
                    "SupportBundleDialog", "The bundle has too many files.");
            return false;
        }
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly))
    {
        if (error != nullptr)
            *error = output.errorString();
        return false;
    }
    const QByteArray archive = writer.data();
    if (output.write(archive) != archive.size())
    {
        if (error != nullptr)
            *error = output.errorString();
        output.cancelWriting();
        return false;
    }
    if (!output.commit())
    {
        if (error != nullptr)
            *error = output.errorString();
        return false;
    }
    return true;
}

QString SupportBundleDialog::pick_path()
{
    if (picker_)
        return picker_(suggested_path());
    return QFileDialog::getSaveFileName(
        this,
        QCoreApplication::translate("SupportBundleDialog",
                                    "Save support bundle"),
        suggested_path(),
        QCoreApplication::translate("SupportBundleDialog",
                                    "ZIP archives (*.zip)"));
}

bool SupportBundleDialog::show_save_failure(const QString& details)
{
    save_failed_dialog dialog(details, this);
    dialog.exec();
    return dialog.another_place();
}

void SupportBundleDialog::saved(const QString& path)
{
    const QFileInfo info(path);
    const QString folder = info.absolutePath();
    // The toast belongs to the window behind this dialog, which closes next.
    QWidget* anchor = parentWidget() != nullptr ? parentWidget() : this;
    folder_opener opener = opener_;
    ToastManager::instance().show(
        anchor,
        QCoreApplication::translate("SupportBundleDialog", "Saved %1.")
            .arg(info.fileName()),
        QCoreApplication::translate("SupportBundleDialog", "Show in folder"),
        [opener, folder]
        {
            if (opener)
                opener(folder);
        });
    accept();
}

void SupportBundleDialog::save()
{
    VSS_PROFILE_ZONE("gui.support.bundle_dialog.save");
    if (!available_)
        return;
    for (;;)
    {
        QString path = pick_path();
        if (path.isEmpty())
            return;
        if (!path.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive))
            path += QStringLiteral(".zip");

        QString error;
        if (write_bundle(path, &error))
        {
            saved(path);
            return;
        }
        if (!show_save_failure(error))
            return;
    }
}

void SupportBundleDialog::copy_environment()
{
    // The same text the environment file holds, cleaned in the same way.
    bundle_selection only;
    only.driver = false;
    only.setup = false;
    only.event_log = false;
    only.counters = false;
    only.serial_ports = false;
    only.licence = false;
    const bundle_contents contents = build_bundle(backend_, log_, only);
    QString text;
    if (!contents.sections.isEmpty() &&
        !contents.sections.first().files.isEmpty())
        text = QString::fromUtf8(contents.sections.first().files.first().data);
    QGuiApplication::clipboard()->setText(text);
    ToastManager::instance().show(
        this, QCoreApplication::translate("SupportBundleDialog",
                                          "Environment copied."));
}

} // namespace vss::gui::support
