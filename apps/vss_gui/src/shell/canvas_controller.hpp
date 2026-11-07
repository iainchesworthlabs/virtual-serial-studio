//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/canvas_edit.hpp"
#include "backend/studio_backend.hpp"
#include "shell/undo_service.hpp"

#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class QWidget;

namespace vss::gui
{

namespace topology
{
class TopologyView;
class Toolbox;
} // namespace topology

/// Carries out what the Advanced canvas asks for: adding tiles and bundles,
/// wiring, Settings, Duplicate, Delete, Link selected, and the undo step of
/// each.  See `docs/gui-design/docs/screens/07-canvas-editor.md`.
///
/// Every step is recorded by port name, so it can be undone and redone after
/// the ports were made again with new ids.  While a step runs the controller
/// is "in a batch": the window does not clear its history for the change in
/// the set of ports and updates the view once, at the end.
class CanvasController final : public QObject
{
    Q_OBJECT

public:
    CanvasController(StudioBackend& backend, UndoService& undo,
                     topology::TopologyView* view, topology::Toolbox* toolbox,
                     QWidget* dialog_parent);

    /// Advanced mode on or off: the toolbox shows and the view edits.
    void set_editing(bool on);
    bool in_batch() const noexcept { return batch_depth_ > 0; }

    /// What Add does with the sheet's request: builds it and returns an empty
    /// string, or returns the reason it failed.
    using attempt_function =
        std::function<QString(const bundle_request& request)>;
    /// The Add sheet.  The default shows `AddSheet`; the tests replace it.  It
    /// calls `attempt` each time the user chooses Add and stays open while the
    /// answer is a reason, showing it.  It returns true once an attempt
    /// worked, false when the user cancelled.
    using sheet_function =
        std::function<bool(canvas_item item, const attempt_function& attempt)>;
    void set_sheet(sheet_function sheet) { sheet_ = std::move(sheet); }
    /// The Delete confirmation; the default shows `ConfirmDialog`.
    using confirm_function =
        std::function<bool(const QString& title, const QString& body)>;
    void set_confirm(confirm_function confirm)
    {
        confirm_ = std::move(confirm);
    }
    /// Device, Network and Share open the window's own flows.
    void set_flow_opener(std::function<void(canvas_item)> opener)
    {
        open_flow_ = std::move(opener);
    }

    // The operations, also reachable without the view for the tests.
    bool add_item(canvas_item item, std::optional<QPointF> at = {});
    bool add_request(const bundle_request& request,
                     std::optional<QPointF> at = {});
    bool wire(vss::endpoint_id from, vss::endpoint_id to, wire_kind kind);
    bool wire_new(vss::endpoint_id from, slot_kind kind, wire_kind wire,
                  std::optional<QPointF> at = {});
    bool duplicate(const QVector<vss::endpoint_id>& ports);
    bool remove(const QVector<vss::endpoint_id>& ports,
                const QVector<vss::link_id>& links);
    bool link_selected(const QVector<vss::endpoint_id>& ports,
                       vss::endpoint_id hub, canvas_item shape);
    bool change_link(vss::link_id id, const vss::link_options& options);
    bool reverse(vss::link_id id);
    /// Records the undo step of a Settings change made by the dialog.
    void
    record_settings(const QVector<QPair<QString, vss::serial_settings>>& before,
                    const vss::serial_settings& after);

    void open_settings(const QVector<vss::endpoint_id>& ports);
    void open_link_settings(vss::link_id id);

Q_SIGNALS:
    void message(const QString& text);
    /// A step ended; the window updates once and keeps its history.
    void batch_finished();
    /// The user chose "Set up Virtual Serial Studio" from an Add that failed
    /// because the driver or helper is not ready.
    void driver_setup_requested();

private:
    template <typename Function> auto batch(Function&& function)
    {
        ++batch_depth_;
        struct leave
        {
            CanvasController* self;
            ~leave()
            {
                if (--self->batch_depth_ == 0)
                    Q_EMIT self->batch_finished();
            }
        } guard{this};
        return function();
    }

    void push_created(const QString& text, const QStringList& created);
    void select_names(const QStringList& names);
    QVector<vss::endpoint_id> ids_of(const QStringList& names) const;
    QString describe(const QVector<vss::endpoint_id>& ports,
                     const QVector<vss::link_id>& links) const;

    StudioBackend& backend_;
    UndoService& undo_;
    topology::TopologyView* view_;
    topology::Toolbox* toolbox_;
    QWidget* parent_;
    sheet_function sheet_;
    confirm_function confirm_;
    std::function<void(canvas_item)> open_flow_;
    int batch_depth_ = 0;
    bool setup_offered_ = false;
};

} // namespace vss::gui
