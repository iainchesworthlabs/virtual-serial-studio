//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "simple/simple_page.hpp"

#include "simple/job_grid.hpp"
#include "simple/job_plan.hpp"
#include "simple/setup_page.hpp"
#include "theme/icon_provider.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace vss::gui::simple
{

namespace
{

/// Delete on the list removes the selected setup.
class SetupList final : public QListWidget
{
public:
    using QListWidget::QListWidget;
    std::function<void()> on_delete;

protected:
    bool event(QEvent* event) override
    {
        // Claim Delete before the window's Remove shortcut does, so it
        // removes the whole setup this row stands for.
        if (event->type() == QEvent::ShortcutOverride && on_delete &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Delete)
        {
            event->accept();
            return true;
        }
        return QListWidget::event(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Delete && on_delete)
        {
            on_delete();
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
};

QString end_line(const port_view& port)
{
    const QString name = port_shown_name(port);
    const QString label = program_label(port);
    QString shown = label.isEmpty()
                              ? name
                              : QCoreApplication::translate(
                                    "vss::gui::simple::SimplePage", "%1 (%2)")
                                    .arg(name, label);
    if (port.kind == vss::endpoint_kind::virtual_port)
    {
        if (port.peer == peer_state::open)
            return QCoreApplication::translate("vss::gui::simple::SimplePage",
                                               "%1, open in a program")
                .arg(shown);
        if (port.peer == peer_state::not_open)
            return QCoreApplication::translate("vss::gui::simple::SimplePage",
                                               "%1, no program yet")
                .arg(shown);
        return shown;
    }
    if (port.kind == vss::endpoint_kind::physical)
        return QCoreApplication::translate("vss::gui::simple::SimplePage",
                                           "%1, device")
            .arg(shown);
    return shown;
}

} // namespace

SimplePage::SimplePage(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    setObjectName(QStringLiteral("simplePage"));
    build();
    refresh();
    const auto queue = [this]
    {
        if (refresh_queued_)
            return;
        refresh_queued_ = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               refresh_queued_ = false;
                               refresh();
                           });
    };
    connect(&backend_, &StudioBackend::ports_changed, this, queue);
    connect(&backend_, &StudioBackend::links_changed, this, queue);
    connect(&backend_, &StudioBackend::batch_ready, this,
            [this](const event_batch& events) { add_events(events); });
}

void SimplePage::build()
{
    VSS_PROFILE_ZONE("gui.simple.page.build");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    // Your setups.
    auto* side = new QFrame(this);
    side->setObjectName(QStringLiteral("simpleSetups"));
    side->setProperty("card", true);
    polish(side);
    side->setMinimumWidth(240);
    side->setMaximumWidth(360);
    auto* side_layout = new QVBoxLayout(side);
    side_layout->setContentsMargins(8, 8, 8, 8);
    side_layout->setSpacing(8);
    auto* head = new QHBoxLayout();
    auto* title = new QLabel(tr("Your setups"), side);
    QFont heading = Theme::instance().font(text_style::bodyLarge);
    heading.setWeight(QFont::DemiBold);
    title->setFont(heading);
    count_ = new QLabel(side);
    count_->setProperty("muted", true);
    auto* add = new QPushButton(tr("New"), side);
    add->setObjectName(QStringLiteral("simpleNew"));
    add->setIcon(
        IconProvider::instance().icon(QStringLiteral("add"), colour_role::ink));
    add->setAccessibleName(tr("Set up something new"));
    add->setToolTip(tr("Set up something new"));
    set_role(add, "ghost");
    connect(add, &QPushButton::clicked, this, [this] { show_jobs(); });
    head->addWidget(title);
    head->addWidget(count_);
    head->addStretch(1);
    head->addWidget(add);
    side_layout->addLayout(head);

    auto* list = new SetupList(side);
    list_ = list;
    list_->setObjectName(QStringLiteral("simpleSetupList"));
    list_->setAccessibleName(tr("Your setups"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setWordWrap(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list->on_delete = [this]
    {
        if (shown_ != 0)
            Q_EMIT remove_setup_requested(shown_);
    };
    connect(
        list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
        { select_setup(item->data(Qt::UserRole).value<vss::endpoint_id>()); });
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* item)
            {
                if (item != nullptr && list_->hasFocus())
                    select_setup(
                        item->data(Qt::UserRole).value<vss::endpoint_id>());
            });
    side_layout->addWidget(list_, 1);

    auto* foot = new QHBoxLayout();
    auto* save = new QPushButton(tr("Save…"), side);
    set_role(save, "secondary");
    connect(save, &QPushButton::clicked, this,
            [this] { Q_EMIT save_requested(); });
    auto* open = new QPushButton(tr("Open…"), side);
    set_role(open, "secondary");
    connect(open, &QPushButton::clicked, this,
            [this] { Q_EMIT open_requested(); });
    remove_all_ = new QPushButton(tr("Remove all"), side);
    remove_all_->setObjectName(QStringLiteral("simpleRemoveAll"));
    set_role(remove_all_, "danger");
    connect(remove_all_, &QPushButton::clicked, this,
            [this] { Q_EMIT remove_all_requested(); });
    foot->addWidget(save);
    foot->addWidget(open);
    foot->addStretch(1);
    foot->addWidget(remove_all_);
    side_layout->addLayout(foot);
    layout->addWidget(side, 0);

    // The page.
    auto* main = new QFrame(this);
    main->setProperty("card", true);
    polish(main);
    auto* main_layout = new QVBoxLayout(main);
    main_layout->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(main);
    main_layout->addWidget(stack_);
    layout->addWidget(main, 1);

    grid_ = new JobGrid(backend_, stack_);
    stack_->addWidget(grid_);
    connect(grid_, &JobGrid::job_chosen, this,
            [this](job kind) { open_job(kind); });
    connect(grid_, &JobGrid::check_requested, this,
            [this] { Q_EMIT check_requested(); });
    connect(grid_, &JobGrid::driver_setup_requested, this,
            [this] { Q_EMIT driver_setup_requested(); });
    connect(grid_, &JobGrid::open_requested, this,
            [this] { Q_EMIT open_requested(); });
    connect(grid_, &JobGrid::examples_requested, this,
            [this] { Q_EMIT examples_requested(); });
    connect(grid_, &JobGrid::watch_requested, this,
            [this]
            {
                if (setups_.empty())
                {
                    Q_EMIT message(tr("Nothing is set up yet, so there is no "
                                      "port to look at. Set something up "
                                      "first."));
                    return;
                }
                select_setup(setups_.front().key());
                if (page_ != nullptr)
                    page_->set_show_data(true);
                Q_EMIT message(tr("Choose a setup on the left to see its "
                                  "data."));
            });
}

const setup_view* SimplePage::find(vss::endpoint_id port) const
{
    for (const auto& setup : setups_)
    {
        if (setup.group.ports.contains(port))
            return &setup;
    }
    return nullptr;
}

setup_prefs SimplePage::prefs_of(const setup_view& setup) const
{
    for (const auto id : setup.group.ports)
    {
        if (prefs_.contains(id))
            return prefs_.value(id);
    }
    return {};
}

void SimplePage::store_prefs(const setup_view& setup, const setup_prefs& prefs)
{
    for (const auto id : setup.group.ports)
        prefs_.remove(id);
    if (setup.key() != 0)
        prefs_.insert(setup.key(), prefs);
}

void SimplePage::refresh()
{
    VSS_PROFILE_ZONE("gui.simple.page.refresh");
    setups_ = list_setups(backend_.ports(), backend_.links());
    // Forget the choices of setups that are gone.
    QSet<vss::endpoint_id> alive;
    for (const auto& setup : setups_)
    {
        for (const auto id : setup.group.ports)
            alive.insert(id);
    }
    for (auto it = prefs_.begin(); it != prefs_.end();)
    {
        if (!alive.contains(it.key()))
            it = prefs_.erase(it);
        else
            ++it;
    }
    rebuild_list();
    grid_->refresh();
    if (shown_ != 0)
    {
        if (const setup_view* setup = find(shown_))
        {
            if (page_ != nullptr)
                page_->refresh(*setup);
        }
        else
        {
            show_jobs();
        }
    }
}

void SimplePage::rebuild_list()
{
    VSS_PROFILE_ZONE("gui.simple.page.rebuild_list");
    const QSignalBlocker blocker(list_);
    list_->clear();
    const auto ports = backend_.ports();
    count_->setText(QString::number(setups_.size()));
    count_->setAccessibleName(
        tr("%n setup(s)", nullptr, static_cast<int>(setups_.size())));
    remove_all_->setEnabled(!setups_.empty());
    if (setups_.empty())
    {
        auto* empty = new QListWidgetItem(
            tr("Nothing set up yet. Choose what you want to do on the right."),
            list_);
        empty->setFlags(Qt::NoItemFlags);
        return;
    }
    for (const auto& setup : setups_)
    {
        const setup_prefs prefs = prefs_of(setup);
        QString title =
            prefs.name.isEmpty() ? setup_title(setup, ports) : prefs.name;
        if (setup.kind == job::custom)
            title = tr("%1 (Advanced)").arg(title);
        QStringList lines{title};
        for (const auto id : setup.group.ports)
        {
            if (id == setup.echo)
                continue;
            for (const auto& port : ports)
            {
                if (port.id == id)
                    lines << QStringLiteral("    ") + end_line(port);
            }
        }
        auto* item = new QListWidgetItem(lines.join(QLatin1Char('\n')), list_);
        item->setData(Qt::UserRole, QVariant::fromValue(setup.key()));
        QFont font = item->font();
        item->setFont(font);
        if (shown_ != 0 && setup.group.ports.contains(shown_))
            list_->setCurrentItem(item);
    }
}

void SimplePage::show_jobs()
{
    shown_ = 0;
    if (form_ != nullptr)
        form_->deleteLater();
    if (page_ != nullptr)
        page_->deleteLater();
    stack_->setCurrentWidget(grid_);
    grid_->refresh();
    {
        const QSignalBlocker blocker(list_);
        list_->clearSelection();
        list_->setCurrentItem(nullptr);
    }
    Q_EMIT setup_selected(0);
}

void SimplePage::open_job(job kind)
{
    form_return_ = 0;
    show_form(kind, std::nullopt);
}

void SimplePage::show_form(job kind, const std::optional<setup_view>& previous)
{
    VSS_PROFILE_ZONE("gui.simple.page.show_form");
    if (form_ != nullptr)
        form_->deleteLater();
    const setup_prefs prefs = previous ? prefs_of(*previous) : setup_prefs{};
    form_ = new JobForm(backend_, kind, previous, prefs, stack_);
    stack_->addWidget(form_);
    stack_->setCurrentWidget(form_);
    connect(form_, &JobForm::driver_setup_requested, this,
            [this] { Q_EMIT driver_setup_requested(); });
    connect(form_, &JobForm::cancelled, this,
            [this]
            {
                if (form_return_ != 0 && find(form_return_) != nullptr)
                    select_setup(form_return_);
                else
                    show_jobs();
            });
    connect(form_, &JobForm::done, this,
            [this](vss::endpoint_id port, const setup_prefs& chosen)
            {
                backend_.refresh();
                refresh();
                if (const setup_view* setup = find(port))
                {
                    store_prefs(*setup, chosen);
                    rebuild_list();
                    select_setup(port);
                    Q_EMIT message(
                        tr("Done. %1.")
                            .arg(chosen.name.isEmpty()
                                     ? setup_title(*setup, backend_.ports())
                                     : chosen.name));
                }
                else
                {
                    show_jobs();
                }
            });
    if (QWidget* first =
            form_->findChild<QWidget*>(QStringLiteral("jobFormLabel")))
        first->setFocus();
}

void SimplePage::select_setup(vss::endpoint_id port)
{
    const setup_view* setup = find(port);
    if (setup == nullptr)
    {
        show_jobs();
        return;
    }
    show_setup(*setup);
}

void SimplePage::show_setup(const setup_view& setup)
{
    VSS_PROFILE_ZONE("gui.simple.page.show_setup");
    const bool same = page_ != nullptr && shown_ != 0 &&
                      setup.group.ports.contains(shown_) &&
                      stack_->currentWidget() == page_;
    shown_ = setup.key();
    if (form_ != nullptr)
        form_->deleteLater();
    if (!same)
    {
        if (page_ != nullptr)
            page_->deleteLater();
        page_ = new SetupPage(backend_, setup, prefs_of(setup), stack_);
        stack_->addWidget(page_);
        connect(page_, &SetupPage::change_requested, this,
                [this]
                {
                    if (const setup_view* current = find(shown_))
                    {
                        form_return_ = shown_;
                        show_form(current->kind, *current);
                    }
                });
        connect(page_, &SetupPage::remove_requested, this,
                [this] { Q_EMIT remove_setup_requested(shown_); });
        connect(page_, &SetupPage::advanced_requested, this,
                [this] { Q_EMIT advanced_requested(); });
        connect(page_, &SetupPage::troubleshoot_requested, this,
                [this] { Q_EMIT troubleshoot_requested(); });
        connect(page_, &SetupPage::message, this,
                [this](const QString& text) { Q_EMIT message(text); });
        connect(page_, &SetupPage::prefs_changed, this,
                [this](const setup_prefs& prefs)
                {
                    if (const setup_view* current = find(shown_))
                        store_prefs(*current, prefs);
                });
        connect(page_, &SetupPage::add_program_requested, this,
                [this]
                {
                    if (const setup_view* current = find(shown_))
                        change_programs(*current, 0);
                });
        connect(page_, &SetupPage::close_port_requested, this,
                [this](vss::endpoint_id id)
                {
                    if (const setup_view* current = find(shown_))
                        change_programs(*current, id);
                });
    }
    else
    {
        page_->refresh(setup);
    }
    stack_->setCurrentWidget(page_);
    {
        const QSignalBlocker blocker(list_);
        for (int row = 0; row < list_->count(); ++row)
        {
            QListWidgetItem* item = list_->item(row);
            if (setup.group.ports.contains(
                    item->data(Qt::UserRole).value<vss::endpoint_id>()))
                list_->setCurrentItem(item);
        }
    }
    Q_EMIT setup_selected(shown_);
}

// A copy is the point: refresh() below rebuilds setups_ under the caller's
// reference.
void SimplePage::change_programs(
    setup_view setup, // NOLINT(performance-unnecessary-value-param)
    vss::endpoint_id close)
{
    VSS_PROFILE_ZONE("gui.simple.page.change_programs");
    job_plan plan = plan_from(setup, backend_.ports());
    auto& programs = setup.kind == job::share || setup.kind == job::attach
                         ? plan.second
                         : plan.first;
    QString what;
    if (close != 0)
    {
        programs.erase(std::remove_if(programs.begin(), programs.end(),
                                      [close](const end_plan& end)
                                      { return end.existing == close; }),
                       programs.end());
        if (const auto port = backend_.port(close))
            what = tr("Closed %1. The other programs were not interrupted.")
                       .arg(port_shown_name(*port));
        if (setup.kind == job::attach && programs.size() == 1)
            plan.kind = job::attach;
    }
    else
    {
        end_plan added;
        added.what = end_plan::kind::program;
        programs.push_back(added);
        if (setup.kind == job::attach)
            plan.kind = job::share;
    }
    if (setup.kind == job::share && programs.size() == 1)
        plan.kind = job::attach;
    const vss::endpoint_id keep =
        setup.key() == close
            ? (plan.first.empty() ? 0 : plan.first.front().existing)
            : setup.key();
    const setup_prefs prefs = prefs_of(setup);
    const auto built = build_setup(backend_, plan, &setup);
    if (!built)
    {
        Q_EMIT message(tr("Could not change the setup. %1")
                           .arg(error_text(built.error())));
        return;
    }
    backend_.refresh();
    refresh();
    const vss::endpoint_id select = keep != 0 ? keep : *built;
    if (const setup_view* changed = find(select))
    {
        store_prefs(*changed, prefs);
        if (close == 0)
        {
            // The new port is the one that was not there before.
            QString added_name;
            for (const auto id : changed->programs)
            {
                if (!setup.group.ports.contains(id))
                {
                    if (const auto port = backend_.port(id))
                        added_name = port_shown_name(*port);
                }
            }
            what = tr("Added %1. The other programs were not interrupted.")
                       .arg(added_name);
        }
        select_setup(select);
    }
    if (!what.isEmpty())
        Q_EMIT message(what);
}

void SimplePage::add_events(const event_batch& events)
{
    VSS_PROFILE_ZONE("gui.simple.page.add_events");
    if (page_ != nullptr && stack_->currentWidget() == page_)
        page_->add_events(events);
}

} // namespace vss::gui::simple
