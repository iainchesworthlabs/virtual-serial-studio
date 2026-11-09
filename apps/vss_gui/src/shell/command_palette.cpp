//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/command_palette.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

} // namespace

CommandPalette::CommandPalette(ActionRegistry& registry, LevelService& level,
                               QWidget* parent)
    : QDialog(parent), registry_(registry), level_(level)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    setAttribute(Qt::WA_StyledBackground, true);
    setAccessibleName(tr("Command palette"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(760, 520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* search_row = new QFrame(this);
    auto* search_layout = new QHBoxLayout(search_row);
    search_layout->setContentsMargins(20, 16, 20, 16);
    search_layout->setSpacing(12);
    auto* search_icon = new QLabel(search_row);
    search_icon->setPixmap(
        IconProvider::instance()
            .icon(QStringLiteral("search"), colour_role::ink3, 20)
            .pixmap(20, 20));
    search_layout->addWidget(search_icon);
    search_ = new QLineEdit(search_row);
    search_->setFont(Theme::instance().font(text_style::subtitle));
    search_->setPlaceholderText(tr("Search actions"));
    search_->setAccessibleName(tr("Search actions"));
    search_->setFrame(false);
    search_layout->addWidget(search_, 1);
    search_layout->addWidget(new KeyCap(tr("Esc"), search_row));
    root->addWidget(search_row);

    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet(QStringLiteral("color: %1")
                               .arg(Theme::instance()
                                        .colour(colour_role::line)
                                        .name(QColor::HexRgb)));
    root->addWidget(divider);

    results_ = new QListWidget(this);
    results_->setFrameShape(QFrame::NoFrame);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    results_->setAccessibleName(tr("Actions"));
    root->addWidget(results_, 1);

    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("dialogFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    footer->setStyleSheet(
        QStringLiteral(
            "QFrame#dialogFooter { background: %1; border-top: 1px solid %2; }")
            .arg(Theme::instance()
                     .colour(colour_role::sunken)
                     .name(QColor::HexRgb),
                 Theme::instance()
                     .colour(colour_role::line)
                     .name(QColor::HexRgb)));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(20, 10, 20, 10);
    footer_layout->setSpacing(12);
    footer_ = new QLabel(footer);
    footer_->setFont(Theme::instance().font(text_style::caption));
    footer_->setProperty("muted", true);
    footer_->setWordWrap(true);
    footer_layout->addWidget(footer_, 1);
    turn_on_ = new QPushButton(tr("Turn on Advanced mode"), footer);
    turn_on_->setProperty("role", QStringLiteral("primary"));
    polish(turn_on_);
    turn_on_->setVisible(false);
    connect(turn_on_, &QPushButton::clicked, this,
            [this]
            {
                level_.set_advanced(true);
                activate_current();
            });
    footer_layout->addWidget(turn_on_);
    root->addWidget(footer);

    connect(search_, &QLineEdit::textChanged, this, [this] { rebuild(); });
    connect(search_, &QLineEdit::returnPressed, this,
            [this] { activate_current(); });
    connect(results_, &QListWidget::itemSelectionChanged, this,
            [this] { update_footer(); });
    connect(results_, &QListWidget::itemActivated, this,
            [this] { activate_current(); });
    connect(&level_, &LevelService::changed, this, [this] { rebuild(); });

    rebuild();
    search_->setFocus();
}

void CommandPalette::set_query(const QString& query)
{
    search_->setText(query);
    rebuild();
}

void CommandPalette::activate_current()
{
    VSS_PROFILE_ZONE("gui.shell.palette.activate");
    QAction* action = selected_action();
    if (action == nullptr)
    {
        const auto items =
            results_->findItems(QString(), Qt::MatchContains | Qt::MatchWrap);
        for (QListWidgetItem* item : items)
        {
            if ((item->flags() & Qt::ItemIsSelectable) != 0)
            {
                results_->setCurrentItem(item);
                action = selected_action();
                break;
            }
        }
    }
    if (action == nullptr)
        return;

    const bool needs_advanced =
        !level_.advanced() &&
        action->property("actionLevel").toString() == QLatin1String("advanced");
    if (needs_advanced)
        level_.set_advanced(true);
    accept();
    action->trigger();
}

int CommandPalette::available_count() const
{
    return available_;
}

int CommandPalette::hidden_count() const
{
    return hidden_;
}

QAction* CommandPalette::selected_action() const
{
    QListWidgetItem* item = results_->currentItem();
    if (item == nullptr || (item->flags() & Qt::ItemIsSelectable) == 0)
        return nullptr;
    const QString id = item->data(Qt::UserRole).toString();
    return registry_.action(id);
}

void CommandPalette::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (parentWidget() != nullptr)
    {
        const QRect parent = parentWidget()->rect();
        move(parentWidget()->mapToGlobal(QPoint(
            (parent.width() - width()) / 2, (parent.height() - height()) / 2)));
    }
    search_->setFocus();
    search_->selectAll();
    for (int index = 0; index < results_->count(); ++index)
    {
        QListWidgetItem* item = results_->item(index);
        if ((item->flags() & Qt::ItemIsSelectable) != 0)
        {
            results_->setCurrentItem(item);
            break;
        }
    }
}

void CommandPalette::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        reject();
        return;
    }
    if (event->key() == Qt::Key_Down && search_->hasFocus())
    {
        results_->setFocus();
        if (results_->currentItem() == nullptr)
        {
            for (int index = 0; index < results_->count(); ++index)
            {
                if ((results_->item(index)->flags() & Qt::ItemIsSelectable) !=
                    0)
                {
                    results_->setCurrentRow(index);
                    break;
                }
            }
        }
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void CommandPalette::rebuild()
{
    VSS_PROFILE_ZONE("gui.shell.palette.rebuild");
    const QString query = search_->text();
    const QVector<QAction*> matches = registry_.search(query);
    QVector<QAction*> available;
    QVector<QAction*> hidden;
    for (QAction* action : matches)
    {
        if (!level_.advanced() && action->property("actionLevel").toString() ==
                                      QLatin1String("advanced"))
            hidden.push_back(action);
        else
            available.push_back(action);
    }
    available_ = static_cast<int>(available.size());
    hidden_ = static_cast<int>(hidden.size());

    results_->clear();
    add_group(tr("Available now"), available);
    if (!level_.advanced())
        add_group(tr("Needs Advanced mode"), hidden);

    for (int index = 0; index < results_->count(); ++index)
    {
        QListWidgetItem* item = results_->item(index);
        if ((item->flags() & Qt::ItemIsSelectable) != 0)
        {
            results_->setCurrentItem(item);
            break;
        }
    }
    update_footer();
}

void CommandPalette::update_footer()
{
    QAction* action = selected_action();
    const bool needs_advanced =
        action != nullptr && !level_.advanced() &&
        action->property("actionLevel").toString() == QLatin1String("advanced");
    turn_on_->setVisible(needs_advanced);
    if (needs_advanced)
    {
        footer_->setText(tr("Advanced mode adds panels and options to this "
                            "window. Nothing you have built changes."));
    }
    else
    {
        footer_->setText(tr("Enter runs the selected action. Escape closes."));
    }
}

void CommandPalette::add_group(const QString& title,
                               const QVector<QAction*>& actions)
{
    // An empty group, such as "Needs Advanced mode", is still shown with its
    // explanation, per the spec.
    auto* group_item = new QListWidgetItem(title.toUpper(), results_);
    group_item->setFlags(Qt::NoItemFlags);
    group_item->setForeground(Theme::instance().colour(colour_role::ink3));
    QFont group_font = Theme::instance().font(text_style::caption);
    group_font.setWeight(QFont::DemiBold);
    group_font.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
    group_item->setFont(group_font);
    group_item->setSizeHint(QSize(0, 30));

    for (QAction* action : actions)
        add_action(action);

    if (actions.isEmpty())
    {
        const QString mode = title == tr("Available now") ? tr("Simple mode")
                                                          : tr("Advanced mode");
        auto* empty = new QListWidgetItem(
            tr("Nothing in %1 matches \"%2\".").arg(mode, search_->text()),
            results_);
        empty->setFlags(Qt::NoItemFlags);
        empty->setForeground(Theme::instance().colour(colour_role::ink2));
        empty->setFont(Theme::instance().font(text_style::body));
        empty->setSizeHint(QSize(0, 32));
    }
}

QListWidgetItem* CommandPalette::add_action(QAction* action)
{
    auto* item = new QListWidgetItem(results_);
    item->setData(Qt::UserRole, action->data());
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setSizeHint(QSize(0, 54));

    auto* row = new QWidget(results_);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(14, 6, 14, 6);
    layout->setSpacing(12);

    auto* icon = new QLabel(row);
    icon->setFixedSize(32, 32);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(action->icon().pixmap(20, 20));
    icon->setStyleSheet(
        QStringLiteral("QLabel { background: %1; border: 1px solid %2; "
                       "border-radius: 6px; }")
            .arg(Theme::instance()
                     .colour(colour_role::sunken)
                     .name(QColor::HexRgb),
                 Theme::instance()
                     .colour(colour_role::line)
                     .name(QColor::HexRgb)));
    layout->addWidget(icon);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    auto* text = new QLabel(action->text(), row);
    text->setFont(semibold(Theme::instance().font(text_style::body)));
    column->addWidget(text);
    const QString shortcut =
        action->shortcut().toString(QKeySequence::NativeText);
    if (!shortcut.isEmpty())
    {
        auto* hint = new QLabel(shortcut, row);
        hint->setFont(Theme::instance().font(text_style::caption));
        hint->setProperty("muted", true);
        column->addWidget(hint);
    }
    layout->addLayout(column, 1);

    if (action->property("actionLevel").toString() ==
            QLatin1String("advanced") &&
        !level_.advanced())
    {
        layout->addWidget(new Badge(tr("ADVANCED"), row));
    }

    results_->setItemWidget(item, row);
    return item;
}

} // namespace vss::gui
