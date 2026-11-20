//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

// Where the keyboard focus ring is drawn, when it shows, and the matching
// geometry of spin boxes.  The ring is found by rendering a control with and
// without focus and comparing the two pictures, so the tests do not depend on
// the ring colour matching a text colour.

#include "test_framework.hpp"

#include "theme/focus_ring_style.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"
#include "widgets/surfaces.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QTabBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace
{

using vss::gui::colour_role;
using vss::gui::Theme;

/// The pixels that differ between a control drawn without and with focus.
struct footprint
{
    QRect bounds;
    int pixels = 0;
    /// Pixels that differ well inside the control, away from its edge.
    int inside = 0;
};

constexpr int kInsideMargin = 8;

/// The widget as it paints itself, on a transparent background.  grab() also
/// fills the corners with the window colour, which hides the rounded shape.
QImage grab_image(QWidget& widget)
{
    QApplication::processEvents();
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    widget.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

footprint compare(const QImage& off, const QImage& on, const QRect& widget_rect)
{
    footprint result;
    const QRect inner = widget_rect.adjusted(kInsideMargin, kInsideMargin,
                                             -kInsideMargin, -kInsideMargin);
    const int width = std::min(off.width(), on.width());
    const int height = std::min(off.height(), on.height());
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (off.pixel(x, y) == on.pixel(x, y))
                continue;
            ++result.pixels;
            result.bounds = result.bounds.isNull()
                                ? QRect(x, y, 1, 1)
                                : result.bounds.united(QRect(x, y, 1, 1));
            if (inner.contains(x, y))
                ++result.inside;
        }
    }
    return result;
}

void send_key(QWidget* widget, int key, Qt::KeyboardModifiers modifiers = {})
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(widget, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

/// A mouse click: the press reaches the application filter, and the focus
/// follows the click as it does for a real one.
void click_with_mouse(QWidget* widget)
{
    const QPointF centre = QRectF(widget->rect()).center();
    QMouseEvent press(QEvent::MouseButtonPress, centre,
                      widget->mapToGlobal(centre), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    if (widget->focusPolicy() != Qt::NoFocus)
        widget->setFocus(Qt::MouseFocusReason);
    QMouseEvent release(QEvent::MouseButtonRelease, centre,
                        widget->mapToGlobal(centre), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
    QApplication::processEvents();
}

/// True when focusing `widget` changes how it is drawn.
bool ring_shown(QWidget& widget)
{
    widget.clearFocus();
    const QImage off = grab_image(widget);
    widget.setFocus(Qt::OtherFocusReason);
    const QImage on = grab_image(widget);
    return compare(off, on, widget.rect()).pixels > 0;
}

struct made_control
{
    /// What goes in the layout.
    QWidget* root = nullptr;
    /// What takes the focus.
    QWidget* focus = nullptr;
};

struct control_case
{
    control_case(const char* case_name,
                 std::function<made_control(QWidget* parent)> factory,
                 bool is_field = false,
                 std::function<QRect(QWidget*)> expected_rect = {},
                 bool is_asserted = true, bool is_bordered = false)
        : name(case_name), make(std::move(factory)), field(is_field),
          expected(std::move(expected_rect)), asserted(is_asserted),
          bordered(is_field || is_bordered)
    {
    }

    const char* name;
    std::function<made_control(QWidget* parent)> make;
    /// Text fields draw a caret inside when focused, and mark focus with a
    /// border, so only the outer bounds are checked for them.
    bool field;
    /// The rectangle the ring has to follow, in the focused widget's
    /// coordinates.  Empty means the whole widget.
    std::function<QRect(QWidget*)> expected;
    /// False for a control that has no focus mark yet.  The case is still
    /// drawn into the review pictures and printed.
    bool asserted;
    /// The control draws its own edge (a field's border, a tile's outline),
    /// which the ring replaces, so that edge is not content.
    bool bordered;
};

made_control simple(QWidget* widget)
{
    return {widget, widget};
}

QPushButton* push_button(const QString& text, const char* role,
                         const char* size, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    if (role != nullptr)
        button->setProperty("role", QString::fromLatin1(role));
    if (size != nullptr)
        button->setProperty("size", QString::fromLatin1(size));
    return button;
}

std::vector<control_case> control_cases()
{
    using vss::gui::IconProvider;
    // One case for each kind of control that can take the keyboard focus.
    // The factory builds the control under a parent and says which widget
    // takes the focus. The optional arguments after it mark a text field, a
    // narrower rectangle for the ring, a case without a focus mark yet, and a
    // control that draws its own edge.
    std::vector<control_case> cases;

    // Push buttons come first, one for each role the style sheet knows.
    const std::pair<const char*, const char*> roles[] = {
        {"push button, secondary", "secondary"},
        {"push button, primary", "primary"},
        {"push button, ghost", "ghost"},
        {"push button, quiet", "quiet"},
        {"push button, danger", "danger"}};
    for (const auto& entry : roles)
    {
        const char* role = entry.second;
        cases.push_back({entry.first, [role](QWidget* parent)
                         {
                             return simple(push_button(QStringLiteral("Cancel"),
                                                       role, nullptr, parent));
                         }});
    }
    // A button with no role, then the small size and the icon only form,
    // whose padding and shape differ from the plain button.
    cases.push_back({"push button, no role", [](QWidget* parent)
                     {
                         return simple(push_button(QStringLiteral("Cancel"),
                                                   nullptr, nullptr, parent));
                     }});
    cases.push_back({"push button, small", [](QWidget* parent)
                     {
                         return simple(push_button(QStringLiteral("Cancel"),
                                                   "primary", "small", parent));
                     }});
    cases.push_back({"push button, icon only", [](QWidget* parent)
                     {
                         auto* button =
                             push_button(QString(), nullptr, "icon", parent);
                         button->setIcon(IconProvider::instance().icon(
                             QStringLiteral("copy"), colour_role::ink, 16));
                         return simple(button);
                     }});
    // Tool buttons, alone and inside a tool bar. In the bar the button takes
    // the focus with the Tab key, and the bar is what goes in the layout.
    cases.push_back({"tool button", [](QWidget* parent)
                     {
                         auto* button = new QToolButton(parent);
                         button->setText(QStringLiteral("Tool"));
                         return simple(button);
                     }});
    cases.push_back({"tool button, in a tool bar", [](QWidget* parent)
                     {
                         auto* bar = new QToolBar(parent);
                         auto* button = new QToolButton(bar);
                         button->setText(QStringLiteral("Tool"));
                         button->setFocusPolicy(Qt::TabFocus);
                         bar->addWidget(button);
                         return made_control{bar, button};
                     }});
    // Choice controls: check box, radio button and the switch.
    cases.push_back(
        {"check box", [](QWidget* parent)
         { return simple(new QCheckBox(QStringLiteral("Echo"), parent)); }});
    cases.push_back(
        {"radio button", [](QWidget* parent)
         { return simple(new QRadioButton(QStringLiteral("Raw"), parent)); }});
    cases.push_back(
        {"switch", [](QWidget* parent)
         {
             return simple(new vss::gui::Switch(QStringLiteral("DTR"), parent));
         }});
    // The segmented control gives the focus to its first segment, a radio
    // button inside it. These controls draw their own edge.
    cases.push_back(
        {"segmented control, segment",
         [](QWidget* parent)
         {
             auto* segments = new vss::gui::SegmentedControl(parent);
             segments->add_item(QStringLiteral("TCP"));
             segments->add_item(QStringLiteral("UDP"));
             return made_control{
                 segments, segments->findChildren<QRadioButton*>().first()};
         },
         false,
         {},
         true,
         true});
    // The tile is fixed in width so that its text wraps the same way on
    // every platform.
    cases.push_back(
        {"action tile",
         [](QWidget* parent)
         {
             auto* tile = new vss::gui::ActionTile(
                 QStringLiteral("copy"), vss::gui::KindTile::tint::accent,
                 QStringLiteral("Create a pair"),
                 QStringLiteral("Two ports joined together."), parent);
             tile->setFixedWidth(250);
             return simple(tile);
         },
         false,
         {},
         true,
         true});
    // Composite controls: the copy button, and a pick card, whose radio
    // button takes the focus.
    cases.push_back({"copy button", [](QWidget* parent)
                     { return simple(new vss::gui::CopyButton(parent)); }});
    cases.push_back({"pick card, radio", [](QWidget* parent)
                     {
                         auto* card = new vss::gui::PickCard(
                             QStringLiteral("Virtual port"),
                             QStringLiteral("A port other programs open."),
                             parent);
                         return made_control{card, card->radio()};
                     }});
    // Fields: they take text or a number, show a caret and mark the focus
    // with the border, so the trailing true marks each as a field.
    cases.push_back({"combo box",
                     [](QWidget* parent)
                     {
                         auto* combo = new QComboBox(parent);
                         combo->addItems({QStringLiteral("9600"),
                                          QStringLiteral("115200")});
                         return simple(combo);
                     },
                     true});
    cases.push_back(
        {"line edit", [](QWidget* parent)
         { return simple(new QLineEdit(QStringLiteral("port-1"), parent)); },
         true});
    // The search form of the line edit is chosen by a property on the widget.
    cases.push_back({"line edit, search",
                     [](QWidget* parent)
                     {
                         auto* edit = new QLineEdit(parent);
                         edit->setProperty("search", true);
                         edit->setPlaceholderText(QStringLiteral("Find"));
                         return simple(edit);
                     },
                     true});
    cases.push_back({"spin box",
                     [](QWidget* parent)
                     {
                         auto* spin = new QSpinBox(parent);
                         spin->setRange(0, 999);
                         spin->setValue(3);
                         return simple(spin);
                     },
                     true});
    cases.push_back({"plain text edit",
                     [](QWidget* parent)
                     {
                         auto* edit = new QPlainTextEdit(parent);
                         edit->setPlainText(QStringLiteral("notes"));
                         edit->setFixedHeight(60);
                         return simple(edit);
                     },
                     true});
    // Controls that are not fields: a slider, a label with a link in it that
    // can be tabbed to, and (below) the current tab of a tab bar.
    cases.push_back({"slider", [](QWidget* parent)
                     {
                         auto* slider = new QSlider(Qt::Horizontal, parent);
                         slider->setFixedWidth(160);
                         return simple(slider);
                     }});
    cases.push_back(
        {"label with a link",
         [](QWidget* parent)
         {
             auto* label = new QLabel(
                 QStringLiteral("Read the <a href=\"x\">manual</a>"), parent);
             label->setTextInteractionFlags(Qt::TextBrowserInteraction);
             return simple(label);
         },
         true});
    cases.push_back(
        {"tab bar, current tab",
         [](QWidget* parent)
         {
             auto* bar = new QTabBar(parent);
             bar->addTab(QStringLiteral("General"));
             bar->addTab(QStringLiteral("Wiring"));
             bar->addTab(QStringLiteral("Timing"));
             return simple(bar);
         },
         false,
         [](QWidget* widget)
         {
             auto* bar = qobject_cast<QTabBar*>(widget);
             // The style sheet gives a tab a 2 px margin; the
             // pill, and so the ring, is inside it.
             return bar->tabRect(bar->currentIndex()).adjusted(2, 2, -2, -2);
         }});
    // The ring is checked against the cases above one by one by the caller.
    return cases;
}

/// A window with the control, and a plain line edit that takes the focus
/// away from it for the unfocused picture.
/// Sets the application's layout direction, as the language setting does,
/// and puts it back.
class direction_guard
{
public:
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

void scale_font(QWidget& widget, int text_scale)
{
    QFont font = widget.font();
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF() * text_scale);
    else
        font.setPixelSize(font.pixelSize() * text_scale);
    widget.setFont(font);
}

struct harness
{
    QWidget host;
    QLineEdit other;
    made_control control;
    QImage off;
    QImage on;

    explicit harness(const control_case& item, int text_scale = 1)
    {
        scale_font(host, text_scale);
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(16);
        other.setParent(&host);
        control = item.make(&host);
        layout->addWidget(control.root, 0, Qt::AlignLeft);
        layout->addWidget(&other);
        host.show();
        host.activateWindow();
        QApplication::processEvents();
        other.setFocus(Qt::TabFocusReason);
        off = grab_image(*control.focus);
        control.focus->setFocus(Qt::TabFocusReason);
        on = grab_image(*control.focus);
    }
};

void print_header()
{
    std::printf("%-30s %-14s %-14s %7s %7s\n", "control", "widget", "ring",
                "pixels", "inside");
}

void print_row(const char* name, const QRect& widget, const footprint& ring)
{
    std::printf("%-30s %4dx%-3d      %3d,%-3d %3dx%-3d %7d %7d\n", name,
                widget.width(), widget.height(), ring.bounds.x(),
                ring.bounds.y(), ring.bounds.width(), ring.bounds.height(),
                ring.pixels, ring.inside);
}

struct review_cell
{
    QString name;
    QImage image;
};

void save_sheet(const std::vector<review_cell>& cells, const char* mode)
{
    const QByteArray dir = qgetenv("VSS_REVIEW_DIR");
    if (dir.isEmpty() || cells.empty())
        return;
    const Theme& theme = Theme::instance();
    constexpr int kScale = 2;
    constexpr int kColumns = 4;
    constexpr int kCellWidth = 300;
    constexpr int kCaption = 28;
    constexpr int kGap = 12;

    // Each row is as tall as its tallest picture.
    const int count = static_cast<int>(cells.size());
    const int rows = (count + kColumns - 1) / kColumns;
    std::vector<QImage> pictures;
    std::vector<int> row_top(static_cast<std::size_t>(rows) + 1, 0);
    for (int index = 0; index < count; ++index)
    {
        QImage image = cells[static_cast<std::size_t>(index)].image.scaled(
            cells[static_cast<std::size_t>(index)].image.size() * kScale,
            Qt::KeepAspectRatio, Qt::FastTransformation);
        if (image.width() > kCellWidth - 16)
            image =
                image.scaledToWidth(kCellWidth - 16, Qt::FastTransformation);
        const int row = index / kColumns;
        row_top[static_cast<std::size_t>(row) + 1] =
            std::max(row_top[static_cast<std::size_t>(row) + 1],
                     kCaption + image.height() + kGap);
        pictures.push_back(std::move(image));
    }
    for (int row = 0; row < rows; ++row)
        row_top[static_cast<std::size_t>(row) + 1] +=
            row_top[static_cast<std::size_t>(row)];

    QImage sheet(kColumns * kCellWidth, row_top.back(), QImage::Format_ARGB32);
    sheet.fill(theme.colour(colour_role::panel));
    QPainter painter(&sheet);
    painter.setPen(theme.colour(colour_role::ink3));
    for (int index = 0; index < count; ++index)
    {
        const int x = (index % kColumns) * kCellWidth;
        const int y = row_top[static_cast<std::size_t>(index / kColumns)];
        painter.drawText(QRect(x + 8, y + 4, kCellWidth - 16, 16),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         cells[static_cast<std::size_t>(index)].name);
        painter.drawImage(x + 8, y + kCaption,
                          pictures[static_cast<std::size_t>(index)]);
    }
    painter.end();
    sheet.save(QStringLiteral("%1/focus-ring-%2.png")
                   .arg(QString::fromUtf8(dir), QString::fromUtf8(mode)));
}

/// How far the ring keeps from the control's content: the 2 px stroke is
/// followed by a gap of `focusRing.offset`, also 2 px.
constexpr int kClearance = 2;
/// The ring and its gap lie within this many pixels of the control's edge.
constexpr int kRingBand = 4;

/// Content pixels of the unfocused control that lie under the ring or within
/// the gap of it.  The ring is what changes when the control takes focus,
/// near its edge.  Content is whatever is drawn that is not the control's own
/// background: the label's glyphs, an indicator, an icon.
bool solid(QRgb pixel)
{
    return qAlpha(pixel) >= 128;
}

int colour_distance(QRgb first, QRgb second)
{
    return std::abs(qRed(first) - qRed(second)) +
           std::abs(qGreen(first) - qGreen(second)) +
           std::abs(qBlue(first) - qBlue(second));
}

int blocked_content(const QImage& off, const QImage& on, const QRect& rect,
                    bool bordered, QRect* where = nullptr)
{
    const int width = std::min(off.width(), on.width());
    const int height = std::min(off.height(), on.height());
    auto edge_distance = [&rect](int x, int y)
    {
        return std::min({x - rect.left(), rect.right() - x, y - rect.top(),
                         rect.bottom() - y});
    };

    // The control's background is its most common opaque colour; a control
    // with none (a check box, a ghost button) has only content.
    std::map<QRgb, int> opaque;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (qAlpha(off.pixel(x, y)) == 255)
                ++opaque[off.pixel(x, y)];
    const bool has_background = !opaque.empty();
    QColor background;
    if (has_background)
    {
        background = QColor::fromRgba(
            std::max_element(opaque.begin(), opaque.end(),
                             [](const auto& first, const auto& second)
                             { return first.second < second.second; })
                ->first);
    }

    // A bordered control marks focus on its own edge, which is two pixels;
    // what changes deeper (a selection, a caret) is not the ring.
    const int band = bordered ? kClearance : kRingBand;
    std::vector<QPoint> ring;
    std::set<std::pair<int, int>> ring_pixels;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (rect.contains(x, y) && edge_distance(x, y) < band &&
                solid(on.pixel(x, y)) &&
                (!solid(off.pixel(x, y)) ||
                 colour_distance(off.pixel(x, y), on.pixel(x, y)) > 60))
            {
                ring.emplace_back(x, y);
                ring_pixels.emplace(x, y);
            }
        }
    }

    const int corner = std::min(rect.height() / 2, 16);
    int blocked = 0;
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const QColor pixel = QColor::fromRgba(off.pixel(x, y));
            // Faint anti-aliasing at the edge of a glyph is not content.
            if (!solid(off.pixel(x, y)) || !rect.contains(x, y))
                continue;
            // A field's border and a tile's outline are the ring's to cover,
            // and so are the arcs of its rounded corners.
            if (bordered &&
                (ring_pixels.count({x, y}) != 0 ||
                 (std::min(x - rect.left(), rect.right() - x) < corner &&
                  std::min(y - rect.top(), rect.bottom() - y) < corner)))
                continue;
            if (has_background)
            {
                const int distance =
                    std::abs(pixel.red() - background.red()) +
                    std::abs(pixel.green() - background.green()) +
                    std::abs(pixel.blue() - background.blue());
                if (distance <= 90)
                    continue;
            }
            for (const QPoint& point : ring)
            {
                const int dx = point.x() - x;
                const int dy = point.y() - y;
                if (dx * dx + dy * dy <= kClearance * kClearance)
                {
                    ++blocked;
                    if (where != nullptr)
                        *where = where->united(QRect(x, y, 1, 1));
                    break;
                }
            }
        }
    }
    return blocked;
}

} // namespace

TEST_CASE("focus ring: clears the content of every control")
{
    auto& theme = Theme::instance();
    std::vector<std::string> failures;
    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        theme.set_mode(mode);
        for (const auto direction : {Qt::LeftToRight, Qt::RightToLeft})
        {
            for (const int text_scale : {1, 2})
            {
                const direction_guard guard(direction);
                for (const control_case& item : control_cases())
                {
                    harness h(item, text_scale);
                    QWidget* focus = h.control.focus;
                    const QRect rect =
                        item.expected ? item.expected(focus) : focus->rect();
                    QRect where(rect.center(), QSize(1, 1));
                    const int blocked = blocked_content(h.off, h.on, rect,
                                                        item.bordered, &where);
                    const bool small =
                        focus->width() < 24 || focus->height() < 24;
                    if (blocked > 0 || small)
                    {
                        std::printf(
                            "%s, %s, %s, %d00%%: %d content pixels under or "
                            "beside the ring (x %d to %d, y %d to %d), "
                            "widget %dx%d\n",
                            item.name,
                            mode == vss::gui::theme_mode::light ? "light"
                                                                : "dark",
                            direction == Qt::LeftToRight ? "ltr" : "rtl",
                            text_scale, blocked, where.left(), where.right(),
                            where.top(), where.bottom(), focus->width(),
                            focus->height());
                        failures.emplace_back(item.name);
                    }
                }
            }
        }
    }
    theme.set_mode(vss::gui::theme_mode::light);
    REQUIRE(failures.empty());
}

TEST_CASE("focus ring: follows the control, not its text, in both themes")
{
    auto& theme = Theme::instance();
    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        theme.set_mode(mode);
        const char* mode_name =
            mode == vss::gui::theme_mode::light ? "light" : "dark";
        std::printf("\nfocus ring geometry, %s\n", mode_name);
        print_header();

        std::vector<review_cell> cells;
        std::vector<std::string> failures;
        for (const control_case& item : control_cases())
        {
            harness h(item);
            QWidget* focus = h.control.focus;
            REQUIRE(focus->hasFocus());
            const QRect expected =
                item.expected ? item.expected(focus) : focus->rect();
            const footprint ring = compare(h.off, h.on, expected);
            print_row(item.name, expected, ring);

            cells.push_back({QString::fromUtf8(item.name), h.on});

            const bool shaped = ring.pixels > 0 && ring.bounds == expected &&
                                (item.field || ring.inside == 0);
            if (!shaped && item.asserted)
                failures.emplace_back(item.name);
        }
        for (const auto& name : failures)
            std::printf("  wrong: %s\n", name.c_str());
        save_sheet(cells, mode_name);
        REQUIRE(failures.empty());
    }
    theme.set_mode(vss::gui::theme_mode::light);
}

TEST_CASE("focus ring: stays through activation, popups and dialogs")
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* first = new QPushButton(QStringLiteral("One"), &window);
    auto* second = new QPushButton(QStringLiteral("Two"), &window);
    layout->addWidget(first);
    layout->addWidget(second);
    window.show();
    window.activateWindow();
    QApplication::processEvents();

    // Tab to the second button: the ring shows.
    first->setFocus(Qt::OtherFocusReason);
    send_key(first, Qt::Key_Tab);
    REQUIRE(second->hasFocus());
    REQUIRE(vss::gui::FocusRingStyle::keyboard_focus());
    REQUIRE(ring_shown(*second));

    // Another window takes the activation and gives it back.
    QWidget other_window;
    other_window.show();
    other_window.activateWindow();
    QApplication::processEvents();
    window.activateWindow();
    QApplication::processEvents();
    for (const auto reason : {Qt::ActiveWindowFocusReason, Qt::PopupFocusReason,
                              Qt::MenuBarFocusReason, Qt::OtherFocusReason})
    {
        QFocusEvent focus_in(QEvent::FocusIn, reason);
        QApplication::sendEvent(second, &focus_in);
    }
    REQUIRE(vss::gui::FocusRingStyle::keyboard_focus());
    REQUIRE(ring_shown(*second));

    // A modifier key alone and a mouse move change nothing.
    send_key(second, Qt::Key_Shift);
    REQUIRE(vss::gui::FocusRingStyle::keyboard_focus());

    // A click hides the ring until the keyboard is used again.
    click_with_mouse(first);
    REQUIRE(first->hasFocus());
    REQUIRE(!vss::gui::FocusRingStyle::keyboard_focus());
    REQUIRE(!ring_shown(*first));
    send_key(first, Qt::Key_Shift);
    REQUIRE(!vss::gui::FocusRingStyle::keyboard_focus());
    send_key(first, Qt::Key_Left);
    REQUIRE(vss::gui::FocusRingStyle::keyboard_focus());
    REQUIRE(ring_shown(*first));

    // A click on something that cannot take focus also hides it.
    auto* label = new QLabel(QStringLiteral("Text"), &window);
    layout->addWidget(label);
    label->show();
    click_with_mouse(label);
    REQUIRE(!vss::gui::FocusRingStyle::keyboard_focus());
}

TEST_CASE("focus ring: a dialog opened from the keyboard shows it")
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* opener = new QPushButton(QStringLiteral("Remove"), &window);
    layout->addWidget(opener);
    window.show();
    window.activateWindow();
    QApplication::processEvents();

    for (const bool keyboard : {true, false})
    {
        if (keyboard)
        {
            opener->setFocus(Qt::TabFocusReason);
            send_key(opener, Qt::Key_Return);
        }
        else
        {
            click_with_mouse(opener);
        }

        vss::gui::ConfirmDialog dialog(QStringLiteral("Remove this setup?"),
                                       QStringLiteral("It cannot be undone."),
                                       QStringLiteral("Remove"),
                                       QStringLiteral("Cancel"), true, &window);
        dialog.show();
        dialog.activateWindow();
        QApplication::processEvents();
        QApplication::processEvents();

        QPushButton* focused = nullptr;
        for (int index = 0; index < dialog.choice_count(); ++index)
        {
            if (dialog.choice_button(index)->hasFocus())
                focused = dialog.choice_button(index);
        }
        REQUIRE(focused != nullptr);
        REQUIRE_EQ(vss::gui::FocusRingStyle::keyboard_focus(), keyboard);
        if (focused != nullptr)
            REQUIRE_EQ(ring_shown(*focused), keyboard);
        dialog.close();
    }
}

namespace
{

/// The distance from `origin`, a column of the control, to the first pixel of
/// text on its right, in pixels.  The control is drawn with a value that is
/// not a space; `skip` is the arrows, which are left out of the search.  A Qt
/// text field keeps its text on the left even in a right-to-left layout, where
/// the arrows are on the left too, so the origin is the arrows' right edge.
int text_inset(QWidget& widget, const QRect& skip, int origin)
{
    const QImage image = grab_image(widget);
    const QColor background = Theme::instance().colour(colour_role::panel);
    const int top = 6;
    const int bottom = image.height() - 6;
    for (int x = std::max(origin, 3); x < image.width() - 3; ++x)
    {
        for (int y = top; y < bottom; ++y)
        {
            if (skip.contains(x, y))
                continue;
            const QColor pixel = image.pixelColor(x, y);
            const int distance = std::abs(pixel.red() - background.red()) +
                                 std::abs(pixel.green() - background.green()) +
                                 std::abs(pixel.blue() - background.blue());
            if (distance > 200)
                return x - origin;
        }
    }
    return -1;
}

QRect spin_buttons(QSpinBox& spin)
{
    QStyleOptionSpinBox option;
    option.initFrom(&spin);
    option.subControls = QStyle::SC_SpinBoxUp | QStyle::SC_SpinBoxDown;
    const QRect up = spin.style()->subControlRect(QStyle::CC_SpinBox, &option,
                                                  QStyle::SC_SpinBoxUp, &spin);
    const QRect down = spin.style()->subControlRect(
        QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, &spin);
    return up.united(down);
}

} // namespace

namespace
{

/// A line edit, a combo box, an editable combo box and a spin box, as the
/// dialogs lay them out, in one window.
struct field_column
{
    QWidget host;
    QLineEdit* edit = nullptr;
    QComboBox* combo = nullptr;
    QComboBox* editable = nullptr;
    QSpinBox* spin = nullptr;

    field_column(const field_column&) = delete;
    field_column& operator=(const field_column&) = delete;

    explicit field_column(int text_scale)
    {
        scale_font(host, text_scale);

        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(16);
        edit = new QLineEdit(QStringLiteral("8"), &host);
        combo = new QComboBox(&host);
        combo->addItem(QStringLiteral("8"));
        editable = new QComboBox(&host);
        editable->setEditable(true);
        editable->addItem(QStringLiteral("8"));
        spin = new QSpinBox(&host);
        spin->setRange(0, 999);
        spin->setValue(8);
        for (QWidget* widget :
             std::initializer_list<QWidget*>{edit, combo, editable, spin})
        {
            widget->setFixedWidth(150 * text_scale);
            layout->addWidget(widget);
        }
        host.show();
        host.activateWindow();
        QApplication::processEvents();
    }
};

QRect combo_arrow(QComboBox& combo)
{
    QStyleOptionComboBox option;
    option.initFrom(&combo);
    option.editable = combo.isEditable();
    option.subControls = QStyle::SC_ComboBoxArrow;
    return combo.style()->subControlRect(QStyle::CC_ComboBox, &option,
                                         QStyle::SC_ComboBoxArrow, &combo);
}

} // namespace

TEST_CASE(
    "spin box: the value is inset like a line edit and the box is as tall")
{
    auto& theme = Theme::instance();
    for (const auto density :
         {vss::gui::density_mode::comfortable, vss::gui::density_mode::compact})
    {
        theme.set_density(density);
        for (const auto mode :
             {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
        {
            theme.set_mode(mode);
            for (const auto direction : {Qt::LeftToRight, Qt::RightToLeft})
            {
                for (const int text_scale : {1, 2})
                {
                    const direction_guard guard(direction);
                    field_column fields(text_scale);
                    QLineEdit& edit = *fields.edit;
                    QSpinBox& spin = *fields.spin;
                    const QRect buttons = spin_buttons(spin);
                    const QRect arrow = combo_arrow(*fields.editable);
                    // Where the text area starts: after the arrows when they
                    // are on the left.
                    const int spin_origin =
                        direction == Qt::RightToLeft ? buttons.right() + 1 : 0;
                    const int combo_origin =
                        direction == Qt::RightToLeft ? arrow.right() + 1 : 0;
                    const int edit_inset = text_inset(edit, {}, 0);
                    const int spin_inset =
                        text_inset(spin, buttons, spin_origin);
                    const int editable_inset =
                        text_inset(*fields.editable, arrow, combo_origin);

                    std::printf(
                        "fields %s %s %s %d00%%: heights edit %d combo %d "
                        "editable %d spin %d, inset edit %d editable %d "
                        "spin %d, arrows %d,%d %dx%d of %d\n",
                        density == vss::gui::density_mode::compact
                            ? "compact"
                            : "comfortable",
                        mode == vss::gui::theme_mode::light ? "light" : "dark",
                        direction == Qt::LeftToRight ? "ltr" : "rtl",
                        text_scale, edit.height(), fields.combo->height(),
                        fields.editable->height(), spin.height(), edit_inset,
                        editable_inset, spin_inset, buttons.x(), buttons.y(),
                        buttons.width(), buttons.height(), spin.width());

                    // A spin box is as tall as the other fields.
                    REQUIRE_EQ(spin.height(), edit.height());
                    REQUIRE_EQ(fields.combo->height(), edit.height());
                    REQUIRE_EQ(fields.editable->height(), edit.height());

                    // The value starts where a line edit's text starts: the
                    // border and the padding, from the leading edge.
                    REQUIRE(edit_inset >= 10);
                    REQUIRE(std::abs(spin_inset - edit_inset) <= 1);
                    // An editable combo box is not what was reported; it is
                    // held to two pixels, since its edit sits in a different
                    // frame.
                    REQUIRE(std::abs(editable_inset - edit_inset) <= 2);

                    // The arrows keep a 22 px column on the trailing edge.
                    REQUIRE_EQ(buttons.width(), 22);
                    if (direction == Qt::LeftToRight)
                        REQUIRE_EQ(buttons.right(), spin.width() - 1);
                    else
                        REQUIRE_EQ(buttons.left(), 0);
                }
            }
        }
    }
    theme.set_density(vss::gui::density_mode::comfortable);
    theme.set_mode(vss::gui::theme_mode::light);
}

TEST_CASE("focus ring: review pictures of the spin boxes")
{
    const QByteArray dir = qgetenv("VSS_REVIEW_DIR");
    if (dir.isEmpty())
        return;
    auto& theme = Theme::instance();
    for (const auto mode :
         {vss::gui::theme_mode::light, vss::gui::theme_mode::dark})
    {
        theme.set_mode(mode);
        std::vector<review_cell> cells;
        for (const auto direction : {Qt::LeftToRight, Qt::RightToLeft})
        {
            const direction_guard guard(direction);
            const QString suffix = direction == Qt::RightToLeft
                                       ? QStringLiteral(", right to left")
                                       : QString();
            field_column fields(1);
            auto* disabled = new QSpinBox(&fields.host);
            disabled->setRange(0, 999);
            disabled->setValue(8);
            disabled->setEnabled(false);
            disabled->setFixedWidth(150);
            fields.host.layout()->addWidget(disabled);
            fields.host.adjustSize();
            QApplication::processEvents();

            fields.edit->setFocus(Qt::TabFocusReason);
            cells.push_back({QStringLiteral("line edit, focused") + suffix,
                             grab_image(*fields.edit)});
            cells.push_back({QStringLiteral("combo box") + suffix,
                             grab_image(*fields.combo)});
            cells.push_back({QStringLiteral("editable combo box") + suffix,
                             grab_image(*fields.editable)});
            cells.push_back({QStringLiteral("spin box") + suffix,
                             grab_image(*fields.spin)});
            fields.spin->setFocus(Qt::TabFocusReason);
            cells.push_back({QStringLiteral("spin box, focused") + suffix,
                             grab_image(*fields.spin)});
            cells.push_back({QStringLiteral("spin box, disabled") + suffix,
                             grab_image(*disabled)});
        }
        theme.set_density(vss::gui::density_mode::compact);
        {
            field_column fields(1);
            cells.push_back({QStringLiteral("spin box, compact"),
                             grab_image(*fields.spin)});
            cells.push_back({QStringLiteral("line edit, compact"),
                             grab_image(*fields.edit)});
        }
        theme.set_density(vss::gui::density_mode::comfortable);
        save_sheet(cells, mode == vss::gui::theme_mode::light ? "spin-light"
                                                              : "spin-dark");
    }
    theme.set_mode(vss::gui::theme_mode::light);
}
