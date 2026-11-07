//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/canvas_controller.hpp"

#include "advanced/link_settings_dialog.hpp"
#include "advanced/port_settings_dialog.hpp"
#include "backend/driver_error.hpp"
#include "topology/add_sheet.hpp"
#include "topology/toolbox.hpp"
#include "topology/topology_text.hpp"
#include "topology/topology_view.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QCursor>
#include <QHash>
#include <QWidget>

#include <memory>
#include <utility>

namespace vss::gui
{
namespace
{

QStringList names_in(const removal& what)
{
    QStringList out;
    for (const auto& entry : what.ports)
        out.append(QString::fromStdString(entry.name));
    return out;
}

} // namespace

CanvasController::CanvasController(StudioBackend& backend, UndoService& undo,
                                   topology::TopologyView* view,
                                   topology::Toolbox* toolbox,
                                   QWidget* dialog_parent)
    : QObject(dialog_parent), backend_(backend), undo_(undo), view_(view),
      toolbox_(toolbox), parent_(dialog_parent)
{
    sheet_ = [this](canvas_item item, const attempt_function& attempt)
    {
        topology::AddSheet sheet(item, backend_.ports(), parent_);
        sheet.set_attempt(attempt);
        sheet.set_setup_offer([this] { return setup_offered_; });
        bool set_up = false;
        connect(&sheet, &topology::AddSheet::driver_setup_requested, this,
                [&set_up] { set_up = true; });
        sheet.move(QCursor::pos() - QPoint(sheet.sizeHint().width() / 2, 24));
        const bool added = sheet.exec() == QDialog::Accepted;
        // Said after the sheet is gone, so the set-up dialog does not stack.
        if (set_up)
            Q_EMIT driver_setup_requested();
        return added;
    };
    confirm_ = [this](const QString& title, const QString& body)
    {
        return ConfirmDialog::ask(title, body, tr("Delete"), tr("Cancel"), true,
                                  parent_);
    };

    if (toolbox_ != nullptr)
    {
        toolbox_->set_capabilities(backend_.caps());
        connect(toolbox_, &topology::Toolbox::add_requested, this,
                [this](canvas_item item) { add_item(item); });
    }
    if (view_ == nullptr)
        return;
    connect(view_, &topology::TopologyView::add_requested, this,
            [this](canvas_item item, const QPointF& point, bool at_point)
            {
                add_item(item, at_point ? std::optional<QPointF>(point)
                                        : std::nullopt);
            });
    connect(view_, &topology::TopologyView::wire_requested, this,
            [this](vss::endpoint_id from, vss::endpoint_id to, wire_kind kind)
            { wire(from, to, kind); });
    connect(view_, &topology::TopologyView::wire_new_requested, this,
            [this](vss::endpoint_id from, slot_kind kind, wire_kind link,
                   const QPointF& point)
            { wire_new(from, kind, link, point); });
    connect(view_, &topology::TopologyView::settings_requested, this,
            [this](const QVector<vss::endpoint_id>& ports)
            { open_settings(ports); });
    connect(view_, &topology::TopologyView::link_settings_requested, this,
            [this](vss::link_id id) { open_link_settings(id); });
    connect(view_, &topology::TopologyView::reverse_requested, this,
            [this](vss::link_id id) { reverse(id); });
    connect(view_, &topology::TopologyView::duplicate_requested, this,
            [this](const QVector<vss::endpoint_id>& ports)
            { duplicate(ports); });
    connect(view_, &topology::TopologyView::delete_requested, this,
            [this](const QVector<vss::endpoint_id>& ports,
                   const QVector<vss::link_id>& links)
            { remove(ports, links); });
    connect(view_, &topology::TopologyView::link_selected_requested, this,
            [this](const QVector<vss::endpoint_id>& ports, vss::endpoint_id hub,
                   canvas_item shape) { link_selected(ports, hub, shape); });
}

void CanvasController::set_editing(bool on)
{
    if (view_ != nullptr)
        view_->set_editing(on);
    if (toolbox_ != nullptr)
        toolbox_->setVisible(on);
}

QVector<vss::endpoint_id>
CanvasController::ids_of(const QStringList& names) const
{
    QVector<vss::endpoint_id> ids;
    for (const port_view& port : backend_.ports())
    {
        if (names.contains(port.name))
            ids.append(port.id);
    }
    return ids;
}

void CanvasController::select_names(const QStringList& names)
{
    if (view_ != nullptr && !names.isEmpty())
        view_->set_multi_selection(ids_of(names), {});
}

void CanvasController::push_created(const QString& text,
                                    const QStringList& created)
{
    // Undo takes the new ports away (their links go with them); redo makes
    // them again from what undo saw, by name.
    auto saved = std::make_shared<removal>();
    undo_.push(
        {text,
         [this, created, saved]
         {
             return batch(
                 [&]
                 {
                     *saved = describe_removal(backend_, ids_of(created), {});
                     return saved->ports.size() == created.size() &&
                            apply_removal(backend_, *saved).has_value();
                 });
         },
         [this, saved]
         {
             return batch(
                 [&] { return restore_removal(backend_, *saved).has_value(); });
         }});
}

bool CanvasController::add_item(canvas_item item, std::optional<QPointF> at)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.add_item");
    if (!item_available(backend_.caps(), item))
    {
        Q_EMIT message(QCoreApplication::translate(
            "Advanced", "Not available in this build"));
        return false;
    }
    if (!is_bundle(item))
    {
        if (open_flow_)
            open_flow_(item);
        return false;
    }
    // One sheet for the whole add.  Add builds the bundle; when that fails the
    // sheet stays open with the reason and what was typed, and nothing was
    // made (`build_bundle` undoes a half-made bundle).  A sheet that closed and
    // opened again on every failure looked like Add did nothing.
    return sheet_(
        item,
        [this, item, at](const bundle_request& request) -> QString
        {
            const auto made = batch(
                [&]
                {
                    auto result = build_bundle(backend_, request);
                    if (result && at && view_ != nullptr)
                        view_->place_next_at(*at);
                    return result;
                });
            if (!made)
            {
                // A driver problem is a whole sentence with its own next step.
                const auto problem = driver_problem_of(made.error());
                setup_offered_ = problem && driver_problem_offers_setup(*problem);
                const QString error =
                    problem ? error_text(made.error())
                            : tr("Nothing was added: %1")
                                  .arg(error_text(made.error()));
                Q_EMIT message(error);
                return error;
            }
            push_created(tr("Add %1").arg(topology::item_label(item).toLower()),
                         made->created);
            select_names(made->created);
            Q_EMIT message(
                tr("Added %1.").arg(made->created.join(QStringLiteral(", "))));
            return {};
        });
}

bool CanvasController::add_request(const bundle_request& request,
                                   std::optional<QPointF> at)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.add_request");
    const auto made = batch(
        [&]
        {
            auto result = build_bundle(backend_, request);
            if (result && at && view_ != nullptr)
                view_->place_next_at(*at);
            return result;
        });
    if (!made)
    {
        Q_EMIT message(
            tr("Nothing was added: %1").arg(error_text(made.error())));
        return false;
    }
    push_created(tr("Add %1").arg(topology::item_label(request.item).toLower()),
                 made->created);
    select_names(made->created);
    return true;
}

bool CanvasController::wire(vss::endpoint_id from, vss::endpoint_id to,
                            wire_kind kind)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.wire");
    const auto made =
        batch([&] { return make_wire(backend_, from, to, kind); });
    if (!made)
    {
        Q_EMIT message(
            tr("Could not link the ports: %1").arg(error_text(made.error())));
        return false;
    }
    const auto link = backend_.link(*made);
    if (!link)
        return true;
    const link_recipe recipe = recipe_of(backend_.ports(), *link);
    undo_.push({tr("Connect ports"),
                [this, recipe]
                {
                    return batch(
                        [&]
                        {
                            const auto found = find_link(backend_, recipe);
                            if (!found || !backend_.disconnect(found->id))
                                return false;
                            backend_.refresh();
                            return true;
                        });
                },
                [this, recipe]
                {
                    return batch(
                        [&]
                        { return make_link(backend_, recipe).has_value(); });
                }});
    Q_EMIT message(tr("Linked %1.")
                       .arg(topology_text::link_name(
                           *link,
                           [this]
                           {
                               QHash<vss::endpoint_id, QString> names;
                               for (const port_view& port : backend_.ports())
                                   names.insert(port.id, port.name);
                               return names;
                           }())));
    return true;
}

bool CanvasController::wire_new(vss::endpoint_id from, slot_kind kind,
                                wire_kind wire, std::optional<QPointF> at)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.wire_new");
    const auto source = backend_.port(from);
    if (!source)
        return false;
    bundle_request request;
    request.item = kind == slot_kind::new_fixture    ? canvas_item::fixture
                   : kind == slot_kind::new_loopback ? canvas_item::loopback
                                                     : canvas_item::port;
    request = default_request(request.item, backend_.ports(), 1);

    QStringList created;
    const bool ok = batch(
        [&]
        {
            const auto made = build_bundle(backend_, request);
            if (!made || made->created.isEmpty())
                return false;
            created = made->created;
            const auto ids = ids_of(created);
            if (ids.isEmpty())
                return false;
            const auto linked = make_wire(backend_, from, ids.first(), wire);
            if (!linked)
            {
                static_cast<void>(apply_removal(
                    backend_, describe_removal(backend_, ids, {})));
                return false;
            }
            if (at && view_ != nullptr)
                view_->place_next_at(*at);
            return true;
        });
    if (!ok)
    {
        Q_EMIT message(tr("Could not add and link a new port."));
        return false;
    }
    push_created(tr("Add %1").arg(topology::item_label(request.item).toLower()),
                 created);
    select_names(created);
    return true;
}

bool CanvasController::duplicate(const QVector<vss::endpoint_id>& ports)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.duplicate");
    QPointF anchor;
    bool placed = false;
    if (view_ != nullptr && !ports.isEmpty())
    {
        anchor =
            view_->node_rect(ports.first()).topLeft() + QPointF(24.0, 24.0);
        placed = true;
    }
    const auto made = batch(
        [&]
        {
            auto result = duplicate_ports(backend_, ports);
            if (result && placed)
                view_->place_next_at(anchor);
            return result;
        });
    if (!made)
    {
        Q_EMIT message(
            made.error() == vss::make_error_code(vss::errc::unsupported)
                ? tr("This port can only be opened once.")
                : tr("Could not duplicate: %1").arg(error_text(made.error())));
        return false;
    }
    push_created(tr("Duplicate"), made->created);
    select_names(made->created);
    Q_EMIT message(
        tr("Added %1.").arg(made->created.join(QStringLiteral(", "))));
    return true;
}

QString CanvasController::describe(const QVector<vss::endpoint_id>& ports,
                                   const QVector<vss::link_id>& links) const
{
    QStringList names;
    for (const vss::endpoint_id id : ports)
    {
        if (const auto view = backend_.port(id))
            names.append(view->name);
    }
    QString text;
    if (names.size() == 1)
        text = names.first();
    else if (names.size() > 1)
        text =
            tr("%1 and %2")
                .arg(names.mid(0, names.size() - 1).join(QStringLiteral(", ")),
                     names.last());
    if (!links.isEmpty())
    {
        const QString link_text =
            tr("%n link(s)", nullptr, static_cast<int>(links.size()));
        text =
            text.isEmpty() ? link_text : tr("%1 and %2").arg(text, link_text);
    }
    return text;
}

bool CanvasController::remove(const QVector<vss::endpoint_id>& ports,
                              const QVector<vss::link_id>& links)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.remove");
    if (ports.isEmpty() && links.isEmpty())
        return false;
    const removal what = describe_removal(backend_, ports, links);
    // One port with no links goes at once; anything more is confirmed.
    const bool lone = what.ports.size() == 1 && what.links.isEmpty();
    if (!lone)
    {
        const QString subject = describe(ports, links);
        const QString body =
            what.links.size() > links.size()
                ? tr("Delete %1 and the links on them? Undo brings them back.")
                      .arg(subject)
                : tr("Delete %1? Undo brings it back.").arg(subject);
        if (!confirm_(tr("Delete %1?").arg(subject), body))
            return false;
    }
    const auto removed = batch([&] { return apply_removal(backend_, what); });
    if (!removed)
    {
        Q_EMIT message(
            tr("Could not delete: %1").arg(error_text(removed.error())));
        return false;
    }
    // The name goes into the Edit menu ("Undo Delete ..."), so a long list of
    // ports is counted rather than spelled out.
    const QStringList removed_names = names_in(what);
    const QString subject =
        removed_names.isEmpty()  ? tr("link")
        : removed_names.size() > 3 ? tr("%1 ports").arg(removed_names.size())
                                   : removed_names.join(QStringLiteral(", "));
    undo_.push(
        {tr("Delete %1").arg(subject),
         [this, what]
         {
             return batch(
                 [&]
                 {
                     const auto restored = restore_removal(backend_, what);
                     if (!restored)
                         return false;
                     for (const QStringList& moved : *restored)
                     {
                         Q_EMIT message(tr("Recreated %1 as %2 (it was %3).")
                                            .arg(moved.value(0), moved.value(2),
                                                 moved.value(1)));
                     }
                     return true;
                 });
         },
         [this, what]
         {
             return batch(
                 [&] { return apply_removal(backend_, what).has_value(); });
         }});
    Q_EMIT message(tr("Deleted %1.").arg(subject));
    return true;
}

bool CanvasController::link_selected(const QVector<vss::endpoint_id>& ports,
                                     vss::endpoint_id hub, canvas_item shape)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.link_selected");
    bundle_request request;
    request.item = shape;
    QVector<vss::endpoint_id> order;
    if (shape != canvas_item::pair && shape != canvas_item::bus)
        order.append(hub);
    for (const vss::endpoint_id id : ports)
    {
        if (!order.contains(id))
            order.append(id);
    }
    for (const vss::endpoint_id id : order)
    {
        const auto view = backend_.port(id);
        if (!view)
            return false;
        request.slots.append({slot_kind::existing, view->name});
    }
    const auto made = batch([&] { return build_bundle(backend_, request); });
    if (!made || made->links.isEmpty())
    {
        Q_EMIT message(tr("Could not link the ports: %1")
                           .arg(made ? QString() : error_text(made.error())));
        return false;
    }
    const auto link = backend_.link(made->links.first());
    if (!link)
        return true;
    const link_recipe recipe = recipe_of(backend_.ports(), *link);
    undo_.push(
        {tr("Link selected"),
         [this, recipe]
         {
             return batch(
                 [&]
                 {
                     const auto found = find_link(backend_, recipe);
                     return found && backend_.disconnect(found->id).has_value();
                 });
         },
         [this, recipe]
         {
             return batch([&]
                          { return make_link(backend_, recipe).has_value(); });
         }});
    return true;
}

bool CanvasController::change_link(vss::link_id id,
                                   const vss::link_options& options)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.change_link");
    const auto before = backend_.link(id);
    if (!before)
        return false;
    const link_recipe old_recipe = recipe_of(backend_.ports(), *before);
    link_recipe new_recipe = old_recipe;
    new_recipe.options = options;
    const auto made = batch([&] { return remake_link(backend_, id, options); });
    if (!made)
    {
        Q_EMIT message(
            tr("Could not change the link: %1").arg(error_text(made.error())));
        return false;
    }
    const auto swap = [this](const link_recipe& from, const link_recipe& to)
    {
        return batch(
            [&]
            {
                const auto found = find_link(backend_, from);
                return found &&
                       remake_link(backend_, found->id, to.options).has_value();
            });
    };
    undo_.push({tr("Link settings"), [swap, old_recipe, new_recipe]
                { return swap(new_recipe, old_recipe); },
                [swap, old_recipe, new_recipe]
                { return swap(old_recipe, new_recipe); }});
    return true;
}

bool CanvasController::reverse(vss::link_id id)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.reverse");
    const auto link = backend_.link(id);
    if (!link || link->shape != link_shape::one_way)
        return false;
    const link_recipe forward = recipe_of(backend_.ports(), *link);
    link_recipe backward = forward;
    std::swap(backward.sources, backward.targets);
    const auto flip = [this](const link_recipe& from, const link_recipe& to)
    {
        return batch(
            [&]
            {
                const auto found = find_link(backend_, from);
                if (!found || !backend_.disconnect(found->id))
                    return false;
                if (make_link(backend_, to))
                    return true;
                static_cast<void>(make_link(backend_, from));
                return false;
            });
    };
    if (!flip(forward, backward))
    {
        Q_EMIT message(tr("Could not reverse the link."));
        return false;
    }
    undo_.push({tr("Reverse direction"),
                [flip, forward, backward] { return flip(backward, forward); },
                [flip, forward, backward] { return flip(forward, backward); }});
    return true;
}

void CanvasController::record_settings(
    const QVector<QPair<QString, vss::serial_settings>>& before,
    const vss::serial_settings& after)
{
    VSS_PROFILE_ZONE("gui.shell.canvas.record_settings");
    if (before.isEmpty())
        return;
    const auto apply =
        [this](const QVector<QPair<QString, vss::serial_settings>>& values)
    {
        bool ok = true;
        for (const auto& [name, settings] : values)
        {
            const auto ids = ids_of({name});
            ok = ok && !ids.isEmpty() &&
                 backend_.set_port_settings(ids.first(), settings).has_value();
        }
        backend_.refresh();
        return ok;
    };
    QVector<QPair<QString, vss::serial_settings>> next;
    for (const auto& [name, settings] : before)
        next.append({name, after});
    const QString text = before.size() == 1
                             ? tr("Settings for %1").arg(before.first().first)
                             : tr("Settings for %n ports", nullptr,
                                  static_cast<int>(before.size()));
    undo_.push({text, [apply, before] { return apply(before); },
                [apply, next] { return apply(next); }});
}

void CanvasController::open_settings(const QVector<vss::endpoint_id>& ports)
{
    if (ports.isEmpty())
        return;
    PortSettingsDialog dialog(backend_, ports, parent_);
    connect(&dialog, &PortSettingsDialog::error_shown, this,
            [this](const QString& text) { Q_EMIT message(text); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    record_settings(dialog.before(), dialog.settings());
    Q_EMIT message(tr("Settings changed."));
}

void CanvasController::open_link_settings(vss::link_id id)
{
    const auto link = backend_.link(id);
    if (!link)
        return;
    QHash<vss::endpoint_id, QString> names;
    for (const port_view& port : backend_.ports())
        names.insert(port.id, port.name);
    LinkSettingsDialog dialog(*link, topology_text::link_name(*link, names),
                              parent_);
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (change_link(id, dialog.options()))
        Q_EMIT message(tr("Link settings changed."));
}

} // namespace vss::gui
