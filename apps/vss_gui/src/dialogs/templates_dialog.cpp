//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/templates_dialog.hpp"

#include "dialogs/common.hpp"
#include "dialogs/recent_files.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

QTableWidgetItem* text_item(const QString& text, text_style style,
                            colour_role colour, bool bold = false)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    QFont font = Theme::instance().font(style);
    if (bold)
        font.setWeight(QFont::DemiBold);
    item->setFont(font);
    item->setForeground(Theme::instance().colour(colour));
    return item;
}

QWidget* aligned(QWidget* widget, Qt::Alignment alignment, QWidget* parent)
{
    auto* host = new QWidget(parent);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(0);
    if (alignment & Qt::AlignRight)
        layout->addStretch(1);
    layout->addWidget(widget);
    if (!(alignment & Qt::AlignRight))
        layout->addStretch(1);
    return host;
}

} // namespace

TemplatesDialog::TemplatesDialog(StudioBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend), templates_(bundled_templates())
{
    build();
    select_first_template();
    refresh_recent();
}

void TemplatesDialog::build()
{
    VSS_PROFILE_ZONE("gui.dialogs.templates.build");
    VSS_PROFILE_MESSAGE("gui.dialog.templates.opened");
    prepare_dialog(this, tr("Setups"), 920, 700);
    setMinimumWidth(760);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 0);
    outer->setSpacing(12);

    auto* header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(12);
    auto* titles = new QVBoxLayout();
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(4);
    titles->addWidget(make_dialog_title(tr("Setups"), this));
    titles->addWidget(make_dialog_subtitle(
        tr("Start from a ready-made setup, or open one you saved."), this));
    header->addLayout(titles, 1);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("setupSearch"));
    search_->setPlaceholderText(tr("Search setups"));
    search_->setAccessibleName(tr("Search setups"));
    search_->setClearButtonEnabled(true);
    search_->setFixedWidth(260);
    header->addWidget(search_, 0, Qt::AlignTop);
    outer->addLayout(header);

    group_ = new QButtonGroup(this);
    group_->setExclusive(true);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("setupTabs"));
    tabs_->setAccessibleName(tr("Setups"));
    tabs_->setDocumentMode(true);
    tabs_->addTab(build_templates_page(), tr("Templates"));
    tabs_->addTab(build_recent_page(), tr("Recent"));
    tabs_->tabBar()->setAccessibleName(tr("Setups"));
    outer->addWidget(tabs_, 1);

    device_note_ = new NoteBanner(
        NoteBanner::kind::info,
        tr("This setup needs a real device. Attach it from Ports, Attach a "
           "real device, then use the template."),
        this);
    device_note_->hide();
    outer->addWidget(device_note_);

    status_ = new NoteBanner(NoteBanner::kind::ok, {}, this);
    status_->hide();
    outer->addWidget(status_);

    error_ = new InlineError(this);
    outer->addWidget(error_);

    build_footer(outer);

    connect(search_, &QLineEdit::textChanged, this,
            [this](const QString&) { apply_filter(); });
    connect(tabs_, &QTabWidget::currentChanged, this,
            [this](int)
            {
                update_primary();
                apply_filter();
            });
    connect(group_, &QButtonGroup::idToggled, this,
            [this](int, bool checked)
            {
                if (checked)
                    update_selection();
            });
}

QWidget* TemplatesDialog::build_templates_page()
{
    auto* page = new QWidget(this);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 18, 0, 0);
    outer->setSpacing(18);

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAccessibleName(tr("Templates"));
    auto* content = new QWidget(scroll);
    auto* grid = new QGridLayout(content);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);

    for (int index = 0; index < templates_.size(); ++index)
    {
        const bundled_template& entry = templates_.at(index);
        auto* card = new PickCard(entry.title, entry.description, content);
        card->setMinimumHeight(150);
        card->set_meta(meta_for(entry));
        card->radio()->setAccessibleName(entry.title);
        card->radio()->setAccessibleDescription(meta_for(entry));
        group_->addButton(card->radio(), index);
        cards_.push_back(card);
        connect(card, &PickCard::selected, this,
                [this] { update_selection(); });
        grid->addWidget(card, index / 2, index % 2);
    }
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(static_cast<int>((templates_.size() + 1) / 2), 1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    return page;
}

QWidget* TemplatesDialog::build_recent_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 18, 0, 0);
    layout->setSpacing(10);

    auto* heading = make_field_label(tr("Recent"), page);
    layout->addWidget(heading);

    recent_table_ = new QTableWidget(0, 4, page);
    recent_table_->setObjectName(QStringLiteral("recentSetups"));
    recent_table_->setAccessibleName(tr("Recent setups"));
    recent_table_->setHorizontalHeaderLabels(
        {tr("File"), tr("Summary"), tr("Last opened"), {}});
    recent_table_->verticalHeader()->hide();
    recent_table_->setSelectionMode(QAbstractItemView::NoSelection);
    recent_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recent_table_->setShowGrid(false);
    recent_table_->verticalHeader()->setDefaultSectionSize(40);
    recent_table_->horizontalHeader()->setSectionResizeMode(0,
                                                            QHeaderView::Fixed);
    recent_table_->horizontalHeader()->resizeSection(0, 220);
    recent_table_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    recent_table_->horizontalHeader()->setSectionResizeMode(2,
                                                            QHeaderView::Fixed);
    recent_table_->horizontalHeader()->resizeSection(2, 200);
    recent_table_->horizontalHeader()->setSectionResizeMode(3,
                                                            QHeaderView::Fixed);
    recent_table_->horizontalHeader()->resizeSection(3, 100);
    layout->addWidget(recent_table_, 1);

    no_recent_ = new QLabel(tr("No saved setups yet."), page);
    no_recent_->setFont(Theme::instance().font(text_style::body));
    no_recent_->setProperty("muted", true);
    layout->addWidget(no_recent_);
    layout->addStretch(1);
    return page;
}

void TemplatesDialog::build_footer(QVBoxLayout* outer)
{
    QHBoxLayout* layout = nullptr;
    QFrame* footer = make_dialog_footer(this, layout);

    auto* browse =
        make_dialog_action_button(tr("Browse for a file"), "quiet", footer);
    connect(browse, &QPushButton::clicked, this,
            [this]
            {
                const QString path = QFileDialog::getOpenFileName(
                    this, tr("Open setup"), {},
                    tr("Virtual Serial Studio setups (*.json);;All files "
                       "(*)"));
                if (!path.isEmpty())
                    static_cast<void>(open_file(path));
            });
    layout->insertWidget(0, browse);

    auto* save = make_dialog_action_button(tr("Save current setup as template"),
                                           "quiet", footer);
    connect(save, &QPushButton::clicked, this,
            [this]
            {
                QString path = QFileDialog::getSaveFileName(
                    this, tr("Save setup as template"),
                    QStringLiteral("template.json"),
                    tr("Virtual Serial Studio setups (*.json)"));
                if (path.isEmpty())
                    return;
                if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
                {
                    path += QStringLiteral(".json");
                }
                static_cast<void>(save_current_template(path));
            });
    layout->insertWidget(1, save);

    auto* cancel = make_dialog_action_button(tr("Cancel"), nullptr, footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancel);

    primary_ = make_dialog_action_button(tr("Use template"), "primary", footer);
    primary_->setDefault(true);
    connect(primary_, &QPushButton::clicked, this,
            [this] { static_cast<void>(use_current()); });
    layout->addWidget(primary_);

    outer->addWidget(footer);
}

QString TemplatesDialog::meta_for(const bundled_template& entry) const
{
    const QString ports =
        entry.ports == 1 ? tr("1 port") : tr("%1 ports").arg(entry.ports);
    const QString links =
        entry.links == 1 ? tr("1 link") : tr("%1 links").arg(entry.links);
    QString meta = tr("%1, %2, %3").arg(entry.level, ports, links);
    if (entry.requires_device)
        meta += tr(". Needs a real device.");
    return meta;
}

void TemplatesDialog::select_first_template()
{
    if (cards_.isEmpty())
        return;
    cards_.first()->set_selected(true);
    update_selection();
}

void TemplatesDialog::update_selection()
{
    clear_error();
    status_->hide();
    update_primary();
}

void TemplatesDialog::update_primary()
{
    const int index = current_template();
    const bool template_tab = tabs_ != nullptr && tabs_->currentIndex() == 0;
    const bool usable = template_tab && index >= 0 && card_visible(index) &&
                        !templates_.at(index).requires_device;
    primary_->setEnabled(usable);
    const bool needs_device =
        template_tab && index >= 0 && templates_.at(index).requires_device;
    device_note_->setVisible(needs_device);
    primary_->setToolTip(needs_device ? tr("Attach a device first")
                                      : QString());
}

bool TemplatesDialog::card_visible(int index) const
{
    return index >= 0 && index < cards_.size() && !cards_.at(index)->isHidden();
}

int TemplatesDialog::template_count() const
{
    return static_cast<int>(templates_.size());
}

void TemplatesDialog::select_template(int index)
{
    if (index < 0 || index >= cards_.size() || !card_visible(index))
        return;
    cards_.at(index)->set_selected(true);
    update_selection();
}

int TemplatesDialog::current_template() const
{
    if (group_ == nullptr)
        return -1;
    return group_->checkedId();
}

QString TemplatesDialog::template_title(int index) const
{
    if (index < 0 || index >= templates_.size())
        return {};
    return templates_.at(index).title;
}

QString TemplatesDialog::template_meta(int index) const
{
    if (index < 0 || index >= templates_.size())
        return {};
    return meta_for(templates_.at(index));
}

bool TemplatesDialog::requires_device_selected() const
{
    const int index = current_template();
    return index >= 0 && index < templates_.size() &&
           templates_.at(index).requires_device;
}

void TemplatesDialog::set_search(const QString& text)
{
    search_->setText(text);
}

QString TemplatesDialog::search() const
{
    return search_->text();
}

void TemplatesDialog::apply_filter()
{
    VSS_PROFILE_ZONE("gui.dialogs.templates.apply_filter");
    const QString needle = search_->text().trimmed();
    for (int index = 0; index < cards_.size(); ++index)
    {
        const bundled_template& entry = templates_.at(index);
        const bool match =
            needle.isEmpty() ||
            entry.title.contains(needle, Qt::CaseInsensitive) ||
            entry.description.contains(needle, Qt::CaseInsensitive) ||
            meta_for(entry).contains(needle, Qt::CaseInsensitive);
        cards_.at(index)->setVisible(match);
    }

    int visible_recent = 0;
    for (int row = 0; row < recent_table_->rowCount(); ++row)
    {
        QTableWidgetItem* name = recent_table_->item(row, 0);
        QTableWidgetItem* summary = recent_table_->item(row, 1);
        const bool match =
            needle.isEmpty() ||
            (name != nullptr &&
             name->text().contains(needle, Qt::CaseInsensitive)) ||
            (summary != nullptr &&
             summary->text().contains(needle, Qt::CaseInsensitive));
        recent_table_->setRowHidden(row, !match);
        if (match)
            ++visible_recent;
    }

    if (recent_table_->rowCount() == 0)
    {
        no_recent_->setText(tr("No saved setups yet."));
        no_recent_->show();
        recent_table_->hide();
    }
    else if (visible_recent == 0)
    {
        no_recent_->setText(tr("No saved setups match the search."));
        no_recent_->show();
        recent_table_->show();
    }
    else
    {
        no_recent_->hide();
        recent_table_->show();
    }
    update_primary();
}

void TemplatesDialog::refresh_recent()
{
    VSS_PROFILE_ZONE("gui.dialogs.templates.refresh_recent");
    const QVector<recent_entry> entries = recent_entries();
    recent_table_->setRowCount(static_cast<int>(entries.size()));
    for (int row = 0; row < entries.size(); ++row)
    {
        const recent_entry& entry = entries.at(row);
        const QString name = QFileInfo(entry.path).fileName();
        recent_table_->setItem(
            row, 0, text_item(name, text_style::data, colour_role::ink, true));
        recent_table_->setItem(
            row, 1,
            text_item(entry.summary, text_style::caption, colour_role::ink3));
        const QString opened =
            entry.last_opened.isValid()
                ? tr("Last opened %1")
                      .arg(QLocale().toString(entry.last_opened,
                                              QLocale::ShortFormat))
                : tr("Last opened %1").arg(tr("unknown"));
        recent_table_->setItem(
            row, 2, text_item(opened, text_style::caption, colour_role::ink3));

        auto* open =
            make_dialog_action_button(tr("Open"), "quiet", recent_table_);
        open->setProperty("size", QStringLiteral("small"));
        open->setAccessibleName(tr("Open %1").arg(name));
        polish(open);
        const QString path = entry.path;
        connect(open, &QPushButton::clicked, this,
                [this, path] { static_cast<void>(open_file(path)); });
        recent_table_->setCellWidget(
            row, 3, aligned(open, Qt::AlignRight, recent_table_));
    }
    apply_filter();
}

void TemplatesDialog::set_tab(int index)
{
    if (index < 0 || index > 1)
        return;
    tabs_->setCurrentIndex(index);
    update_primary();
    apply_filter();
}

int TemplatesDialog::tab() const
{
    return tabs_->currentIndex();
}

int TemplatesDialog::recent_count() const
{
    return recent_table_->rowCount();
}

QString TemplatesDialog::recent_name(int index) const
{
    if (index < 0 || index >= recent_table_->rowCount())
        return {};
    QTableWidgetItem* item = recent_table_->item(index, 0);
    return item == nullptr ? QString() : item->text();
}

bool TemplatesDialog::use_current()
{
    VSS_PROFILE_ZONE("gui.dialogs.templates.use");
    if (tabs_->currentIndex() != 0)
        return false;
    const int index = current_template();
    if (index < 0 || index >= templates_.size())
    {
        show_error(tr("Choose a setup"),
                   tr("Select a template, then choose Use template."));
        return false;
    }
    const bundled_template& entry = templates_.at(index);
    if (entry.requires_device)
    {
        show_error(
            tr("This setup needs a real device"),
            tr("Attach the device first: choose Ports, Attach a real device, "
               "then open this template again."));
        update_primary();
        return false;
    }
    clear_error();
    const auto document = load_bundled_template(entry.id);
    if (!document)
    {
        show_error(tr("Could not open %1").arg(entry.title),
                   tr("The bundled setup could not be loaded. This is a bug "
                      "in the build."),
                   error_text(document.error()));
        return false;
    }
    result_document_ = *document;
    result_title_ = entry.title;
    result_path_.clear();
    result_is_template_ = true;
    accept();
    return true;
}

bool TemplatesDialog::open_file(const QString& path)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.dialogs.templates.open_file");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    clear_error();
    const auto document = vss::config::load_file(path.toStdString());
    if (!document)
    {
        show_error(tr("Could not open %1").arg(QFileInfo(path).fileName()),
                   tr("The file is not a valid Virtual Serial Studio setup. "
                      "Choose another file."),
                   error_text(document.error()));
        return false;
    }
    result_document_ = *document;
    result_title_ = document->name.empty()
                        ? QFileInfo(path).completeBaseName()
                        : QString::fromStdString(document->name);
    result_path_ = path;
    result_is_template_ = false;
    accept();
    return true;
}

bool TemplatesDialog::save_current_template(const QString& path)
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.dialogs.templates.save");
    VSS_PROFILE_ZONE_TEXT(zone, path.toStdString());
    clear_error();
    const auto saved =
        vss::config::save_file(path.toStdString(), backend_.snapshot());
    if (!saved)
    {
        show_error(tr("Could not save this setup"),
                   tr("The file could not be written. Choose another place."),
                   error_text(saved.error()));
        return false;
    }
    const auto ports = backend_.ports();
    const auto links = backend_.links();
    record_recent(path,
                  tr("%1 ports, %2 links").arg(ports.size()).arg(links.size()));
    refresh_recent();
    status_->set_text(tr("Saved %1.").arg(QFileInfo(path).fileName()));
    status_->show();
    return true;
}

void TemplatesDialog::show_error(const QString& title, const QString& body,
                                 const QString& details)
{
    error_->show_error(title, body, details);
    status_->hide();
}

void TemplatesDialog::clear_error()
{
    if (error_ != nullptr)
        error_->clear();
}

QString TemplatesDialog::error_title() const
{
    return error_ == nullptr ? QString() : error_->title();
}

QString TemplatesDialog::error_body() const
{
    return error_ == nullptr ? QString() : error_->body();
}

bool TemplatesDialog::error_visible() const
{
    return error_ != nullptr && error_->is_showing();
}

QString TemplatesDialog::status_text() const
{
    return status_ == nullptr ? QString() : status_->text();
}

bool TemplatesDialog::status_visible() const
{
    return status_ != nullptr && !status_->isHidden();
}

} // namespace vss::gui
