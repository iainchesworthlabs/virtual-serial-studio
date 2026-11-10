//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/connect_ports.hpp"

#include "dialogs/common.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/half_duplex_fields.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextDocumentFragment>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

QString chip_text(const port_view& port)
{
    if (port.open)
        return ConnectPortsDialog::tr("Open");
    return ConnectPortsDialog::tr("Closed");
}

/// One checkable port in the split or join checklist.
class PortCheckRow final : public QFrame
{
public:
    PortCheckRow(const port_view& port, QWidget* parent) : QFrame(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(10);

        check_ = new QCheckBox(this);
        const QString path = port.path.startsWith(QStringLiteral("loopback:"))
                                 ? ConnectPortsDialog::tr("Studio only")
                                 : port.path;
        check_->setAccessibleName(
            ConnectPortsDialog::tr("%1, %2").arg(port.name, path));
        layout->addWidget(check_, 0, Qt::AlignVCenter);

        auto* column = new QVBoxLayout();
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        name_ = new QLabel(port.name, this);
        name_->setFont(semibold(Theme::instance().font(text_style::body)));
        name_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        path_ = new QLabel(path, this);
        path_->setFont(Theme::instance().font(text_style::data));
        path_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        path_->setProperty("muted", true);
        column->addWidget(name_);
        column->addWidget(path_);
        column->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        layout->addLayout(column, 1);

        chip_ = new StatusChip(chip_text(port),
                               port.open ? StatusChip::kind::healthy
                                         : StatusChip::kind::closed,
                               this);
        layout->addWidget(chip_, 0, Qt::AlignVCenter);

        id_ = port.id;
        setAccessibleName(check_->accessibleName());
    }

    QCheckBox* check() const { return check_; }
    vss::endpoint_id id() const { return id_; }

    void set_reason(const QString& reason)
    {
        check_->setChecked(false);
        check_->setEnabled(false);
        check_->setAccessibleName(
            ConnectPortsDialog::tr("%1. %2").arg(name_->text(), reason));
        name_->setProperty("muted", true);
        polish(name_);
        path_->setText(reason);
        path_->setFont(Theme::instance().font(text_style::body));
        path_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        path_->setProperty("muted", true);
        chip_->hide();
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && check_->isEnabled() &&
            rect().contains(event->position().toPoint()))
        {
            check_->toggle();
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    QCheckBox* check_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* path_ = nullptr;
    StatusChip* chip_ = nullptr;
    vss::endpoint_id id_ = 0;
};

void clear_layout(QVBoxLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
}

} // namespace

ConnectPortsDialog::ConnectPortsDialog(StudioBackend& backend,
                                       vss::endpoint_id preselected,
                                       bool advanced, QWidget* parent)
    : QDialog(parent), backend_(backend), advanced_(advanced),
      preselected_(preselected)
{
    build();
}

void ConnectPortsDialog::build()
{
    VSS_PROFILE_ZONE("gui.dialogs.connect_ports.build");
    VSS_PROFILE_MESSAGE("gui.dialog.connect_ports.opened");
    prepare_dialog(this, tr("Connect ports"), 920, 700);
    setMinimumWidth(760);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(24, 24, 24, 0);
    outer->setSpacing(12);

    auto* header = new QVBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(4);
    header->addWidget(make_dialog_title(tr("Connect ports"), this));
    header->addWidget(
        make_dialog_subtitle(tr("Choose how the ports should be linked, then "
                                "which ports take part."),
                             this));
    outer->addLayout(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setAccessibleName(tr("Link options"));
    auto* body = new QWidget(scroll);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(14);

    auto* columns = new QHBoxLayout();
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(24);

    QWidget* modes = build_modes();
    modes->setFixedWidth(420);
    columns->addWidget(modes, 0, Qt::AlignTop);

    auto* right = new QVBoxLayout();
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(12);
    stack_ = new QStackedWidget(body);
    stack_->addWidget(build_pair_page());
    stack_->addWidget(build_split_page());
    stack_->addWidget(build_join_page());
    stack_->addWidget(build_bus_page());
    right->addWidget(stack_);
    columns->addLayout(right, 1);
    body_layout->addLayout(columns);

    result_ = new ResultSentence(body);
    body_layout->addWidget(result_);

    block_note_ = new NoteBanner(NoteBanner::kind::warn, {}, body);
    block_note_->hide();
    body_layout->addWidget(block_note_);

    error_ = new InlineError(body);
    body_layout->addWidget(error_);

    scroll->setWidget(body);
    outer->addWidget(scroll, 1);
    build_footer(outer);

    for (int index = 0; index < 6; ++index)
    {
        connect(cards_[index], &PickCard::selected, this,
                [this, index] { on_mode_selected(static_cast<mode>(index)); });
    }

    updating_ = true;
    cards_[static_cast<int>(mode::two_way)]->set_selected(true);
    updating_ = false;
    reload_ports();
    set_advanced(advanced_);

    QDialog::setTabOrder(cards_[0]->radio(), cards_[1]->radio());
    QDialog::setTabOrder(cards_[1]->radio(), cards_[2]->radio());
    QDialog::setTabOrder(cards_[2]->radio(), cards_[3]->radio());
    QDialog::setTabOrder(cards_[3]->radio(), cards_[4]->radio());
    QDialog::setTabOrder(cards_[4]->radio(), cards_[5]->radio());
    QDialog::setTabOrder(cards_[5]->radio(), from_combo_);
    QDialog::setTabOrder(from_combo_, to_combo_);
    QDialog::setTabOrder(to_combo_, split_source_combo_);
    QDialog::setTabOrder(split_source_combo_, join_target_combo_);
    QDialog::setTabOrder(join_target_combo_, primary_);

    cards_[0]->radio()->setFocus(Qt::OtherFocusReason);
}

QWidget* ConnectPortsDialog::build_modes()
{
    auto* modes = new QWidget(this);
    auto* layout = new QVBoxLayout(modes);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    modes->setAccessibleName(tr("Link mode"));

    card_group_ = new QButtonGroup(this);
    card_group_->setExclusive(true);
    add_mode_card(layout, mode::two_way, tr("Two-way"),
                  tr("Like a null-modem cable. Both ports see everything."),
                  QStringLiteral("A \u21c4 B"));
    add_mode_card(layout, mode::redirect, tr("One-way redirect"),
                  tr("Data flows from A to B only. B cannot reply."),
                  QStringLiteral("A \u2192 B"));
    add_mode_card(layout, mode::monitor, tr("Monitor"),
                  tr("Watch traffic without disturbing it. Never drives modem "
                     "lines."),
                  QStringLiteral("A \u21e2 B"));
    add_mode_card(layout, mode::split, tr("Split one port to many"),
                  tr("Several programs see the same device."),
                  QStringLiteral("A \u21c4 B, C"));
    add_mode_card(layout, mode::join, tr("Join many ports into one"),
                  tr("Several sources feed one target."),
                  QStringLiteral("A, B \u2192 C"));
    add_mode_card(layout, mode::bus, tr("Shared bus"),
                  tr("Several programs on one wire. Each hears all the "
                     "others."),
                  QStringLiteral("A \u21c4 B \u21c4 C"));
    layout->addStretch(1);
    return modes;
}

void ConnectPortsDialog::add_mode_card(QLayout* layout, mode value,
                                       const QString& title,
                                       const QString& description,
                                       const QString& glyph)
{
    auto* card = new PickCard(title, description, this);
    card->set_trailing(glyph);
    card->radio()->setAccessibleName(title);
    card->radio()->setAccessibleDescription(description);
    card_group_->addButton(card->radio(), static_cast<int>(value));
    cards_[static_cast<int>(value)] = card;
    layout->addWidget(card);
}

QWidget* ConnectPortsDialog::build_pair_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    from_combo_ = new QComboBox(page);
    from_combo_->setObjectName(QStringLiteral("connectFrom"));
    to_combo_ = new QComboBox(page);
    to_combo_->setObjectName(QStringLiteral("connectTo"));
    layout->addWidget(new FormRow(tr("From"), from_combo_, {}, page));
    layout->addWidget(new FormRow(tr("To"), to_combo_, {}, page));
    layout->addStretch(1);

    const auto changed = [this]
    {
        if (updating_)
            return;
        clear_error();
        update_ui();
    };
    connect(from_combo_, &QComboBox::currentIndexChanged, this, changed);
    connect(to_combo_, &QComboBox::currentIndexChanged, this, changed);
    return page;
}

QWidget* ConnectPortsDialog::build_split_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    split_source_combo_ = new QComboBox(page);
    split_source_combo_->setObjectName(QStringLiteral("splitSource"));
    layout->addWidget(
        new FormRow(tr("Port to split"), split_source_combo_, {}, page));

    layout->addWidget(
        make_field_label(tr("Send copies to, choose at least 2"), page));
    auto* card = new QFrame(page);
    card->setProperty("card", true);
    polish(card);
    split_layout_ = new QVBoxLayout(card);
    split_layout_->setContentsMargins(0, 0, 0, 0);
    split_layout_->setSpacing(0);
    split_card_ = card;
    layout->addWidget(card);

    count_ = new QLabel(page);
    count_->setFont(Theme::instance().font(text_style::caption));
    count_->setWordWrap(true);
    layout->addWidget(count_);
    layout->addStretch(1);

    connect(split_source_combo_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                if (updating_)
                    return;
                const vss::endpoint_id source = split_source_id();
                split_targets_.removeAll(source);
                clear_error();
                rebuild_split_list();
                update_ui();
            });
    return page;
}

QWidget* ConnectPortsDialog::build_join_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(
        make_field_label(tr("Ports that send, choose at least 2"), page));
    auto* card = new QFrame(page);
    card->setProperty("card", true);
    polish(card);
    join_layout_ = new QVBoxLayout(card);
    join_layout_->setContentsMargins(0, 0, 0, 0);
    join_layout_->setSpacing(0);
    join_card_ = card;
    layout->addWidget(card);

    join_count_ = new QLabel(page);
    join_count_->setFont(Theme::instance().font(text_style::caption));
    join_count_->setWordWrap(true);
    layout->addWidget(join_count_);

    join_target_combo_ = new QComboBox(page);
    join_target_combo_->setObjectName(QStringLiteral("joinTarget"));
    layout->addWidget(
        new FormRow(tr("Send everything to"), join_target_combo_, {}, page));

    auto* more = new QFrame(page);
    more->setProperty("dashed", true);
    polish(more);
    auto* more_layout = new QHBoxLayout(more);
    more_layout->setContentsMargins(12, 8, 12, 8);
    more_layout->setSpacing(8);
    auto* chevron = new QLabel(more);
    chevron->setPixmap(
        IconProvider::instance()
            .icon(QStringLiteral("chevron-down"), colour_role::ink2, 16)
            .pixmap(16, 16));
    more_layout->addWidget(chevron, 0, Qt::AlignVCenter);
    auto* more_title = new QLabel(tr("More options"), more);
    more_title->setFont(semibold(Theme::instance().font(text_style::body)));
    more_layout->addWidget(more_title);
    more_layout->addWidget(new Badge(tr("ADVANCED"), more));
    more_layout->addStretch(1);
    owner_check_ = new QCheckBox(tr("Choose one owner at a time"), more);
    owner_check_->setAccessibleName(tr("Choose one owner at a time"));
    more_layout->addWidget(owner_check_, 0, Qt::AlignVCenter);
    advanced_button_ =
        make_dialog_action_button(tr("Turn on Advanced mode"), "quiet", more);
    advanced_button_->setProperty("size", QStringLiteral("small"));
    polish(advanced_button_);
    more_layout->addWidget(advanced_button_);
    connect(advanced_button_, &QPushButton::clicked, this,
            [this]
            {
                set_advanced(true);
                Q_EMIT advanced_requested();
            });
    layout->addWidget(more);
    layout->addStretch(1);

    connect(join_target_combo_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                if (updating_)
                    return;
                const vss::endpoint_id target = join_target_id();
                join_sources_.removeAll(target);
                clear_error();
                rebuild_join_list();
                update_ui();
            });
    return page;
}

QWidget* ConnectPortsDialog::build_bus_page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    layout->addWidget(
        make_field_label(tr("Ports on the bus, choose at least 2"), page));
    auto* card = new QFrame(page);
    card->setProperty("card", true);
    polish(card);
    bus_layout_ = new QVBoxLayout(card);
    bus_layout_->setContentsMargins(0, 0, 0, 0);
    bus_layout_->setSpacing(0);
    bus_card_ = card;
    layout->addWidget(card);

    bus_count_ = new QLabel(page);
    bus_count_->setFont(Theme::instance().font(text_style::caption));
    bus_count_->setWordWrap(true);
    layout->addWidget(bus_count_);

    half_duplex_ = new HalfDuplexFields(page);
    half_duplex_->set_available(backend_.caps().half_duplex_bus);
    layout->addWidget(half_duplex_);
    layout->addStretch(1);
    return page;
}

void ConnectPortsDialog::build_footer(QVBoxLayout* outer)
{
    QHBoxLayout* layout = nullptr;
    QFrame* footer = make_dialog_footer(this, layout);

    auto* shortcut = new QLabel(tr("Shortcut: Ctrl+L"), footer);
    shortcut->setFont(Theme::instance().font(text_style::caption));
    shortcut->setProperty("muted", true);
    layout->insertWidget(0, shortcut);

    auto* cancel = make_dialog_action_button(tr("Cancel"), nullptr, footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(cancel);

    primary_ = make_dialog_action_button(tr("Connect"), "primary", footer);
    primary_->setDefault(true);
    connect(primary_, &QPushButton::clicked, this,
            [this] { static_cast<void>(submit()); });
    layout->addWidget(primary_);

    outer->addWidget(footer);
}

void ConnectPortsDialog::reload_ports()
{
    VSS_PROFILE_ZONE("gui.dialogs.connect_ports.reload_ports");
    ports_ = backend_.ports();
    const vss::endpoint_id old_from = combo_port_id(from_combo_);
    const vss::endpoint_id old_to = combo_port_id(to_combo_);
    const vss::endpoint_id old_source = combo_port_id(split_source_combo_);
    const vss::endpoint_id old_target = combo_port_id(join_target_combo_);

    updating_ = true;
    fill_port_combo(from_combo_, ports_);
    fill_port_combo(to_combo_, ports_);
    fill_port_combo(split_source_combo_, ports_);
    fill_port_combo(join_target_combo_, ports_);

    const auto first_other = [this](vss::endpoint_id avoid)
    {
        for (const port_view& port : ports_)
        {
            if (port.id != avoid)
                return port.id;
        }
        return vss::endpoint_id{0};
    };

    vss::endpoint_id chosen_from = old_from;
    vss::endpoint_id chosen_to = old_to;
    if (preselected_ != 0 && find_port(preselected_))
    {
        chosen_from = preselected_;
        chosen_to = first_other(preselected_);
    }
    if (!find_port(chosen_from))
        chosen_from = ports_.empty() ? 0 : ports_.front().id;
    if (!find_port(chosen_to) || chosen_to == chosen_from)
        chosen_to = first_other(chosen_from);
    select_combo_port(from_combo_, chosen_from);
    select_combo_port(to_combo_, chosen_to);

    vss::endpoint_id source = old_source;
    if (preselected_ != 0 && find_port(preselected_))
        source = preselected_;
    if (!find_port(source))
        source = ports_.empty() ? 0 : ports_.front().id;
    select_combo_port(split_source_combo_, source);

    vss::endpoint_id target = old_target;
    if (preselected_ != 0 && find_port(preselected_))
        target = preselected_;
    if (!find_port(target))
        target = ports_.empty() ? 0 : ports_.front().id;
    select_combo_port(join_target_combo_, target);
    preselected_ = 0;

    const auto keep_existing = [this](QVector<vss::endpoint_id>& ids)
    {
        QVector<vss::endpoint_id> kept;
        for (const vss::endpoint_id id : ids)
        {
            if (find_port(id))
                kept.push_back(id);
        }
        ids = kept;
    };
    keep_existing(split_targets_);
    keep_existing(join_sources_);
    keep_existing(bus_members_);
    split_targets_.removeAll(source);
    join_sources_.removeAll(target);

    if (split_targets_.isEmpty())
        choose_first_two(split_targets_, source);
    if (join_sources_.isEmpty())
        choose_first_two(join_sources_, target);
    if (bus_members_.isEmpty())
        choose_first_two(bus_members_, 0);

    rebuild_split_list();
    rebuild_join_list();
    rebuild_bus_list();
    updating_ = false;
    update_ui();
}

void ConnectPortsDialog::choose_first_two(QVector<vss::endpoint_id>& list,
                                          vss::endpoint_id avoid) const
{
    for (const port_view& port : ports_)
    {
        if (port.id != avoid)
            list.push_back(port.id);
        if (list.size() >= 2)
            break;
    }
}

void ConnectPortsDialog::rebuild_split_list()
{
    updating_ = true;
    rebuild_checklist(split_card_, split_layout_, true, split_targets_,
                      split_source_id(), tr("This is the port being split."));
    updating_ = false;
}

void ConnectPortsDialog::rebuild_bus_list()
{
    updating_ = true;
    rebuild_checklist(bus_card_, bus_layout_, false, bus_members_, 0, {});
    updating_ = false;
}

void ConnectPortsDialog::rebuild_join_list()
{
    updating_ = true;
    rebuild_checklist(join_card_, join_layout_, false, join_sources_,
                      join_target_id(),
                      tr("This is the port everything is sent to."));
    updating_ = false;
}

void ConnectPortsDialog::rebuild_checklist(QWidget* card, QVBoxLayout* layout,
                                           bool split,
                                           QVector<vss::endpoint_id>& list,
                                           vss::endpoint_id excluded,
                                           const QString& excluded_reason)
{
    VSS_PROFILE_ZONE("gui.dialogs.connect_ports.rebuild_checklist");
    Q_UNUSED(card);
    clear_layout(layout);
    for (const port_view& port : ports_)
    {
        if (!split && port.id == excluded)
            continue;

        auto* row = new PortCheckRow(port, layout->parentWidget());
        {
            const QSignalBlocker blocker(row->check());
            row->check()->setChecked(list.contains(port.id));
        }
        if (split && port.id == excluded)
            row->set_reason(excluded_reason);
        layout->addWidget(row);

        const vss::endpoint_id id = port.id;
        connect(row->check(), &QCheckBox::toggled, this,
                [this, id, &list](bool on)
                {
                    if (updating_)
                        return;
                    list.removeAll(id);
                    if (on)
                        list.push_back(id);
                    clear_error();
                    update_ui();
                });
    }
}

void ConnectPortsDialog::on_mode_selected(mode value)
{
    if (updating_)
        return;
    if ((value == mode::two_way && !two_way_allowed()) ||
        (value == mode::bus && !backend_.caps().shared_bus))
    {
        update_ui();
        return;
    }
    mode_ = value;
    if (mode_ == mode::split && split_targets_.isEmpty())
    {
        choose_first_two(split_targets_, split_source_id());
        rebuild_split_list();
    }
    else if (mode_ == mode::join && join_sources_.isEmpty())
    {
        choose_first_two(join_sources_, join_target_id());
        rebuild_join_list();
    }
    else if (mode_ == mode::bus && bus_members_.isEmpty())
    {
        choose_first_two(bus_members_, 0);
        rebuild_bus_list();
    }
    clear_error();
    update_ui();
}

void ConnectPortsDialog::update_ui()
{
    VSS_PROFILE_ZONE("gui.dialogs.connect_ports.update_ui");
    if (stack_ != nullptr)
    {
        int index = 0;
        if (mode_ == mode::split)
            index = 1;
        else if (mode_ == mode::join)
            index = 2;
        else if (mode_ == mode::bus)
            index = 3;
        stack_->setCurrentIndex(index);
    }

    const bool allowed = two_way_allowed();
    cards_[static_cast<int>(mode::two_way)]->radio()->setEnabled(allowed);
    cards_[static_cast<int>(mode::two_way)]->setToolTip(
        allowed ? QString()
                : tr("Studio-only ports echo what they receive, so a two-way "
                     "link between them would repeat data. Use one-way, or "
                     "link a real or paired port."));

    const bool bus_available = backend_.caps().shared_bus;
    cards_[static_cast<int>(mode::bus)]->radio()->setEnabled(bus_available);
    cards_[static_cast<int>(mode::bus)]->setToolTip(
        bus_available ? QString() : tr("Not available in this build"));
    if (half_duplex_ != nullptr)
        half_duplex_->set_available(backend_.caps().half_duplex_bus);

    const QString block = block_reason();
    if (!block.isEmpty())
    {
        block_note_->set_text(block);
        block_note_->show();
    }
    else
    {
        block_note_->hide();
    }

    update_count();
    update_result();
    primary_->setEnabled(primary_enabled());
}

void ConnectPortsDialog::update_count()
{
    const auto set_count = [](QLabel* label, int count, const QString& noun)
    {
        if (label == nullptr)
            return;
        const bool enough = count >= 2;
        label->setText(
            enough ? ConnectPortsDialog::tr(
                         "%1 ports chosen. That is enough for a %2.")
                         .arg(count)
                         .arg(noun)
                   : ConnectPortsDialog::tr("Choose at least 2 ports."));
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText,
                         Theme::instance().colour(enough ? colour_role::ok
                                                         : colour_role::bad));
        label->setPalette(palette);
        label->setAccessibleName(label->text());
    };
    set_count(count_, static_cast<int>(split_targets_.size()), tr("split"));
    set_count(join_count_, static_cast<int>(join_sources_.size()), tr("join"));
    set_count(bus_count_, static_cast<int>(bus_members_.size()), tr("bus"));
}

QString ConnectPortsDialog::single_name(vss::endpoint_id id) const
{
    const auto port = find_port(id);
    return port ? port->name : QString();
}

QString
ConnectPortsDialog::names_for(const QVector<vss::endpoint_id>& ids) const
{
    QStringList names;
    for (const port_view& port : ports_)
    {
        if (ids.contains(port.id))
            names << port.name;
    }
    return join_dialog_names(names);
}

void ConnectPortsDialog::update_result()
{
    VSS_PROFILE_ZONE("gui.dialogs.connect_ports.update_result");
    if (result_ == nullptr)
        return;

    // Resolve the names and checked ids for every mode up front; the switch
    // below only uses the ones its own mode needs.
    QString rich;
    const QString from = single_name(from_id());
    const QString to = single_name(to_id());
    const QString source = single_name(split_source_id());
    const QString target = single_name(join_target_id());
    const QVector<vss::endpoint_id> targets = checked_ids(mode::split);
    const QVector<vss::endpoint_id> sources = checked_ids(mode::join);
    const QVector<vss::endpoint_id> members = checked_ids(mode::bus);

    // Each mode describes the link in plain words, or asks for the missing
    // choices first.
    switch (mode_)
    {
    case mode::two_way:
        if (from.isEmpty() || to.isEmpty())
            rich = tr("Choose two ports to see what the link will do.");
        else
            rich = tr("Everything %1 sends arrives at %2, and the other way "
                      "round.")
                       .arg(bold_name(from), bold_name(to));
        break;
    case mode::redirect:
        if (from.isEmpty() || to.isEmpty())
            rich = tr("Choose two ports to see what the link will do.");
        else
            rich = tr("Everything %1 sends arrives at %2. %2 cannot reply.")
                       .arg(bold_name(from), bold_name(to));
        break;
    case mode::monitor:
        if (from.isEmpty() || to.isEmpty())
            rich = tr("Choose two ports to see what the link will do.");
        else
            rich = tr("%2 receives a read-only copy of what %1 sends. It "
                      "never drives modem lines.")
                       .arg(bold_name(from), bold_name(to));
        break;
    case mode::split:
        // A split needs a source and at least two targets before it can be
        // described.
        if (source.isEmpty() || targets.size() < 2)
        {
            rich = source.isEmpty()
                       ? tr("Choose a port to split.")
                       : tr("Everything %1 receives is copied to the ports "
                            "you choose. Whatever they write goes back to %1.")
                             .arg(bold_name(source));
        }
        else
        {
            QStringList target_rich;
            for (const vss::endpoint_id id : targets)
                target_rich << bold_name(single_name(id));
            rich = tr("Everything %1 receives is copied to %2. Whatever %3 of "
                      "them writes goes back to %1.")
                       .arg(bold_name(source), join_dialog_names(target_rich),
                            targets.size() == 2 ? tr("either") : tr("any"));
        }
        break;
    case mode::join:
        // A join needs a target and at least two sources.
        if (target.isEmpty() || sources.size() < 2)
        {
            rich = target.isEmpty()
                       ? tr("Choose a port to send everything to.")
                       : tr("Everything the ports you choose write goes to %1. "
                            "Their bytes are mixed in the order they arrive.")
                             .arg(bold_name(target));
        }
        else
        {
            QStringList source_rich;
            for (const vss::endpoint_id id : sources)
                source_rich << bold_name(single_name(id));
            rich =
                tr("Everything %1 write goes to %2. Their bytes are mixed in "
                   "the order they arrive. %2 does not send anything back to "
                   "them.")
                    .arg(join_dialog_names(source_rich), bold_name(target));
        }
        break;
    case mode::bus:
        // A bus needs at least two members.
        if (members.size() < 2)
        {
            rich = tr("Choose the ports to put on the bus.");
        }
        else
        {
            QStringList member_rich;
            for (const vss::endpoint_id id : members)
                member_rich << bold_name(single_name(id));
            rich = tr("Each of %1 hears what the others write, and none hears "
                      "itself. Their bytes are mixed in the order they "
                      "arrive.")
                       .arg(join_dialog_names(member_rich));
        }
        break;
    }

    // Pass the plain-text form alongside the rich text, with the markup
    // stripped.
    result_->set_result_text(
        rich, QTextDocumentFragment::fromHtml(rich).toPlainText());
}

QString ConnectPortsDialog::count_text() const
{
    if (mode_ == mode::split && count_ != nullptr)
        return count_->text();
    if (mode_ == mode::join && join_count_ != nullptr)
        return join_count_->text();
    if (mode_ == mode::bus && bus_count_ != nullptr)
        return bus_count_->text();
    return {};
}

QString ConnectPortsDialog::result_text() const
{
    return result_ == nullptr ? QString() : result_->text();
}

QString ConnectPortsDialog::accessible_result_text() const
{
    return result_ == nullptr ? QString() : result_->accessibleName();
}

bool ConnectPortsDialog::two_way_allowed() const
{
    const auto first = find_port(from_id());
    const auto second = find_port(to_id());
    if (!first || !second)
        return true;
    return !(first->studio_only && second->studio_only);
}

QString ConnectPortsDialog::block_reason() const
{
    if (!backend_.caps().links)
        return tr("Links are not available in this build.");
    if (mode_ == mode::bus)
    {
        if (!backend_.caps().shared_bus)
            return tr("Not available in this build");
        if (bus_members_.size() > static_cast<qsizetype>(vss::max_bus_members))
            return tr("A bus takes at most %1 ports.")
                .arg(vss::max_bus_members);
        return {};
    }
    if (mode_ != mode::two_way && mode_ != mode::redirect &&
        mode_ != mode::monitor)
    {
        return {};
    }
    const vss::endpoint_id from = from_id();
    const vss::endpoint_id to = to_id();
    if (from == 0 || to == 0)
        return tr("Choose two ports.");
    if (from == to)
        return tr("A port cannot link to itself. Choose a different port.");
    if (mode_ == mode::two_way)
    {
        const auto first = find_port(from);
        const auto second = find_port(to);
        if (first && second && first->studio_only && second->studio_only)
        {
            return tr("Studio-only ports echo what they receive, so a two-way "
                      "link between them would repeat data. Use one-way, or "
                      "link a real or paired port.");
        }
    }
    return {};
}

bool ConnectPortsDialog::primary_enabled() const
{
    if (!backend_.caps().links)
        return false;
    switch (mode_)
    {
    case mode::two_way:
    case mode::redirect:
    case mode::monitor:
        return block_reason().isEmpty();
    case mode::split:
        return block_reason().isEmpty() && checked_ids(mode::split).size() >= 2;
    case mode::join:
        return block_reason().isEmpty() && checked_ids(mode::join).size() >= 2;
    case mode::bus:
        return block_reason().isEmpty() && checked_ids(mode::bus).size() >= 2;
    }
    return false;
}

bool ConnectPortsDialog::set_mode(mode value)
{
    if (value == mode::two_way && !two_way_allowed())
        return false;
    if (value == mode::bus && !backend_.caps().shared_bus)
        return false;
    mode_ = value;
    updating_ = true;
    if (cards_[static_cast<int>(value)] != nullptr)
        cards_[static_cast<int>(value)]->set_selected(true);
    updating_ = false;
    if (value == mode::split && split_targets_.isEmpty())
        choose_first_two(split_targets_, split_source_id());
    else if (value == mode::join && join_sources_.isEmpty())
        choose_first_two(join_sources_, join_target_id());
    else if (value == mode::bus && bus_members_.isEmpty())
        choose_first_two(bus_members_, 0);
    rebuild_split_list();
    rebuild_join_list();
    rebuild_bus_list();
    clear_error();
    update_ui();
    return true;
}

void ConnectPortsDialog::set_advanced(bool advanced)
{
    advanced_ = advanced;
    if (owner_check_ != nullptr)
    {
        owner_check_->setEnabled(advanced_);
        owner_check_->setToolTip(
            advanced_ ? QString() : tr("Not available in Simple mode"));
    }
    if (advanced_button_ != nullptr)
        advanced_button_->setVisible(!advanced_);
    // The half-duplex line is an Advanced setting; a bus made in Simple mode
    // is a plain one.
    if (half_duplex_ != nullptr)
        half_duplex_->setVisible(advanced_);
}

void ConnectPortsDialog::select_from(vss::endpoint_id id)
{
    select_combo_port(from_combo_, id);
    update_ui();
}

void ConnectPortsDialog::select_to(vss::endpoint_id id)
{
    select_combo_port(to_combo_, id);
    update_ui();
}

vss::endpoint_id ConnectPortsDialog::from_id() const
{
    return combo_port_id(from_combo_);
}

vss::endpoint_id ConnectPortsDialog::to_id() const
{
    return combo_port_id(to_combo_);
}

vss::endpoint_id ConnectPortsDialog::split_source_id() const
{
    return combo_port_id(split_source_combo_);
}

void ConnectPortsDialog::select_split_source(vss::endpoint_id id)
{
    select_combo_port(split_source_combo_, id);
    split_targets_.removeAll(id);
    rebuild_split_list();
    update_ui();
}

vss::endpoint_id ConnectPortsDialog::join_target_id() const
{
    return combo_port_id(join_target_combo_);
}

void ConnectPortsDialog::select_join_target(vss::endpoint_id id)
{
    select_combo_port(join_target_combo_, id);
    join_sources_.removeAll(id);
    rebuild_join_list();
    update_ui();
}

QVector<vss::endpoint_id> ConnectPortsDialog::checked_ids(mode kind) const
{
    const QVector<vss::endpoint_id>& wanted =
        kind == mode::split ? split_targets_
        : kind == mode::bus ? bus_members_
                            : join_sources_;
    QVector<vss::endpoint_id> ids;
    for (const port_view& port : ports_)
    {
        if (wanted.contains(port.id))
            ids.push_back(port.id);
    }
    return ids;
}

QVector<vss::endpoint_id> ConnectPortsDialog::target_ids() const
{
    return checked_ids(mode::split);
}

QVector<vss::endpoint_id> ConnectPortsDialog::source_ids() const
{
    return checked_ids(mode::join);
}

QVector<vss::endpoint_id> ConnectPortsDialog::member_ids() const
{
    return checked_ids(mode::bus);
}

void ConnectPortsDialog::set_member_ids(const QVector<vss::endpoint_id>& ids)
{
    bus_members_.clear();
    for (const vss::endpoint_id id : ids)
    {
        if (find_port(id))
            bus_members_.push_back(id);
    }
    rebuild_bus_list();
    update_ui();
}

void ConnectPortsDialog::set_target_ids(const QVector<vss::endpoint_id>& ids)
{
    split_targets_.clear();
    for (const vss::endpoint_id id : ids)
    {
        if (find_port(id) && id != split_source_id())
            split_targets_.push_back(id);
    }
    rebuild_split_list();
    update_ui();
}

void ConnectPortsDialog::set_source_ids(const QVector<vss::endpoint_id>& ids)
{
    join_sources_.clear();
    for (const vss::endpoint_id id : ids)
    {
        if (find_port(id) && id != join_target_id())
            join_sources_.push_back(id);
    }
    rebuild_join_list();
    update_ui();
}

std::optional<port_view>
ConnectPortsDialog::find_port(vss::endpoint_id id) const
{
    const auto it =
        std::find_if(ports_.begin(), ports_.end(),
                     [id](const port_view& port) { return port.id == id; });
    if (it == ports_.end())
        return std::nullopt;
    return *it;
}

void ConnectPortsDialog::clear_error()
{
    last_error_.clear();
    if (error_ != nullptr)
        error_->clear();
}

void ConnectPortsDialog::show_backend_error(const vss::error_code& error,
                                            const QString& from,
                                            const QString& to)
{
    last_error_ = error_text(error);
    if (mode_ == mode::bus && error == vss::errc::cycle_detected)
    {
        error_->show_error(
            tr("That link would create a loop"),
            tr("A path leads out of the bus and back into it, so data would "
               "go round forever. Nothing was changed. Remove that link "
               "first, or choose different ports."),
            last_error_);
    }
    else if (mode_ == mode::bus && error == vss::errc::already_exists)
    {
        error_->show_error(
            tr("Could not create the link"),
            tr("One of these ports is already on a bus. A port can be on one "
               "bus only. Nothing was changed."),
            last_error_);
    }
    else if (error == vss::errc::cycle_detected)
    {
        error_->show_error(
            tr("That link would create a loop"),
            tr("Data would travel from %1 to %2 and back to %1 forever. "
               "Nothing was changed. Remove a link first, or choose different "
               "ports.")
                .arg(from, to),
            last_error_);
    }
    else
    {
        error_->show_error(tr("Could not create the link"),
                           tr("Nothing was changed. Check the ports and try "
                              "again."),
                           last_error_);
    }
}

bool ConnectPortsDialog::submit()
{
    VSS_PROFILE_ZONE("gui.dialogs.connect_ports.submit");
    if (!primary_enabled())
    {
        update_ui();
        return false;
    }
    clear_error();

    // Only a join has a merge policy: the owner option, or the default
    // interleave.
    vss::link_options options;
    if (mode_ == mode::join)
    {
        options.policy = owner_check_ != nullptr && owner_check_->isChecked()
                             ? vss::merge_policy::owner
                             : vss::merge_policy::interleave;
    }

    // Start from an error result; each mode below overwrites it with the
    // outcome of its backend call. from and to are the names used in messages.
    vss::result<vss::link_id> linked =
        vss::make_error_code(vss::errc::internal);
    QString from;
    QString to;
    // Make the backend call for the chosen mode.
    switch (mode_)
    {
    case mode::two_way:
        from = single_name(from_id());
        to = single_name(to_id());
        linked = backend_.connect_ports(from_id(), to_id(), options);
        break;
    case mode::redirect:
        from = single_name(from_id());
        to = single_name(to_id());
        linked = backend_.redirect(from_id(), to_id(), options);
        break;
    case mode::monitor:
        from = single_name(from_id());
        to = single_name(to_id());
        linked = backend_.monitor(from_id(), to_id());
        break;
    case mode::split:
    {
        from = single_name(split_source_id());
        const QVector<vss::endpoint_id> targets = checked_ids(mode::split);
        to = names_for(targets);
        linked = backend_.split(
            split_source_id(),
            std::vector<vss::endpoint_id>(targets.begin(), targets.end()),
            options);
        break;
    }
    case mode::join:
    {
        const QVector<vss::endpoint_id> sources = checked_ids(mode::join);
        from = names_for(sources);
        to = single_name(join_target_id());
        linked = backend_.join(
            std::vector<vss::endpoint_id>(sources.begin(), sources.end()),
            join_target_id(), options);
        break;
    }
    case mode::bus:
    {
        const QVector<vss::endpoint_id> members = checked_ids(mode::bus);
        from = names_for(members);
        if (half_duplex_ != nullptr && advanced_)
            options.half_duplex = half_duplex_->options();
        linked = backend_.bus(
            std::vector<vss::endpoint_id>(members.begin(), members.end()),
            options);
        break;
    }
    }

    // On failure explain it in the dialog and leave it open so the user can
    // adjust and try again.
    if (!linked)
    {
        show_backend_error(linked.error(), from, to);
        return false;
    }

    // Record the new link and a one-line summary for the caller.
    result_link_ = *linked;
    switch (mode_)
    {
    case mode::two_way:
        result_summary_ = tr("Connected %1 and %2.").arg(from, to);
        break;
    case mode::redirect:
        result_summary_ = tr("Redirected %1 to %2.").arg(from, to);
        break;
    case mode::monitor:
        result_summary_ = tr("Monitoring %1 on %2.").arg(from, to);
        break;
    case mode::split:
        result_summary_ = tr("Split %1 to %2.").arg(from, to);
        break;
    case mode::join:
        result_summary_ = tr("Joined %1 into %2.").arg(from, to);
        break;
    case mode::bus:
        result_summary_ = tr("Put %1 on a shared bus.").arg(from);
        break;
    }
    // Close only after the result has been stored.
    QDialog::accept();
    return true;
}

QString ConnectPortsDialog::last_error() const
{
    return last_error_;
}

QString ConnectPortsDialog::error_title() const
{
    return error_ == nullptr ? QString() : error_->title();
}

QString ConnectPortsDialog::error_body() const
{
    return error_ == nullptr ? QString() : error_->body();
}

bool ConnectPortsDialog::error_visible() const
{
    return error_ != nullptr && error_->is_showing();
}

} // namespace vss::gui
