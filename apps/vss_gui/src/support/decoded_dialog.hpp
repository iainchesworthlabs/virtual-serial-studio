//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Decoded traffic": what every running decoder has found, frames and
// triggers together, newest first, from the stores the plugin host keeps
// (docs/gui-design/docs/screens/11-decoding-and-triggers.md). It reads; it
// never decodes a byte. The words of a row come from a plugin and are shown
// as plain text.
//

#pragma once

#include "backend/plugin_backend.hpp"

#include <QDialog>

class QCheckBox;
class QLabel;
class QStandardItemModel;
class QTableView;
class QTimer;

namespace vss::gui::support
{

class DecodedDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit DecodedDialog(PluginBackend& backend, QWidget* parent = nullptr);

    /// Reads the stores now, instead of at the next tick of the timer.
    void refresh();
    int row_count() const;
    QString cell(int row, int column) const;
    QCheckBox* triggers_only_check() const noexcept { return triggers_only_; }
    QLabel* empty_label() const noexcept { return empty_; }

private:
    PluginBackend& backend_;
    QTableView* table_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QCheckBox* triggers_only_ = nullptr;
    QLabel* empty_ = nullptr;
    QLabel* counts_ = nullptr;
    QTimer* timer_ = nullptr;
    /// What the stores looked like at the last read, so that a quiet tick
    /// changes nothing.
    QString signature_;
};

} // namespace vss::gui::support
