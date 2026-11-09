//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/settings_dialog.hpp"

#include "profiling/profiling_controller.hpp"
#include "support/setup_facts.hpp"
#include "support/sign_in_start.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

QLabel* section_title(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = Theme::instance().font(text_style::subtitle);
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QLabel* hint_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    label->setWordWrap(true);
    polish(label);
    return label;
}

QWidget* scroll_page(QWidget* content, QWidget* parent)
{
    auto* scroll = new QScrollArea(parent);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}

} // namespace

SettingsDialog::SettingsDialog(StudioBackend& backend,
                               const ui_settings& current, QWidget* parent)
    : QDialog(parent), backend_(backend), settings_(current)
{
    build();
}

void SettingsDialog::build()
{
    // The dialog is built in four stages: window chrome and header, the
    // section list and the stacked pages, then the signal connections that
    // apply each change at once, and last the footer.
    // Window chrome: accessible name, panel surface and the wide dialog width,
    // which is also the minimum.
    setWindowTitle(QCoreApplication::translate("Settings", "Settings"));
    setAccessibleName(QCoreApplication::translate("Settings", "Settings"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(Theme::instance().metric(QStringLiteral("layout.dialogWidths.wide")),
           760);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.wide")));

    // The outer layout has no margins so the header, content and footer can
    // set their own padding. The header shows the title with the shortcut
    // that opens this dialog.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    auto* title =
        new QLabel(QCoreApplication::translate("Settings", "Settings"), header);
    title->setFont(Theme::instance().font(text_style::title));
    header_layout->addWidget(title, 1);
    auto* shortcut = new QLabel(QStringLiteral("Ctrl+,"), header);
    shortcut->setFont(Theme::instance().font(text_style::caption));
    shortcut->setProperty("muted", true);
    polish(shortcut);
    header_layout->addWidget(shortcut, 0, Qt::AlignVCenter);
    outer->addWidget(header);

    // The content row: the section list on the left, the pages on the right.
    auto* content = new QWidget(this);
    auto* content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    auto* nav = new QFrame(content);
    nav->setObjectName(QStringLiteral("settingsNav"));
    nav->setFixedWidth(200);
    nav->setAttribute(Qt::WA_StyledBackground, true);
    // The section list sits on the dialog surface with no band or rule.  The
    // current section is an inset `accentSoft` pill (rule 9).
    const auto apply_nav_style = [nav]
    {
        const Theme& theme = Theme::instance();
        const auto hex = [&theme](colour_role role)
        { return theme.colour(role).name(QColor::HexRgb); };
        nav->setStyleSheet(
            QStringLiteral(
                "QFrame#settingsNav { background: transparent; border: 0; }"
                "QFrame#settingsNav QPushButton { min-height: %1px; border: 0; "
                "border-radius: %2px; padding: 0 12px; text-align: left; "
                "background: transparent; color: %3; }"
                "QFrame#settingsNav QPushButton:hover:!checked { background: "
                "%4; }"
                "QFrame#settingsNav QPushButton:checked { background: %5; "
                "color: %6; font-weight: 600; }")
                .arg(theme.metric(QStringLiteral("button")))
                .arg(theme.radius(QStringLiteral("inner")))
                .arg(hex(colour_role::ink2), hex(colour_role::hover),
                     hex(colour_role::accentSoft),
                     hex(colour_role::accentText)));
    };
    // The stylesheet bakes in theme colours, so it is applied now and again
    // whenever the theme changes.
    apply_nav_style();
    connect(&Theme::instance(), &Theme::theme_changed, nav,
            [apply_nav_style] { apply_nav_style(); });
    auto* nav_layout = new QVBoxLayout(nav);
    nav_layout->setContentsMargins(16, 4, 0, 16);
    nav_layout->setSpacing(2);
    // One checkable button per section. The button id is the page index in
    // the stack, and the first section starts selected.
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    nav_ = group;
    const QStringList sections = {
        QCoreApplication::translate("Settings", "General"),
        QCoreApplication::translate("Settings", "Running"),
        QCoreApplication::translate("Settings", "Appearance"),
        QCoreApplication::translate("Settings", "Performance")};
    for (int index = 0; index < sections.size(); ++index)
    {
        auto* button = new QPushButton(sections.at(index), nav);
        button->setCheckable(true);
        button->setChecked(index == 0);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(sections.at(index));
        group->addButton(button, index);
        nav_layout->addWidget(button);
    }
    nav_layout->addStretch(1);
    content_layout->addWidget(nav);

    // Pages must be added in the same order as the sections above, since the
    // button id doubles as the page index. Each page is wrapped in a scroll
    // area by scroll_page().
    pages_ = new QStackedWidget(content);
    content_layout->addWidget(pages_, 1);

    // -- General -----------------------------------------------------------
    auto* general = new QWidget(pages_);
    auto* general_layout = new QVBoxLayout(general);
    general_layout->setContentsMargins(24, 4, 24, 16);
    general_layout->setSpacing(14);
    general_layout->addWidget(section_title(
        QCoreApplication::translate("Settings", "General"), general));

    // Advanced mode adds panels to the same window, so its hint says that
    // nothing already built changes.
    advanced_ = new Switch(
        QCoreApplication::translate("Settings", "Advanced mode"), general);
    advanced_->setObjectName(QStringLiteral("advancedSwitch"));
    advanced_->setChecked(settings_.advanced);
    general_layout->addWidget(advanced_);
    auto* advanced_hint = hint_label(
        QCoreApplication::translate(
            "Settings",
            "Adds panels, options and tools to the same window. Nothing you "
            "have built changes."),
        general);
    general_layout->addWidget(advanced_hint);

    // The holder caps the width so the combo box does not stretch across
    // the page. If the saved baud rate is not in the list, the combo box
    // keeps its first entry.
    auto* baud_holder = new QWidget(general);
    baud_holder->setMaximumWidth(320);
    auto* baud_layout = new QVBoxLayout(baud_holder);
    baud_layout->setContentsMargins(0, 0, 0, 0);
    baud_layout->setSpacing(4);
    baud_ = new QComboBox(baud_holder);
    baud_->setObjectName(QStringLiteral("baudCombo"));
    baud_->addItems(baud_rate_choices());
    const int baud_index =
        baud_->findText(QString::number(settings_.default_baud));
    if (baud_index >= 0)
        baud_->setCurrentIndex(baud_index);
    auto* baud_row = new FormRow(
        QCoreApplication::translate("Settings", "Default baud rate"), baud_,
        QCoreApplication::translate("Settings",
                                    "Offered first when you attach a device."),
        baud_holder);
    baud_layout->addWidget(baud_row);
    general_layout->addWidget(baud_holder);

    ask_on_discard_ =
        new QCheckBox(QCoreApplication::translate(
                          "Settings", "Ask before discarding an unsaved setup"),
                      general);
    ask_on_discard_->setObjectName(QStringLiteral("askOnDiscard"));
    ask_on_discard_->setChecked(settings_.ask_on_discard);
    general_layout->addWidget(ask_on_discard_);
    general_layout->addStretch(1);
    pages_->addWidget(scroll_page(general, pages_));

    // -- Running -----------------------------------------------------------
    auto* running = new QWidget(pages_);
    auto* running_layout = new QVBoxLayout(running);
    running_layout->setContentsMargins(22, 22, 28, 22);
    running_layout->setSpacing(14);
    running_layout->addWidget(section_title(
        QCoreApplication::translate("Settings", "Running"), running));
    running_layout->addWidget(hint_label(
        QCoreApplication::translate(
            "Settings",
            "Your ports run on this computer until you quit Studio or open "
            "another setup. The window is only for setting them up and "
            "watching them."),
        running));

    // Closing the window either keeps the ports running in the background
    // or stops them and quits. Exactly one of the radio buttons is checked.
    auto* close_label = new QLabel(
        QCoreApplication::translate("Settings", "When I close the window"),
        running);
    QFont close_bold = Theme::instance().font(text_style::caption);
    close_bold.setWeight(QFont::DemiBold);
    close_label->setFont(close_bold);
    running_layout->addWidget(close_label);
    auto* close_group = new QButtonGroup(this);
    close_group->setExclusive(true);
    close_background_ = new QRadioButton(
        QCoreApplication::translate("Settings",
                                    "Keep the ports running in the background"),
        running);
    close_background_->setObjectName(QStringLiteral("closeBackground"));
    close_quit_ = new QRadioButton(
        QCoreApplication::translate("Settings", "Stop the ports and quit"),
        running);
    close_quit_->setObjectName(QStringLiteral("closeQuit"));
    close_group->addButton(close_background_);
    close_group->addButton(close_quit_);
    close_background_->setChecked(settings_.keep_running_on_close);
    close_quit_->setChecked(!settings_.keep_running_on_close);
    running_layout->addWidget(close_background_);
    running_layout->addWidget(close_quit_);

    // Start at sign-in depends on the build. Where it is not supported the
    // switch is shown off and disabled, with the reason in a tooltip.
    sign_in_ = new Switch(
        QCoreApplication::translate("Settings", "Start when I sign in"),
        running);
    sign_in_->setObjectName(QStringLiteral("signInSwitch"));
    const bool sign_in_available = sign_in_start_available();
    sign_in_->setChecked(sign_in_available && sign_in_start_enabled());
    sign_in_->setEnabled(sign_in_available);
    if (!sign_in_available)
        sign_in_->setToolTip(QCoreApplication::translate(
            "Settings", "Not available in this build"));
    running_layout->addWidget(sign_in_);
    running_layout->addWidget(hint_label(
        QCoreApplication::translate(
            "Settings",
            "Studio starts in the notification area and starts the ports that "
            "were running before the computer restarted."),
        running));
    running_layout->addStretch(1);
    pages_->addWidget(scroll_page(running, pages_));

    // -- Appearance --------------------------------------------------------
    auto* appearance = new QWidget(pages_);
    auto* appearance_layout = new QVBoxLayout(appearance);
    appearance_layout->setContentsMargins(24, 4, 24, 16);
    appearance_layout->setSpacing(14);
    appearance_layout->addWidget(section_title(
        QCoreApplication::translate("Settings", "Appearance"), appearance));

    // Theme: three radio buttons sharing one group. The bold caption font is
    // reused for the other captions on this page.
    auto* theme_label = new QLabel(
        QCoreApplication::translate("Settings", "Theme"), appearance);
    theme_label->setFont(Theme::instance().font(text_style::caption));
    QFont theme_bold = theme_label->font();
    theme_bold.setWeight(QFont::DemiBold);
    theme_label->setFont(theme_bold);
    appearance_layout->addWidget(theme_label);

    // add_theme() makes one radio button. Only the button being switched on
    // acts: it stores the mode and applies it, and the one switching off is
    // ignored so the change is applied once.
    auto* theme_group = new QButtonGroup(this);
    theme_group->setExclusive(true);
    const auto add_theme = [this, appearance, theme_group](const QString& text,
                                                           theme_mode mode,
                                                           bool checked)
    {
        auto* radio = new QRadioButton(text, appearance);
        radio->setChecked(checked);
        theme_group->addButton(radio);
        connect(radio, &QRadioButton::toggled, this,
                [this, mode](bool on)
                {
                    if (!on)
                        return;
                    settings_.theme = mode;
                    apply_theme();
                });
        return radio;
    };
    theme_system_ =
        add_theme(QCoreApplication::translate("Settings", "Follow the system"),
                  theme_mode::system, settings_.theme == theme_mode::system);
    theme_light_ =
        add_theme(QCoreApplication::translate("Settings", "Light"),
                  theme_mode::light, settings_.theme == theme_mode::light);
    theme_dark_ =
        add_theme(QCoreApplication::translate("Settings", "Dark"),
                  theme_mode::dark, settings_.theme == theme_mode::dark);
    for (QRadioButton* radio : {theme_system_, theme_light_, theme_dark_})
        appearance_layout->addWidget(radio);

    // Density: a two-way control, comfortable (id 0) or compact (id 1),
    // applied as soon as it changes.
    auto* density_label = new QLabel(
        QCoreApplication::translate("Settings", "Density"), appearance);
    density_label->setFont(theme_bold);
    appearance_layout->addWidget(density_label);
    density_ = new SegmentedControl(appearance);
    density_->setObjectName(QStringLiteral("densityControl"));
    density_->add_item(QCoreApplication::translate("Settings", "Comfortable"),
                       0);
    density_->add_item(QCoreApplication::translate("Settings", "Compact"), 1);
    density_->set_current(settings_.density == density_mode::compact ? 1 : 0);
    connect(density_, &SegmentedControl::current_changed, this,
            [this](int id)
            {
                settings_.density =
                    id == 1 ? density_mode::compact : density_mode::comfortable;
                apply_density();
            });
    auto* density_holder = new QWidget(appearance);
    density_holder->setMaximumWidth(320);
    auto* density_layout = new QVBoxLayout(density_holder);
    density_layout->setContentsMargins(0, 0, 0, 0);
    density_layout->addWidget(density_);
    appearance_layout->addWidget(density_holder);

    // Link lines: how the canvas draws a link, curved (id 0, the default) or
    // at right angles (id 1).  The canvas redraws as soon as it changes.
    auto* links_label = new QLabel(
        QCoreApplication::translate("Settings", "Link lines"), appearance);
    links_label->setFont(theme_bold);
    appearance_layout->addWidget(links_label);
    link_style_ = new SegmentedControl(appearance);
    link_style_->setObjectName(QStringLiteral("linkStyleControl"));
    link_style_->add_item(QCoreApplication::translate("Settings", "Curved"), 0);
    link_style_->add_item(
        QCoreApplication::translate("Settings", "Right angles"), 1);
    link_style_->set_current(settings_.links == link_style::elbow ? 1 : 0);
    connect(link_style_, &SegmentedControl::current_changed, this,
            [this](int id)
            {
                settings_.links =
                    id == 1 ? link_style::elbow : link_style::curved;
                persist();
            });
    auto* links_holder = new QWidget(appearance);
    links_holder->setMaximumWidth(320);
    auto* links_layout = new QVBoxLayout(links_holder);
    links_layout->setContentsMargins(0, 0, 0, 0);
    links_layout->addWidget(link_style_);
    appearance_layout->addWidget(links_holder);

    // Text size, scale, contrast and motion are not Studio settings, so a
    // banner sends the user to the system settings. If those cannot be
    // opened, an error dialog says where to find them.
    auto* display_note = new NoteBanner(
        NoteBanner::kind::info,
        QCoreApplication::translate(
            "Settings",
            "Text size, display scale, high contrast and reduced motion follow "
            "your operating system."),
        appearance);
    display_note->set_action(
        QCoreApplication::translate("Settings", "Open system settings"),
        [this]
        {
            if (!open_system_accessibility_settings())
            {
                ErrorDialog dialog(
                    QCoreApplication::translate(
                        "Settings", "Could not open system settings"),
                    QCoreApplication::translate(
                        "Settings", "Open your operating system's "
                                    "display or accessibility settings "
                                    "yourself."),
                    {}, this);
                dialog.exec();
            }
        });
    appearance_layout->addWidget(display_note);
    appearance_layout->addStretch(1);
    pages_->addWidget(scroll_page(appearance, pages_));

    // -- Performance -------------------------------------------------------
    auto* performance = new QWidget(pages_);
    auto* performance_layout = new QVBoxLayout(performance);
    performance_layout->setContentsMargins(24, 4, 24, 16);
    performance_layout->setSpacing(14);
    performance_layout->addWidget(section_title(
        QCoreApplication::translate("Settings", "Performance"), performance));

    // Refresh interval: a slider with its current value above it, and the
    // two ends labelled below. The value label is set after the slider so
    // both start from the saved setting.
    auto* refresh_holder = new QWidget(performance);
    refresh_holder->setMaximumWidth(460);
    auto* refresh_layout = new QVBoxLayout(refresh_holder);
    refresh_layout->setContentsMargins(0, 0, 0, 0);
    refresh_layout->setSpacing(4);
    auto* refresh_heading = new QHBoxLayout();
    refresh_heading->setContentsMargins(0, 0, 0, 0);
    auto* refresh_label =
        new QLabel(QCoreApplication::translate("Settings", "Refresh interval"),
                   refresh_holder);
    refresh_label->setFont(theme_bold);
    refresh_heading->addWidget(refresh_label);
    refresh_heading->addStretch(1);
    refresh_value_ = new QLabel(refresh_holder);
    refresh_value_->setFont(Theme::instance().font(text_style::data));
    refresh_value_->setObjectName(QStringLiteral("refreshValue"));
    refresh_heading->addWidget(refresh_value_);
    refresh_layout->addLayout(refresh_heading);

    refresh_ = new QSlider(Qt::Horizontal, refresh_holder);
    refresh_->setObjectName(QStringLiteral("refreshSlider"));
    refresh_->setAccessibleName(
        QCoreApplication::translate("Settings", "Refresh interval"));
    refresh_->setRange(kRefreshMinMs, kRefreshMaxMs);
    refresh_->setSingleStep(10);
    refresh_->setPageStep(250);
    refresh_->setValue(settings_.refresh_ms);
    refresh_layout->addWidget(refresh_);
    refresh_value_->setText(QCoreApplication::translate("Settings", "%1 ms")
                                .arg(settings_.refresh_ms));

    auto* ends = new QWidget(refresh_holder);
    auto* ends_layout = new QHBoxLayout(ends);
    ends_layout->setContentsMargins(0, 0, 0, 0);
    ends_layout->addWidget(hint_label(
        QCoreApplication::translate("Settings", "50 ms, live"), ends));
    ends_layout->addStretch(1);
    ends_layout->addWidget(hint_label(
        QCoreApplication::translate("Settings", "5000 ms, light"), ends));
    refresh_layout->addWidget(ends);
    refresh_layout->addWidget(hint_label(
        QCoreApplication::translate("Settings",
                                    "How often counters and states are read. "
                                    "Shorter feels live. Longer "
                                    "is lighter on a busy machine."),
        refresh_holder));
    performance_layout->addWidget(refresh_holder);

    // -- Profiling ---------------------------------------------------------
    performance_layout->addWidget(section_title(
        QCoreApplication::translate("Settings", "Profiling"), performance));
    auto* profiling_holder = new QWidget(performance);
    profiling_holder->setMaximumWidth(460);
    auto* profiling_layout = new QVBoxLayout(profiling_holder);
    profiling_layout->setContentsMargins(0, 0, 0, 0);
    profiling_layout->setSpacing(8);
    // Two switches and a status line. The initial state is not set here:
    // sync_profiling() fills it in once the connections below exist.
    profiling_ = new Switch(
        QCoreApplication::translate("Settings", "Record a performance trace"),
        profiling_holder);
    profiling_->setObjectName(QStringLiteral("profilingSwitch"));
    profiling_layout->addWidget(profiling_);
    profiling_remote_ = new Switch(
        QCoreApplication::translate("Settings", "Let other computers connect"),
        profiling_holder);
    profiling_remote_->setObjectName(QStringLiteral("profilingRemoteSwitch"));
    profiling_layout->addWidget(profiling_remote_);
    profiling_status_ = hint_label({}, profiling_holder);
    profiling_status_->setObjectName(QStringLiteral("profilingStatus"));
    profiling_layout->addWidget(profiling_status_);
    profiling_layout->addWidget(hint_label(
        QCoreApplication::translate(
            "Settings",
            "For finding out why Studio is slow. It records timings only and "
            "sends nothing until you connect the Tracy profiler to the address "
            "above. It is off again each time Studio starts."),
        profiling_holder));
    performance_layout->addWidget(profiling_holder);
    performance_layout->addStretch(1);
    pages_->addWidget(scroll_page(performance, pages_));

    // The rest of the connections come last, after every control has its
    // initial value, so setting up the pages does not fire them. Each one
    // updates the settings and persist() saves them: there is no Apply
    // button.
    connect(group, &QButtonGroup::idClicked, this,
            [this](int id) { select_page(id); });
    connect(advanced_, &Switch::toggled, this,
            [this](bool on)
            {
                settings_.advanced = on;
                persist();
                Q_EMIT advanced_changed(on);
            });
    connect(baud_, &QComboBox::currentTextChanged, this,
            [this](const QString& text)
            {
                settings_.default_baud = text.toInt();
                persist();
            });
    connect(ask_on_discard_, &QCheckBox::toggled, this,
            [this](bool on)
            {
                settings_.ask_on_discard = on;
                persist();
            });
    connect(close_background_, &QRadioButton::toggled, this,
            [this](bool on)
            {
                settings_.keep_running_on_close = on;
                persist();
            });
    // Starting at sign-in changes the operating system, so it can fail. On
    // failure show the real state again, with signals blocked so that this
    // does not recurse, and say that nothing changed.
    connect(sign_in_, &Switch::toggled, this,
            [this](bool on)
            {
                if (set_sign_in_start(on))
                    return;
                const QSignalBlocker blocker(sign_in_);
                sign_in_->setChecked(sign_in_start_enabled());
                ErrorDialog dialog(
                    QCoreApplication::translate(
                        "Settings", "Could not change start at sign-in"),
                    QCoreApplication::translate(
                        "Settings",
                        "Studio could not write the sign-in entry. Nothing "
                        "was changed."),
                    {}, this);
                dialog.exec();
            });
    connect(refresh_, &QSlider::valueChanged, this,
            [this](int value)
            {
                settings_.refresh_ms = value;
                apply_refresh();
            });
    apply_refresh();

    // Profiling is owned by a process-wide controller. The switch asks it to
    // start or stop, and its changed signal brings the switches and status
    // text back in line, including changes made elsewhere.
    auto& profiling = ProfilingController::instance();
    connect(profiling_, &Switch::toggled, this,
            [this](bool on)
            {
                auto& controller = ProfilingController::instance();
                if (on)
                    controller.enable(profiling_remote_->isChecked());
                else
                    controller.disable();
                sync_profiling();
            });
    connect(&profiling, &ProfilingController::changed, this,
            [this] { sync_profiling(); });
    sync_profiling();
    outer->addWidget(content, 1);

    // Footer: a note that changes apply at once, then Reset to defaults and
    // Close. Reset is not a default button, so Enter never wipes settings.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* immediate = new QLabel(
        QCoreApplication::translate("Settings", "Changes apply immediately."),
        footer);
    immediate->setFont(Theme::instance().font(text_style::caption));
    immediate->setProperty("muted", true);
    polish(immediate);
    const int stretch_index = 0;
    footer_layout->insertWidget(stretch_index, immediate);
    auto* reset = make_dialog_button(
        QCoreApplication::translate("Settings", "Reset to defaults"), "quiet",
        footer);
    reset->setAutoDefault(false);
    connect(reset, &QPushButton::clicked, this,
            &SettingsDialog::reset_to_defaults);
    footer_layout->addWidget(reset);
    auto* close = make_dialog_button(
        QCoreApplication::translate("Settings", "Close"), "primary", footer);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    outer->addWidget(footer);

    // Focus Close now and again once the event loop runs, in case showing
    // the dialog moves focus elsewhere first.
    close->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, close,
                       [close] { close->setFocus(Qt::OtherFocusReason); });
}

void SettingsDialog::show_page(int index)
{
    select_page(index);
    if (nav_ != nullptr && nav_->button(index) != nullptr)
        nav_->button(index)->setChecked(true);
}

void SettingsDialog::select_page(int index)
{
    if (pages_ != nullptr)
        pages_->setCurrentIndex(index);
}

void SettingsDialog::apply_theme()
{
    Theme::instance().set_mode(settings_.theme);
    persist();
}

void SettingsDialog::apply_density()
{
    Theme::instance().set_density(settings_.density);
    persist();
}

void SettingsDialog::sync_profiling()
{
    const auto& controller = ProfilingController::instance();
    const auto status = controller.status();
    const bool available = controller.available();
    const bool recording = status.current == vss::profiling::state::recording;
    const bool listening = status.port != 0;
    {
        const QSignalBlocker first(profiling_);
        const QSignalBlocker second(profiling_remote_);
        profiling_->setChecked(recording);
        profiling_remote_->setChecked(
            listening ? status.remote : profiling_remote_->isChecked());
    }
    profiling_->setEnabled(available);
    // The choice is made when the listener opens; after that it cannot change
    // until Studio restarts.
    profiling_remote_->setEnabled(available && !listening);
    if (!available)
    {
        profiling_->setToolTip(QCoreApplication::translate(
            "Settings", "Not available in this build"));
    }
    else if (listening && !status.remote)
    {
        profiling_remote_->setToolTip(QCoreApplication::translate(
            "Settings", "Restart Studio with --profile-remote to allow this"));
    }
    else
    {
        profiling_remote_->setToolTip({});
    }
    QString text = controller.summary();
    if (!controller.last_error().isEmpty())
        text = controller.last_error();
    profiling_status_->setText(text);
    profiling_status_->setAccessibleName(text);
}

void SettingsDialog::apply_refresh()
{
    settings_.refresh_ms =
        std::clamp(settings_.refresh_ms, kRefreshMinMs, kRefreshMaxMs);
    backend_.set_refresh_interval(settings_.refresh_ms);
    if (refresh_value_ != nullptr)
        refresh_value_->setText(QCoreApplication::translate("Settings", "%1 ms")
                                    .arg(settings_.refresh_ms));
    persist();
}

void SettingsDialog::persist()
{
    save_ui_settings(settings_);
    Q_EMIT settings_changed(settings_);
}

void SettingsDialog::reset_to_defaults()
{
    settings_ = default_ui_settings();
    {
        const QSignalBlocker block_advanced(advanced_);
        const QSignalBlocker block_baud(baud_);
        const QSignalBlocker block_ask(ask_on_discard_);
        const QSignalBlocker block_refresh(refresh_);
        advanced_->setChecked(settings_.advanced);
        baud_->setCurrentText(QString::number(settings_.default_baud));
        ask_on_discard_->setChecked(settings_.ask_on_discard);
        const QSignalBlocker block_close(close_background_);
        const QSignalBlocker block_quit(close_quit_);
        close_background_->setChecked(settings_.keep_running_on_close);
        close_quit_->setChecked(!settings_.keep_running_on_close);
        refresh_->setValue(settings_.refresh_ms);
        const QSignalBlocker block_density(density_);
        density_->set_current(0);
        const QSignalBlocker block_links(link_style_);
        link_style_->set_current(0);
    }
    {
        const QSignalBlocker block_system(theme_system_);
        const QSignalBlocker block_light(theme_light_);
        const QSignalBlocker block_dark(theme_dark_);
        theme_system_->setChecked(true);
        theme_light_->setChecked(false);
        theme_dark_->setChecked(false);
    }
    Theme::instance().set_mode(settings_.theme);
    Theme::instance().set_density(settings_.density);
    backend_.set_refresh_interval(settings_.refresh_ms);
    if (refresh_value_ != nullptr)
        refresh_value_->setText(QCoreApplication::translate("Settings", "%1 ms")
                                    .arg(settings_.refresh_ms));
    persist();
    Q_EMIT advanced_changed(false);
}

} // namespace vss::gui::support
