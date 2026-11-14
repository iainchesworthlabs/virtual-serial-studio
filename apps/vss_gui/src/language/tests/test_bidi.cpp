//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "language/bidi.hpp"
#include "theme/icon_provider.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QLineEdit>
#include <QString>

#include <cstdlib>

namespace
{

/// Puts the layout direction back when the test ends.
struct direction_guard
{
    explicit direction_guard(Qt::LayoutDirection direction)
        : before_(QGuiApplication::layoutDirection())
    {
        QGuiApplication::setLayoutDirection(direction);
    }
    ~direction_guard() { QGuiApplication::setLayoutDirection(before_); }
    direction_guard(const direction_guard&) = delete;
    direction_guard& operator=(const direction_guard&) = delete;

private:
    Qt::LayoutDirection before_;
};

} // namespace

TEST_CASE("bidi: a name is left alone in a left-to-right layout")
{
    const direction_guard guard(Qt::LeftToRight);
    REQUIRE_EQ(vss::gui::isolate_name(QStringLiteral("loopback:logger")),
               QStringLiteral("loopback:logger"));
    REQUIRE_EQ(vss::gui::isolate_name(QStringLiteral("COM3")),
               QStringLiteral("COM3"));
}

TEST_CASE("bidi: a name is wrapped in an isolate in a right-to-left layout")
{
    const direction_guard guard(Qt::RightToLeft);
    const QString wrapped =
        vss::gui::isolate_name(QStringLiteral("loopback:logger"));
    REQUIRE_EQ(wrapped.size(), qsizetype(17));
    REQUIRE_EQ(wrapped.front().unicode(), char16_t(0x2068));
    REQUIRE_EQ(wrapped.back().unicode(), char16_t(0x2069));
    REQUIRE_EQ(wrapped.mid(1, 15), QStringLiteral("loopback:logger"));
}

TEST_CASE("bidi: empty text and text already isolated are not wrapped again")
{
    const direction_guard guard(Qt::RightToLeft);
    REQUIRE(vss::gui::isolate_name(QString()).isEmpty());
    const QString once = vss::gui::isolate_name(QStringLiteral("COM3"));
    REQUIRE_EQ(vss::gui::isolate_name(once), once);
}

TEST_CASE("bidi: the direction can be given")
{
    REQUIRE_EQ(vss::gui::isolate_name(QStringLiteral("COM3"), false),
               QStringLiteral("COM3"));
    REQUIRE_EQ(vss::gui::isolate_name(QStringLiteral("COM3"), true).size(),
               qsizetype(6));
}

TEST_CASE("bidi: strip_isolates undoes isolate_name")
{
    const QString name = QStringLiteral("loopback:console");
    REQUIRE_EQ(vss::gui::strip_isolates(vss::gui::isolate_name(name, true)),
               name);
    REQUIRE_EQ(vss::gui::strip_isolates(name), name);
}

TEST_CASE("bidi: keep_left_to_right sets the direction of one widget")
{
    const direction_guard guard(Qt::RightToLeft);
    QLineEdit mirrored;
    QLineEdit kept;
    vss::gui::keep_left_to_right(&kept);
    vss::gui::keep_left_to_right(nullptr);
    REQUIRE_EQ(mirrored.layoutDirection(), Qt::RightToLeft);
    REQUIRE_EQ(kept.layoutDirection(), Qt::LeftToRight);
}

TEST_CASE("bidi: the icons that show a direction are the ones that mirror")
{
    using vss::gui::IconProvider;
    for (const char* name :
         {"redirect", "split", "join", "share", "loopback", "run"})
        REQUIRE(
            IconProvider::mirrors_in_right_to_left(QString::fromLatin1(name)));
    for (const char* name : {"port", "pair", "connect", "open", "export",
                             "chevron-down", "copy", "add"})
        REQUIRE(!IconProvider::mirrors_in_right_to_left(
            QString::fromLatin1(name)));
}

TEST_CASE("bidi: an arrow icon is flipped in a right-to-left layout")
{
    const QSize size(32, 32);
    const auto draw = [&size](const char* name)
    {
        return vss::gui::IconProvider::instance()
            .icon(QString::fromLatin1(name), QColor(Qt::black))
            .pixmap(size)
            .toImage();
    };
    QImage arrow_ltr;
    QImage arrow_rtl;
    QImage port_ltr;
    QImage port_rtl;
    {
        const direction_guard guard(Qt::LeftToRight);
        arrow_ltr = draw("redirect");
        port_ltr = draw("port");
    }
    {
        const direction_guard guard(Qt::RightToLeft);
        arrow_rtl = draw("redirect");
        port_rtl = draw("port");
    }
    REQUIRE(arrow_ltr != arrow_rtl);
    // The same picture the other way round, to within the smoothing of the
    // edges.
    const QImage flipped = arrow_rtl.flipped(Qt::Horizontal);
    int different = 0;
    for (int y = 0; y < arrow_ltr.height(); ++y)
    {
        for (int x = 0; x < arrow_ltr.width(); ++x)
        {
            if (std::abs(qAlpha(arrow_ltr.pixel(x, y)) -
                         qAlpha(flipped.pixel(x, y))) > 48)
                ++different;
        }
    }
    REQUIRE(different < 12);
    // An object is drawn the same way in both.
    REQUIRE(port_ltr == port_rtl);
}
