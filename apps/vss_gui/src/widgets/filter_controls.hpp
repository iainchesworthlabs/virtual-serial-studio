//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <QComboBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QString>
#include <QVariant>
#include <QWidget>

#include <utility>
#include <vector>

namespace vss::gui
{

/// The filter controls the Monitor and the Log share, so the two tabs have
/// the same port picker and search box. The texts are the caller's, in the
/// caller's translation context.

/// A port picker 120 to 160 px wide with an accessible name.
inline QComboBox* make_port_filter(const QString& accessible_name,
                                   QWidget* parent)
{
    auto* combo = new QComboBox(parent);
    combo->setAccessibleName(accessible_name);
    combo->setMinimumWidth(120);
    combo->setMaximumWidth(160);
    return combo;
}

/// A search box with a clear button, named and prompted with `text`.
inline QLineEdit* make_search_field(const QString& text, QWidget* parent)
{
    auto* edit = new QLineEdit(parent);
    edit->setPlaceholderText(text);
    edit->setAccessibleName(text);
    edit->setClearButtonEnabled(true);
    edit->setMaximumWidth(190);
    return edit;
}

/// Fills `combo` with `all_text` (data `all_value`) and then `choices`, and
/// keeps the choice the person had when it is still there. Sends no signal.
inline void fill_filter_choices(
    QComboBox* combo, const QString& all_text, const QVariant& all_value,
    const std::vector<std::pair<QString, QVariant>>& choices)
{
    const QVariant previous = combo->currentData();
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(all_text, all_value);
    for (const auto& [text, value] : choices)
        combo->addItem(text, value);
    const int index = combo->findData(previous);
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

/// A search matches when it is empty or in `text`, whatever the case.
inline bool matches_search(const QString& text, const QString& search)
{
    return search.isEmpty() || text.contains(search, Qt::CaseInsensitive);
}

} // namespace vss::gui
