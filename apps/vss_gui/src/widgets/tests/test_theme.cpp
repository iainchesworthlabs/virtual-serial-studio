//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"

#include <QApplication>
#include <QColor>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <QRegularExpression>
#include <QStringList>
#include <QTableWidget>
#include <QTreeWidget>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace
{

double channel(double value)
{
    value /= 255.0;
    return value <= 0.03928 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& colour)
{
    return 0.2126 * channel(colour.red()) + 0.7152 * channel(colour.green()) +
           0.0722 * channel(colour.blue());
}

double contrast(const QColor& first, const QColor& second)
{
    const double a = luminance(first);
    const double b = luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

void require_contrast(const QString& first, const QString& second, double need)
{
    const vss::gui::Theme& theme = vss::gui::Theme::instance();
    const double ratio = contrast(theme.colour(first), theme.colour(second));
    if (ratio + 0.0001 >= need)
        return;
    std::printf("contrast %s on %s is %.2f, need %.1f\n",
                first.toUtf8().constData(), second.toUtf8().constData(), ratio,
                need);
    REQUIRE(false);
}

} // namespace

TEST_CASE("theme: resolves tokens in both themes")
{
    auto& theme = vss::gui::Theme::instance();
    theme.set_mode(vss::gui::theme_mode::light);
    REQUIRE_EQ(theme.colour(QStringLiteral("ink")), QColor(0x14, 0x17, 0x1C));
    REQUIRE_EQ(theme.colour(QStringLiteral("panel")), QColor(0xFF, 0xFF, 0xFF));
    REQUIRE(theme.colour_names().size() >= 30);

    theme.set_mode(vss::gui::theme_mode::dark);
    REQUIRE_EQ(theme.colour(QStringLiteral("ink")), QColor(0xE6, 0xE8, 0xEC));
    REQUIRE_EQ(theme.colour(QStringLiteral("ground")),
               QColor(0x14, 0x16, 0x1A));
    REQUIRE(theme.has_colour(QStringLiteral("accent")));
    REQUIRE(!theme.has_colour(QStringLiteral("not-a-token")));
}

TEST_CASE("theme: every QSS placeholder resolves in both themes")
{
    auto& theme = vss::gui::Theme::instance();
    QFile file(QStringLiteral(":/tokens/style.qss.in"));
    REQUIRE(file.open(QIODevice::ReadOnly));

    const QString source = QString::fromUtf8(file.readAll());
    const QRegularExpression pattern(QStringLiteral("%([A-Za-z0-9_.]+)%"));
    auto matches = pattern.globalMatch(source);
    int count = 0;
    while (matches.hasNext())
    {
        const auto match = matches.next();
        const QString name = match.captured(1);
        // Size and theme placeholders resolve to a number or a name, not to
        // a colour.
        if (name == QLatin1String("theme") ||
            name.startsWith(QLatin1String("space.")))
        {
            ++count;
            continue;
        }
        if (name.startsWith(QLatin1String("radius.")))
        {
            REQUIRE(theme.radius(name.mid(7)) > 0);
            ++count;
            continue;
        }
        REQUIRE(theme.has_colour(name));
        ++count;
    }
    REQUIRE(count >= 30);

    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        theme.set_mode(mode);
        const QString sheet = theme.style_sheet();
        REQUIRE(!sheet.isEmpty());
        REQUIRE(!sheet.contains(QLatin1Char('%')));
    }
}

TEST_CASE("theme: contrast pairs meet WCAG 2.2 AA")
{
    auto& theme = vss::gui::Theme::instance();
    const auto text_pairs =
        [](const QString& first, const QString& second, double need)
    { require_contrast(first, second, need); };

    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        theme.set_mode(mode);
        text_pairs(QStringLiteral("ink"), QStringLiteral("panel"), 4.5);
        text_pairs(QStringLiteral("ink"), QStringLiteral("ground"), 4.5);
        text_pairs(QStringLiteral("ink"), QStringLiteral("sunken"), 4.5);
        text_pairs(QStringLiteral("ink"), QStringLiteral("accentSoft"), 4.5);
        text_pairs(QStringLiteral("ink2"), QStringLiteral("panel"), 4.5);
        text_pairs(QStringLiteral("ink3"), QStringLiteral("ground"), 4.5);
        text_pairs(QStringLiteral("accentStrong"), QStringLiteral("panel"),
                   4.5);
        text_pairs(QStringLiteral("onAccent"), QStringLiteral("accent"), 4.5);
        text_pairs(QStringLiteral("onDanger"), QStringLiteral("bad"), 4.5);
        text_pairs(QStringLiteral("ok"), QStringLiteral("okSoft"), 4.5);
        text_pairs(QStringLiteral("amber"), QStringLiteral("amberSoft"), 4.5);
        text_pairs(QStringLiteral("bad"), QStringLiteral("badSoft"), 4.5);
        text_pairs(QStringLiteral("toastText"), QStringLiteral("toastBg"), 4.5);
        text_pairs(QStringLiteral("toastLink"), QStringLiteral("toastBg"), 4.5);
        text_pairs(QStringLiteral("noteInfoText"), QStringLiteral("accentSoft"),
                   4.5);
        text_pairs(QStringLiteral("noteOkText"), QStringLiteral("okSoft"), 4.5);
        text_pairs(QStringLiteral("noteWarnText"), QStringLiteral("amberSoft"),
                   4.5);
        text_pairs(QStringLiteral("noteDangerText"), QStringLiteral("badSoft"),
                   4.5);
        text_pairs(QStringLiteral("control"), QStringLiteral("panel"), 3.0);
        text_pairs(QStringLiteral("control"), QStringLiteral("ground"), 3.0);
        text_pairs(QStringLiteral("focus"), QStringLiteral("panel"), 3.0);
        text_pairs(QStringLiteral("focus"), QStringLiteral("ground"), 3.0);
        text_pairs(QStringLiteral("trackOff"), QStringLiteral("panel"), 3.0);
    }
}

TEST_CASE("theme: no hard-coded colours or fonts outside Theme")
{
    const QString root = QStringLiteral(VSS_SOURCE_DIR "/apps/vss_gui/src");
    const QRegularExpression literal(QStringLiteral("#[0-9a-fA-F]{6}"));
    const QRegularExpression font_literal(QStringLiteral(
        "setFontFamily|setPointSize|setPixelSize|font-family\\s*:|"
        "QFont\\s*\\(\\s*QStringLiteral"));
    QStringList offenders;
    QStringList font_offenders;

    QDirIterator iterator(root,
                          {QStringLiteral("*.cpp"), QStringLiteral("*.hpp")},
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const QString path = iterator.next();
        if (path.contains(QStringLiteral("/theme/")) ||
            path.contains(QStringLiteral("/tests/")))
        {
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        if (literal.match(text).hasMatch())
            offenders << path;
        if (font_literal.match(text).hasMatch())
            font_offenders << path;
    }

    for (const auto& path : offenders)
        std::printf("hex colour literal outside Theme: %s\n",
                    path.toUtf8().constData());
    for (const auto& path : font_offenders)
        std::printf("font literal outside Theme: %s\n",
                    path.toUtf8().constData());
    REQUIRE(offenders.isEmpty());
    REQUIRE(font_offenders.isEmpty());
}

TEST_CASE("theme: icon cache flushes on a theme change")
{
    auto& theme = vss::gui::Theme::instance();
    auto& icons = vss::gui::IconProvider::instance();
    theme.set_mode(vss::gui::theme_mode::light);
    icons.flush();
    const QIcon light =
        icons.icon(QStringLiteral("add"), vss::gui::colour_role::ink, 16);
    REQUIRE(!light.isNull());
    REQUIRE(icons.cache_size() > 0);

    theme.set_mode(vss::gui::theme_mode::dark);
    REQUIRE_EQ(icons.cache_size(), 0);

    const QIcon dark =
        icons.icon(QStringLiteral("add"), vss::gui::colour_role::ink, 16);
    REQUIRE(!dark.isNull());
    REQUIRE(icons.cache_size() > 0);
}

TEST_CASE("theme: the application icon is present and draws at every size")
{
    auto& icons = vss::gui::IconProvider::instance();
    const QIcon icon = icons.app_icon();
    REQUIRE(!icon.isNull());
    // The same icon the second time, and unchanged by a theme change: it is a
    // picture in its own colours.
    auto& theme = vss::gui::Theme::instance();
    const QImage before = icon.pixmap(48, 48).toImage();
    theme.set_mode(vss::gui::theme_mode::dark);
    const QImage after = icons.app_icon().pixmap(48, 48).toImage();
    theme.set_mode(vss::gui::theme_mode::light);
    REQUIRE(before == after);
    for (const int side : {16, 24, 32, 48, 64, 128, 256, 512})
    {
        const QImage image = icon.pixmap(side, side).toImage();
        // At a display scale above 100 percent the pixmap has more pixels
        // than `side`.
        REQUIRE(image.width() >= side);
        // Rounded tile: the corner is clear and the middle is not.
        REQUIRE_EQ(qAlpha(image.pixel(0, 0)), 0);
        REQUIRE_EQ(qAlpha(image.pixel(image.width() / 2, image.height() / 2)),
                   255);
    }
}

TEST_CASE("theme: an icon keeps its padding at any display scale")
{
    // At 200 percent the pixmap has twice the pixels. An icon drawn at device
    // pixels instead of logical ones is magnified and cropped, so its strokes
    // run off the edge of the pixmap. This is what clipped the toolbar icons
    // and emptied the launcher tiles. Every icon has padding in its design, so
    // the outer ring of pixels of a correct render is transparent.
    auto& icons = vss::gui::IconProvider::instance();
    icons.flush();
    for (const char* name : {"add", "device", "connect", "save", "search"})
    {
        const QIcon icon = icons.icon(QString::fromLatin1(name),
                                      vss::gui::colour_role::ink, 24);
        REQUIRE(!icon.isNull());
        const QImage image = icon.pixmap(24, 24).toImage();
        int drawn = 0;
        int on_the_edge = 0;
        for (int y = 0; y < image.height(); ++y)
        {
            for (int x = 0; x < image.width(); ++x)
            {
                if (qAlpha(image.pixel(x, y)) <= 32)
                    continue;
                ++drawn;
                if (x == 0 || y == 0 || x == image.width() - 1 ||
                    y == image.height() - 1)
                    ++on_the_edge;
            }
        }
        if (drawn == 0)
            throw vss_test::failure((std::string(name) + " is empty").c_str());
        if (on_the_edge != 0)
            throw vss_test::failure(
                (std::string(name) + " runs off the edge of its pixmap")
                    .c_str());
    }
}

TEST_CASE("theme: fonts, metrics, spacing and density")
{
    auto& theme = vss::gui::Theme::instance();
    REQUIRE(theme.font(vss::gui::text_style::caption).pointSizeF() <
            theme.font(vss::gui::text_style::body).pointSizeF());
    REQUIRE(theme.font(vss::gui::text_style::title).pointSizeF() >
            theme.font(vss::gui::text_style::body).pointSizeF());
    REQUIRE_EQ(theme.metric(QStringLiteral("button")), 32);
    REQUIRE_EQ(theme.metric(QStringLiteral("minTarget")), 24);
    REQUIRE_EQ(theme.metric(QStringLiteral("layout.ports.default")), 300);
    REQUIRE_EQ(theme.space(2), 12);
    REQUIRE_EQ(theme.radius(QStringLiteral("control")), 8);
    REQUIRE_EQ(theme.size(QStringLiteral("toggleTrack")), QSize(34, 18));
    REQUIRE_EQ(theme.size(QStringLiteral("layout.windowMin")), QSize(960, 640));

    theme.set_density(vss::gui::density_mode::comfortable);
    REQUIRE_EQ(theme.row_height(), 36);
    theme.set_density(vss::gui::density_mode::compact);
    REQUIRE_EQ(theme.row_height(), 28);
    theme.set_density(vss::gui::density_mode::comfortable);
}

TEST_CASE("theme: reduced motion override zeroes the durations")
{
    auto& theme = vss::gui::Theme::instance();
    qputenv("VSS_REDUCE_MOTION", QByteArrayLiteral("1"));
    REQUIRE(theme.reduced_motion());
    REQUIRE_EQ(theme.motion_ms(vss::gui::motion_kind::hover), 0);
    REQUIRE_EQ(theme.motion_ms(vss::gui::motion_kind::disclosure), 0);
    REQUIRE_EQ(theme.motion_ms(vss::gui::motion_kind::dialog), 0);
    qunsetenv("VSS_REDUCE_MOTION");
    // The OS setting may legitimately be on; only the override is asserted.
    REQUIRE(theme.motion_ms(vss::gui::motion_kind::hover) >= 0);
}

namespace
{

int focus_pixels(const QImage& image)
{
    const QColor ring =
        vss::gui::Theme::instance().colour(vss::gui::colour_role::focus);
    int count = 0;
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            if (image.pixelColor(x, y) == ring)
                ++count;
        }
    }
    return count;
}

void save_review(QWidget& view, const char* name, const char* mode)
{
    const QByteArray dir = qgetenv("VSS_REVIEW_DIR");
    if (dir.isEmpty())
        return;
    view.grab().save(QStringLiteral("%1/%2-%3.png")
                         .arg(QString::fromUtf8(dir), QString::fromUtf8(name),
                              QString::fromUtf8(mode)));
}

} // namespace

TEST_CASE("theme: item views draw the focus ring on the current item")
{
    auto& theme = vss::gui::Theme::instance();
    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        theme.set_mode(mode);
        const char* mode_name =
            mode == vss::gui::theme_mode::light ? "light" : "dark";

        QListWidget list;
        for (int row = 0; row < 4; ++row)
            list.addItem(QStringLiteral("Setup %1").arg(row));
        list.resize(240, 160);
        list.show();
        list.activateWindow();
        list.setCurrentRow(1);
        QApplication::processEvents();
        list.clearFocus();
        list.setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        REQUIRE(list.hasFocus());
        REQUIRE(vss::gui::FocusRingStyle::keyboard_focus());
        REQUIRE(focus_pixels(list.grab().toImage()) > 20);
        save_review(list, "list", mode_name);

        QTableWidget table(4, 3);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 3; ++column)
                table.setItem(row, column,
                              new QTableWidgetItem(QStringLiteral("r%1c%2")
                                                       .arg(row)
                                                       .arg(column)));
        table.resize(320, 180);
        table.show();
        table.activateWindow();
        table.setCurrentCell(1, 1);
        QApplication::processEvents();
        table.clearFocus();
        table.setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        REQUIRE(focus_pixels(table.grab().toImage()) > 20);
        save_review(table, "table", mode_name);

        QTreeWidget tree;
        tree.setHeaderLabel(QStringLiteral("Name"));
        for (int row = 0; row < 4; ++row)
            tree.addTopLevelItem(new QTreeWidgetItem(
                QStringList{QStringLiteral("Node %1").arg(row)}));
        tree.resize(240, 160);
        tree.show();
        tree.activateWindow();
        tree.setCurrentItem(tree.topLevelItem(1));
        QApplication::processEvents();
        tree.clearFocus();
        tree.setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
        REQUIRE(focus_pixels(tree.grab().toImage()) > 20);
        save_review(tree, "tree", mode_name);
    }
    theme.set_mode(vss::gui::theme_mode::light);
}
