//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_THEME_THEME_HPP
#define VSS_GUI_THEME_THEME_HPP

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QPalette>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vss::gui
{

/// Every colour role in `tokens/tokens.json`.
enum class colour_role
{
    ground,
    panel,
    sunken,
    hover,
    pressed,
    line,
    line2,
    control,
    ink,
    ink2,
    ink3,
    accent,
    accentStrong,
    accentSoft,
    accentLine,
    amber,
    amberSoft,
    amberLine,
    ok,
    okSoft,
    okLine,
    bad,
    badSoft,
    badLine,
    focus,
    onAccent,
    onDanger,
    dangerBorder,
    disabledText,
    trackOff,
    noteWarnText,
    noteInfoText,
    noteOkText,
    noteDangerText,
    toastBg,
    toastText,
    toastLink,
    fill,
    fillHover,
    accentText,
    violet,
    violetSoft,
    accentHover
};

/// The type scale from `02-design-tokens.md`.
enum class text_style
{
    caption,
    label,
    body,
    bodyLarge,
    subtitle,
    title,
    display,
    portName,
    data,
    heading,
    group
};

enum class theme_mode
{
    system,
    light,
    dark
};

enum class density_mode
{
    comfortable,
    compact
};

enum class motion_kind
{
    hover,
    disclosure,
    dialog
};

/// The hinting every font from `Theme` asks for, in one place so it can be
/// changed in one line.  Vertical hinting fits stems to whole pixel rows and
/// keeps the letter widths of unhinted text; `QFont::PreferDefaultHinting`
/// also fits them across, which is the classic (and wider) Windows look.
/// Measured on the `windows` platform at 96 DPI with Segoe UI 13 px, the same
/// sentence has 64 fully inked pixels with `PreferNoHinting` and 263 with
/// vertical hinting.  The offscreen platform does not use the platform's font
/// engine and draws all four settings the same, so judge text on a real
/// window with a screen grab, not on an offscreen picture.
inline constexpr QFont::HintingPreference kTextHinting =
    QFont::PreferVerticalHinting;

/// The single source of colour, type, size and spacing.
///
/// Tokens are compiled into the GUI as `:/tokens/tokens.json` and
/// `:/tokens/style.qss.in`.  Nothing outside this module may name a colour,
/// font family or font size.
class Theme final : public QObject
{
    Q_OBJECT

public:
    static Theme& instance();

    /// Loads the resources and applies the initial palette and style sheet.
    /// Call once, after QApplication exists.
    void initialise();
    bool ready() const noexcept { return ready_; }

    theme_mode mode() const noexcept { return mode_; }
    void set_mode(theme_mode value);
    /// True when the resolved theme is dark.
    bool is_dark() const;

    density_mode density() const noexcept { return density_; }
    void set_density(density_mode value);

    bool reduced_motion() const;
    /// Explicit high-contrast fallback: keep the system palette and drop the
    /// custom stylesheet so the platform's contrast scheme wins.
    bool high_contrast() const;
    int motion_ms(motion_kind kind) const;

    QColor colour(colour_role role) const;
    QColor colour(const QString& name) const;
    bool has_colour(const QString& name) const;
    QStringList colour_names() const { return colour_names_; }

    QFont font(text_style style) const;
    int metric(const QString& name) const;
    int space(int index) const;
    int radius(const QString& name) const;
    int row_height() const;
    QSize size(const QString& name) const;

    QPalette palette() const;
    QString style_sheet() const;
    void apply();

Q_SIGNALS:
    void theme_changed();

private:
    explicit Theme(QObject* parent = nullptr);

    void load_tokens();
    void apply_palette_and_style();
    const QHash<QString, QColor>& current_colours() const;
    static QString role_name(colour_role role);
    static QString style_name(text_style style);

    bool ready_ = false;
    theme_mode mode_ = theme_mode::system;
    density_mode density_ = density_mode::comfortable;
    bool system_connected_ = false;
    bool style_installed_ = false;

    QHash<QString, QColor> light_;
    QHash<QString, QColor> dark_;
    QHash<QString, int> sizes_;
    QHash<QString, QSize> size_pairs_;
    QHash<QString, int> layout_;
    QHash<QString, QSize> layout_pairs_;
    QHash<QString, int> radii_;
    QHash<QString, double> ratios_;
    QHash<QString, int> weights_;
    QVector<int> spacing_;
    QStringList colour_names_;

    int row_comfortable_ = 36;
    int row_compact_ = 28;
    int hover_ms_ = 100;
    int disclosure_ms_ = 150;
    int dialog_ms_ = 200;

    QFont base_font_;
    QFont mono_font_;
};

} // namespace vss::gui

#endif // VSS_GUI_THEME_THEME_HPP
