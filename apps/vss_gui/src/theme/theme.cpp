//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "theme/theme.hpp"

#include "theme/focus_ring_style.hpp"

#include <vss/core/profile.hpp>

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScreen>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

// Q_INIT_RESOURCE declares the generated symbol in the scope it is used in,
// so this wrapper has to be at global scope, not in an unnamed namespace.
// NOLINTNEXTLINE(misc-use-anonymous-namespace)
static void init_gui_theme_resources()
{
    Q_INIT_RESOURCE(theme_resources);
}

namespace vss::gui
{
namespace
{

QString read_resource(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(
            ("Theme resource is missing: " + path).toStdString());
    return QString::fromUtf8(file.readAll());
}

void flatten_layout(const QString& prefix, const QJsonObject& object,
                    QHash<QString, int>& integers, QHash<QString, QSize>& pairs)
{
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        const QString key = prefix + QLatin1Char('.') + it.key();
        const QJsonValue value = it.value();
        if (value.isObject())
        {
            flatten_layout(key, value.toObject(), integers, pairs);
        }
        else if (value.isArray())
        {
            const QJsonArray array = value.toArray();
            if (array.size() >= 2 && array.at(0).isDouble() &&
                array.at(1).isDouble())
            {
                pairs.insert(key,
                             QSize(array.at(0).toInt(), array.at(1).toInt()));
            }
            else if (array.size() == 1 && array.at(0).isDouble())
            {
                integers.insert(key, array.at(0).toInt());
            }
        }
        else if (value.isDouble())
        {
            integers.insert(key, value.toInt());
        }
    }
}

/// The design's body size in logical pixels.  The system font on Windows is
/// 9 pt, which is 12 px and below the 12 px floor once the smaller styles are
/// derived from it, so the body never goes under the design size.
constexpr double kDesignBodyPx = 13.0;

double logical_dpi()
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (screen != nullptr && screen->logicalDotsPerInchY() > 0.0)
        return screen->logicalDotsPerInchY();
    return 96.0;
}

/// The faces that ship inside the program (assets/fonts, embedded through
/// theme_resources.qrc).  Registered once, before the base fonts are resolved.
struct BundledFonts
{
    /// The byte-view face.  Empty when it could not be loaded.
    QString mono;
    /// Noto Sans Hebrew and Noto Sans Arabic, in the order they are tried for
    /// glyphs the system UI font does not have.
    QStringList script_fallbacks;
};

const BundledFonts& bundled_fonts()
{
    static const BundledFonts fonts = []
    {
        BundledFonts result;
        const auto add = [](const char* file) -> QStringList
        {
            const int id = QFontDatabase::addApplicationFont(
                QStringLiteral(":/fonts/") + QLatin1String(file));
            return id < 0 ? QStringList{}
                          : QFontDatabase::applicationFontFamilies(id);
        };
        const QStringList mono = add("IBMPlexMono-Regular.ttf");
        if (!mono.isEmpty())
            result.mono = mono.front();
        // The system fonts that cover Hebrew and Arabic stay first; these
        // are what Linux without font packages, and Yiddish, fall back to.
        for (const char* file :
             {"NotoSansHebrew-Regular.ttf", "NotoSansHebrew-SemiBold.ttf",
              "NotoSansArabic-Regular.ttf", "NotoSansArabic-Bold.ttf"})
        {
            for (const QString& family : add(file))
            {
                if (!result.script_fallbacks.contains(family))
                    result.script_fallbacks.append(family);
            }
        }
        return result;
    }();
    return fonts;
}

/// The family is whatever the system reports for its UI font.  Segoe UI
/// Variable would be the Windows 11 face, but Qt 6 draws its variable
/// instances at the wrong weight through DirectWrite (every regular label
/// comes out semi-bold), so Windows keeps Segoe UI.  The look comes from the
/// whole-pixel sizes and the unhinted outline instead.  A Hebrew or Arabic
/// glyph the system face lacks is drawn with the bundled Noto face.
QFont resolve_base_font()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    QStringList families{font.family()};
    families.append(bundled_fonts().script_fallbacks);
    font.setFamilies(families);
    // Hinting snaps stems to whole pixels, which is what makes Windows text
    // look crunchy next to a modern UI.  Positions and sizes are already whole
    // pixels (see Theme::font), so ask for the unhinted outline.
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

/// Hex and byte columns use the bundled IBM Plex Mono, so they line up the
/// same on every platform.  The system fixed font (and the Hebrew and Arabic
/// faces after it) only cover glyphs Plex Mono lacks, or the whole job if the
/// resource could not be loaded.
QFont resolve_mono_font()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QStringList families;
    if (!bundled_fonts().mono.isEmpty())
        families.append(bundled_fonts().mono);
    families.append(font.family());
    families.append(bundled_fonts().script_fallbacks);
    font.setFamilies(families);
    font.setFamily(families.front());
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

} // namespace

Theme& Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme::Theme(QObject* parent) : QObject(parent)
{
    init_gui_theme_resources();
    load_tokens();
    if (QGuiApplication::instance() != nullptr)
    {
        base_font_ = resolve_base_font();
        mono_font_ = resolve_mono_font();
    }
}

void Theme::initialise()
{
    VSS_PROFILE_ZONE("gui.theme.initialise");
    if (ready_)
        return;

    if (QGuiApplication::instance() != nullptr)
    {
        base_font_ = resolve_base_font();
        mono_font_ = resolve_mono_font();
    }

    if (!system_connected_ && QGuiApplication::styleHints() != nullptr)
    {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this,
                [this](Qt::ColorScheme)
                {
                    if (mode_ == theme_mode::system)
                        apply_palette_and_style();
                });
        system_connected_ = true;
    }

    ready_ = true;
    apply_palette_and_style();
}

void Theme::set_mode(theme_mode value)
{
    if (!ready_)
        initialise();
    if (mode_ == value)
        return;
    mode_ = value;
    VSS_PROFILE_MESSAGE("gui.theme.mode_changed");
    apply_palette_and_style();
}

void Theme::set_density(density_mode value)
{
    if (density_ == value)
        return;
    density_ = value;
    VSS_PROFILE_MESSAGE("gui.theme.density_changed");
    apply_palette_and_style();
}

bool Theme::is_dark() const
{
    if (mode_ == theme_mode::dark)
        return true;
    if (mode_ == theme_mode::light)
        return false;

    if (QGuiApplication::styleHints() != nullptr)
    {
        const Qt::ColorScheme scheme =
            QGuiApplication::styleHints()->colorScheme();
        if (scheme == Qt::ColorScheme::Dark)
            return true;
        if (scheme == Qt::ColorScheme::Light)
            return false;
    }

    if (qApp != nullptr)
        return qApp->palette().color(QPalette::Window).lightness() < 128;
    return false;
}

bool Theme::reduced_motion() const
{
    const QByteArray setting = qgetenv("VSS_REDUCE_MOTION");
    if (!setting.isEmpty())
    {
        const QByteArray lowered = setting.toLower();
        return lowered != "0" && lowered != "false" && lowered != "off";
    }

#if defined(Q_OS_WIN)
    BOOL animations = TRUE;
    if (::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0))
        return animations == FALSE;
#endif
    return false;
}

int Theme::motion_ms(motion_kind kind) const
{
    if (reduced_motion())
        return 0;
    switch (kind)
    {
    case motion_kind::hover:
        return hover_ms_;
    case motion_kind::disclosure:
        return disclosure_ms_;
    case motion_kind::dialog:
        return dialog_ms_;
    }
    return 0;
}

QColor Theme::colour(colour_role role) const
{
    return colour(role_name(role));
}

QColor Theme::colour(const QString& name) const
{
    const auto& colours = current_colours();
    const auto it = colours.find(name);
    if (it == colours.end())
    {
        qWarning("Theme: unknown colour role \"%s\"",
                 name.toUtf8().constData());
        return {};
    }
    return it.value();
}

bool Theme::has_colour(const QString& name) const
{
    return light_.contains(name) && dark_.contains(name);
}

QFont Theme::font(text_style style) const
{
    const QString key = style_name(style);
    const bool fixed =
        style == text_style::portName || style == text_style::data;
    QFont font = fixed ? mono_font_ : base_font_;
    if (font.family().isEmpty())
        font = QApplication::font();

    // Every style lands on a whole number of logical pixels.  A fractional
    // size (the system's 9 pt body times 0.923 is 11.08 px) is rasterised at
    // a size no stem lines up with, which is the soft, uneven look.  The
    // system size still scales the whole scale up when the user asks for
    // larger text.
    double system_px = font.pixelSize();
    if (system_px <= 0.0)
    {
        const double points = font.pointSizeF() > 0 ? font.pointSizeF() : 9.0;
        system_px = points * logical_dpi() / 72.0;
    }
    const double base_px = std::max(kDesignBodyPx, std::round(system_px));
    const double ratio = ratios_.value(key, 1.0);
    const double px = std::max(1.0, std::round(base_px * ratio));
    font.setPointSizeF(px * 72.0 / logical_dpi());

    QString weight_key = QStringLiteral("body");
    // Caption is the 12 px size used for hints and secondary lines; only the
    // label style carries the semi-bold weight.
    if (style == text_style::label)
        weight_key = QStringLiteral("label");
    else if (style == text_style::subtitle || style == text_style::title ||
             style == text_style::display)
        weight_key = QStringLiteral("title");
    else if (style == text_style::portName)
        weight_key = QStringLiteral("portName");
    else if (style == text_style::heading || style == text_style::group)
        weight_key = key;
    font.setWeight(static_cast<QFont::Weight>(weights_.value(
        weight_key, weights_.value(QStringLiteral("body"), 400))));
    if (style == text_style::group)
    {
        // Group labels are small capitals with a little tracking, so a short
        // label still reads as a heading at 12 px.
        font.setCapitalization(QFont::AllUppercase);
        font.setLetterSpacing(QFont::PercentageSpacing, 106.0);
    }
    return font;
}

int Theme::metric(const QString& name) const
{
    if (sizes_.contains(name))
        return sizes_.value(name);
    if (layout_.contains(name))
        return layout_.value(name);
    if (radii_.contains(name))
        return radii_.value(name);
    return 0;
}

int Theme::space(int index) const
{
    if (index < 0 || index >= spacing_.size())
        return 0;
    return spacing_.at(index);
}

int Theme::radius(const QString& name) const
{
    return radii_.value(name, 0);
}

int Theme::row_height() const
{
    return density_ == density_mode::compact ? row_compact_ : row_comfortable_;
}

QSize Theme::size(const QString& name) const
{
    if (size_pairs_.contains(name))
        return size_pairs_.value(name);
    if (layout_pairs_.contains(name))
        return layout_pairs_.value(name);
    const int value = metric(name);
    return QSize(value, value);
}

QPalette Theme::palette() const
{
    QPalette value;
    // Window is the pane surface: anything that fills its own background (a
    // scroll area viewport, a page) lands on the pane, and only the main
    // window paints the ground, from the style sheet.
    value.setColor(QPalette::Window, colour(colour_role::panel));
    value.setColor(QPalette::WindowText, colour(colour_role::ink));
    value.setColor(QPalette::Base, colour(colour_role::panel));
    value.setColor(QPalette::AlternateBase, colour(colour_role::panel));
    value.setColor(QPalette::ToolTipBase, colour(colour_role::toastBg));
    value.setColor(QPalette::ToolTipText, colour(colour_role::toastText));
    value.setColor(QPalette::Text, colour(colour_role::ink));
    value.setColor(QPalette::PlaceholderText, colour(colour_role::ink3));
    value.setColor(QPalette::Button, colour(colour_role::panel));
    value.setColor(QPalette::ButtonText, colour(colour_role::ink));
    value.setColor(QPalette::BrightText, colour(colour_role::bad));
    value.setColor(QPalette::Link, colour(colour_role::accentStrong));
    value.setColor(QPalette::Highlight, colour(colour_role::accent));
    value.setColor(QPalette::HighlightedText, colour(colour_role::onAccent));
    value.setColor(QPalette::Light, colour(colour_role::panel));
    value.setColor(QPalette::Midlight, colour(colour_role::sunken));
    value.setColor(QPalette::Mid, colour(colour_role::line2));
    value.setColor(QPalette::Dark, colour(colour_role::line));
    value.setColor(QPalette::Shadow, colour(colour_role::ground));

    const QColor disabled = colour(colour_role::disabledText);
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
          QPalette::PlaceholderText, QPalette::ToolTipText})
    {
        value.setColor(QPalette::Disabled, role, disabled);
    }
    return value;
}

QString Theme::style_sheet() const
{
    VSS_PROFILE_ZONE("gui.theme.style_sheet");
    const QString source =
        read_resource(QStringLiteral(":/tokens/style.qss.in"));
    const auto& colours = current_colours();
    const QRegularExpression pattern(QStringLiteral("%([A-Za-z0-9_.]+)%"));
    auto matches = pattern.globalMatch(source);

    QString output;
    output.reserve(source.size() + 1024);
    qsizetype last = 0;
    while (matches.hasNext())
    {
        const QRegularExpressionMatch match = matches.next();
        output += source.mid(last, match.capturedStart() - last);
        const QString name = match.captured(1);
        // Sizes come from the same token file: `%radius.control%` is the
        // control radius and `%space.2%` the third step of the spacing scale,
        // both in pixels.
        // `%theme%` names the resolved theme so a path can pick the light or
        // dark copy of an indicator image.
        if (name == QLatin1String("theme"))
        {
            output +=
                is_dark() ? QLatin1String("dark") : QLatin1String("light");
            last = match.capturedEnd();
            continue;
        }
        if (name.startsWith(QLatin1String("radius.")))
        {
            output +=
                QString::number(radius(name.mid(7))) + QLatin1String("px");
            last = match.capturedEnd();
            continue;
        }
        if (name.startsWith(QLatin1String("space.")))
        {
            output += QString::number(space(name.mid(6).toInt())) +
                      QLatin1String("px");
            last = match.capturedEnd();
            continue;
        }
        const auto it = colours.find(name);
        if (it == colours.end())
            throw std::runtime_error(
                ("Unknown token in style.qss.in: " + name).toStdString());
        output += it.value().name(QColor::HexRgb);
        last = match.capturedEnd();
    }
    output += source.mid(last);
    return output;
}

void Theme::apply()
{
    apply_palette_and_style();
}

void Theme::apply_palette_and_style()
{
    VSS_PROFILE_ZONE("gui.theme.apply");
    if (qApp == nullptr)
        return;

    if (!style_installed_)
    {
        auto* style =
            new FocusRingStyle(QStyleFactory::create(QStringLiteral("Fusion")));
        qApp->setStyle(style);
        style->install();
        style_installed_ = true;
    }

    if (high_contrast())
    {
        qApp->setPalette(QApplication::style()->standardPalette());
        qApp->setStyleSheet(QString());
        Q_EMIT theme_changed();
        return;
    }

    qApp->setPalette(palette());
    // Widgets that do not ask for a style (menus, tooltips, item views,
    // dialog buttons) take the application font; make it the body style so
    // they match the rest of the window instead of the platform's 9 pt.
    qApp->setFont(font(text_style::body));
    qApp->setStyleSheet(style_sheet());
    Q_EMIT theme_changed();
}

bool Theme::high_contrast() const
{
    return qEnvironmentVariableIsSet("VSS_HIGH_CONTRAST");
}

const QHash<QString, QColor>& Theme::current_colours() const
{
    return is_dark() ? dark_ : light_;
}

QString Theme::role_name(colour_role role)
{
    switch (role)
    {
#define VSS_THEME_ROLE(role_name_value)                                        \
    case colour_role::role_name_value:                                         \
        return QStringLiteral(#role_name_value)
        VSS_THEME_ROLE(ground);
        VSS_THEME_ROLE(panel);
        VSS_THEME_ROLE(sunken);
        VSS_THEME_ROLE(hover);
        VSS_THEME_ROLE(pressed);
        VSS_THEME_ROLE(line);
        VSS_THEME_ROLE(line2);
        VSS_THEME_ROLE(control);
        VSS_THEME_ROLE(ink);
        VSS_THEME_ROLE(ink2);
        VSS_THEME_ROLE(ink3);
        VSS_THEME_ROLE(accent);
        VSS_THEME_ROLE(accentStrong);
        VSS_THEME_ROLE(accentSoft);
        VSS_THEME_ROLE(accentLine);
        VSS_THEME_ROLE(amber);
        VSS_THEME_ROLE(amberSoft);
        VSS_THEME_ROLE(amberLine);
        VSS_THEME_ROLE(ok);
        VSS_THEME_ROLE(okSoft);
        VSS_THEME_ROLE(okLine);
        VSS_THEME_ROLE(bad);
        VSS_THEME_ROLE(badSoft);
        VSS_THEME_ROLE(badLine);
        VSS_THEME_ROLE(focus);
        VSS_THEME_ROLE(onAccent);
        VSS_THEME_ROLE(onDanger);
        VSS_THEME_ROLE(dangerBorder);
        VSS_THEME_ROLE(disabledText);
        VSS_THEME_ROLE(trackOff);
        VSS_THEME_ROLE(noteWarnText);
        VSS_THEME_ROLE(noteInfoText);
        VSS_THEME_ROLE(noteOkText);
        VSS_THEME_ROLE(noteDangerText);
        VSS_THEME_ROLE(toastBg);
        VSS_THEME_ROLE(toastText);
        VSS_THEME_ROLE(toastLink);
        VSS_THEME_ROLE(fill);
        VSS_THEME_ROLE(fillHover);
        VSS_THEME_ROLE(accentText);
        VSS_THEME_ROLE(violet);
        VSS_THEME_ROLE(violetSoft);
        VSS_THEME_ROLE(accentHover);
#undef VSS_THEME_ROLE
    }
    return {};
}

QString Theme::style_name(text_style style)
{
    switch (style)
    {
    case text_style::caption:
        return QStringLiteral("caption");
    case text_style::label:
        return QStringLiteral("label");
    case text_style::body:
        return QStringLiteral("body");
    case text_style::bodyLarge:
        return QStringLiteral("bodyLarge");
    case text_style::subtitle:
        return QStringLiteral("subtitle");
    case text_style::title:
        return QStringLiteral("title");
    case text_style::display:
        return QStringLiteral("display");
    case text_style::portName:
        return QStringLiteral("portName");
    case text_style::data:
        return QStringLiteral("data");
    case text_style::heading:
        return QStringLiteral("heading");
    case text_style::group:
        return QStringLiteral("group");
    }
    return QStringLiteral("body");
}

void Theme::load_tokens()
{
    VSS_PROFILE_ZONE("gui.theme.load_tokens");
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(
        read_resource(QStringLiteral(":/tokens/tokens.json")).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError)
        throw std::runtime_error(
            ("tokens.json is not valid JSON: " + error.errorString())
                .toStdString());
    const QJsonObject root = document.object();

    const QJsonObject colours = root.value(QStringLiteral("color")).toObject();
    const auto load_colours = [](const QJsonObject& source)
    {
        QHash<QString, QColor> out;
        for (auto it = source.begin(); it != source.end(); ++it)
        {
            const QColor value(it.value().toString());
            if (!value.isValid())
                throw std::runtime_error(
                    ("Invalid colour token: " + it.key()).toStdString());
            out.insert(it.key(), value);
        }
        return out;
    };
    light_ = load_colours(colours.value(QStringLiteral("light")).toObject());
    dark_ = load_colours(colours.value(QStringLiteral("dark")).toObject());
    colour_names_ = light_.keys();
    std::sort(colour_names_.begin(), colour_names_.end());

    const QJsonObject sizes = root.value(QStringLiteral("size")).toObject();
    for (auto it = sizes.begin(); it != sizes.end(); ++it)
    {
        if (it.value().isDouble())
        {
            sizes_.insert(it.key(), it.value().toInt());
        }
        else if (it.value().isArray())
        {
            const QJsonArray array = it.value().toArray();
            if (array.size() >= 2)
                size_pairs_.insert(
                    it.key(), QSize(array.at(0).toInt(), array.at(1).toInt()));
        }
    }

    const QJsonObject layout = root.value(QStringLiteral("layout")).toObject();
    flatten_layout(QStringLiteral("layout"), layout, layout_, layout_pairs_);

    const QJsonObject radii = root.value(QStringLiteral("radius")).toObject();
    for (auto it = radii.begin(); it != radii.end(); ++it)
        radii_.insert(it.key(), it.value().toInt());

    const QJsonArray space = root.value(QStringLiteral("space")).toArray();
    spacing_.reserve(space.size());
    for (const QJsonValue value : space)
        spacing_.push_back(value.toInt());

    const QJsonObject type = root.value(QStringLiteral("type")).toObject();
    const QJsonObject ratios =
        type.value(QStringLiteral("scaleRatios")).toObject();
    for (auto it = ratios.begin(); it != ratios.end(); ++it)
        ratios_.insert(it.key(), it.value().toDouble());
    const QJsonObject weights =
        type.value(QStringLiteral("weights")).toObject();
    for (auto it = weights.begin(); it != weights.end(); ++it)
        weights_.insert(it.key(), it.value().toInt());

    const QJsonObject motion =
        root.value(QStringLiteral("motionMs")).toObject();
    hover_ms_ = motion.value(QStringLiteral("hover")).toInt(hover_ms_);
    disclosure_ms_ =
        motion.value(QStringLiteral("disclosure")).toInt(disclosure_ms_);
    dialog_ms_ = motion.value(QStringLiteral("dialog")).toInt(dialog_ms_);

    const QJsonObject density =
        root.value(QStringLiteral("density")).toObject();
    row_comfortable_ = density.value(QStringLiteral("comfortable"))
                           .toObject()
                           .value(QStringLiteral("rowHeight"))
                           .toInt(row_comfortable_);
    row_compact_ = density.value(QStringLiteral("compact"))
                       .toObject()
                       .value(QStringLiteral("rowHeight"))
                       .toInt(row_compact_);
}

} // namespace vss::gui
