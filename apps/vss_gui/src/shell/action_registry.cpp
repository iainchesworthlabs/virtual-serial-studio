//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/action_registry.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

#include <vss/core/profile.hpp>

#include <QSet>

#include <utility>

namespace vss::gui
{

ActionRegistry::ActionRegistry(QObject* parent) : QObject(parent)
{
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this]
            {
                // The cache is colour-keyed, so flush before rebuilding the
                // action icons for the new ink colour.
                IconProvider::instance().flush();
                for (QAction* action : order_)
                {
                    const action_spec spec = specs_.value(action);
                    if (!spec.icon.isEmpty())
                    {
                        action->setIcon(IconProvider::instance().icon(
                            spec.icon, colour_role::ink, 16));
                    }
                }
            });
}

QAction* ActionRegistry::add(action_spec spec)
{
    if (spec.id.isEmpty() || by_id_.contains(spec.id))
        return nullptr;

    auto* action = new QAction(spec.text, this);
    action->setObjectName(spec.id);
    action->setData(spec.id);
    action->setShortcut(spec.shortcut);
    action->setShortcutContext(Qt::WindowShortcut);
    action->setCheckable(spec.checkable);
    action->setProperty("actionLevel", spec.level == action_level::advanced
                                           ? QStringLiteral("advanced")
                                           : QStringLiteral("simple"));
    if (!spec.icon.isEmpty())
    {
        action->setIcon(
            IconProvider::instance().icon(spec.icon, colour_role::ink, 16));
    }
    if (spec.handler)
        connect(action, &QAction::triggered, this,
                [handler = spec.handler] { handler(); });
    action->setVisible(spec.level == action_level::simple ||
                       level_ == action_level::advanced);

    by_id_.insert(spec.id, action);
    specs_.insert(action, std::move(spec));
    order_.push_back(action);
    refresh();
    Q_EMIT changed();
    return action;
}

QAction* ActionRegistry::action(const QString& id) const
{
    return by_id_.value(id, nullptr);
}

QVector<QAction*> ActionRegistry::visible(action_level level) const
{
    QVector<QAction*> out;
    for (QAction* action : order_)
    {
        const action_spec spec = specs_.value(action);
        if (spec.level == action_level::simple ||
            level == action_level::advanced)
            out.push_back(action);
    }
    return out;
}

QVector<QAction*> ActionRegistry::search(const QString& query) const
{
    VSS_PROFILE_ZONE_NAMED(zone, "gui.shell.actions.search");
    VSS_PROFILE_ZONE_TEXT(zone, query.toStdString());
    const QString needle = query.trimmed();
    QVector<QAction*> out;
    for (QAction* action : order_)
    {
        const action_spec spec = specs_.value(action);
        if (!spec.palette)
            continue;
        const bool match = needle.isEmpty() ||
                           spec.text.contains(needle, Qt::CaseInsensitive) ||
                           spec.keywords.contains(needle, Qt::CaseInsensitive);
        if (match)
            out.push_back(action);
    }
    return out;
}

void ActionRegistry::set_level(action_level level)
{
    VSS_PROFILE_ZONE("gui.shell.actions.set_level");
    if (level_ == level)
        return;
    level_ = level;
    for (QAction* action : order_)
    {
        const action_spec& spec = specs_[action];
        action->setVisible(spec.level == action_level::simple ||
                           level_ == action_level::advanced);
    }
    Q_EMIT changed();
}

void ActionRegistry::refresh()
{
    VSS_PROFILE_ZONE("gui.shell.actions.refresh");
    for (QAction* action : order_)
    {
        const action_spec& spec = specs_[action];
        if (spec.enabled)
            action->setEnabled(spec.enabled());
    }
}

bool ActionRegistry::shortcuts_unique() const
{
    return duplicate_shortcuts().isEmpty();
}

QStringList ActionRegistry::duplicate_shortcuts() const
{
    QHash<QString, int> counts;
    for (QAction* action : order_)
    {
        const QString key =
            action->shortcut().toString(QKeySequence::NativeText);
        if (!key.isEmpty())
            counts[key] += 1;
    }
    QStringList duplicates;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
    {
        if (it.value() > 1)
            duplicates << it.key();
    }
    duplicates.sort();
    return duplicates;
}

QVector<support::shortcut_entry>
shortcut_entries(const ActionRegistry& registry)
{
    QVector<support::shortcut_entry> entries;
    for (QAction* action : registry.actions())
    {
        if (action->shortcut().isEmpty())
            continue;
        support::shortcut_entry entry;
        entry.action = action->text();
        entry.shortcut = action->shortcut();
        entry.advanced = action->property("actionLevel").toString() ==
                         QLatin1String("advanced");
        entries.push_back(entry);
    }
    return entries;
}

} // namespace vss::gui
