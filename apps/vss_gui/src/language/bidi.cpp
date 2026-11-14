//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "language/bidi.hpp"

#include <QGuiApplication>
#include <QWidget>

namespace vss::gui
{

QString isolate_name(const QString& name, bool right_to_left)
{
    if (!right_to_left || name.isEmpty() ||
        name.front() == QChar(first_strong_isolate))
        return name;
    QString result;
    result.reserve(name.size() + 2);
    result += QChar(first_strong_isolate);
    result += name;
    result += QChar(pop_directional_isolate);
    return result;
}

QString isolate_name(const QString& name)
{
    return isolate_name(name,
                        QGuiApplication::layoutDirection() == Qt::RightToLeft);
}

QString strip_isolates(const QString& text)
{
    QString result = text;
    // U+2066 to U+2069: the isolates (LRI, RLI, FSI, PDI).
    result.removeIf([](QChar c)
                    { return c.unicode() >= 0x2066 && c.unicode() <= 0x2069; });
    return result;
}

void keep_left_to_right(QWidget* widget)
{
    if (widget != nullptr)
        widget->setLayoutDirection(Qt::LeftToRight);
}

} // namespace vss::gui
