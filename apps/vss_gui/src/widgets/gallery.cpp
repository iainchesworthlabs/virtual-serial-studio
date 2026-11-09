//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "widgets/gallery.hpp"

#include "widgets/chips.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

QLabel* heading(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(semibold(Theme::instance().font(text_style::subtitle)));
    return label;
}

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    label->setWordWrap(true);
    return label;
}

QWidget* row(QWidget* parent)
{
    auto* widget = new QWidget(parent);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    return widget;
}

QHBoxLayout* row_layout(QWidget* row_widget)
{
    return qobject_cast<QHBoxLayout*>(row_widget->layout());
}

QPushButton* button(const QString& text, const char* role, QWidget* parent,
                    bool small = false)
{
    auto* result = new QPushButton(text, parent);
    result->setCursor(Qt::PointingHandCursor);
    if (role != nullptr)
        set_role(result, role);
    if (small)
    {
        result->setProperty("size", QStringLiteral("small"));
        polish(result);
    }
    return result;
}

QVBoxLayout* card(const QString& title, const QString& note, QWidget* parent,
                  QWidget*& card_widget)
{
    auto* frame = new QFrame(parent);
    frame->setProperty("card", true);
    polish(frame);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    layout->addWidget(heading(title, frame));
    if (!note.isEmpty())
        layout->addWidget(caption(note, frame));
    card_widget = frame;
    return layout;
}

} // namespace

WidgetGallery::WidgetGallery(QWidget* parent) : QDialog(parent)
{
    Theme::instance().initialise();
    setWindowTitle(QStringLiteral("Widget gallery"));
    setAccessibleName(QStringLiteral("Widget gallery"));
    setMinimumSize(980, 720);
    build();
}

void WidgetGallery::build()
{
    VSS_PROFILE_ZONE("gui.widgets.gallery.build");
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);

    auto* header = row(this);
    auto* header_layout = row_layout(header);
    auto* title = new QLabel(QStringLiteral("Widget gallery"), header);
    title->setFont(Theme::instance().font(text_style::display));
    header_layout->addWidget(title);
    header_layout->addStretch(1);

    auto* theme_caption = new QLabel(QStringLiteral("Theme"), header);
    theme_caption->setFont(Theme::instance().font(text_style::caption));
    header_layout->addWidget(theme_caption);
    theme_segment_ = new SegmentedControl(header);
    theme_segment_->add_item(QStringLiteral("System"));
    theme_segment_->add_item(QStringLiteral("Light"), 1);
    theme_segment_->add_item(QStringLiteral("Dark"), 2);
    connect(theme_segment_, &SegmentedControl::current_changed, this,
            [](int id)
            {
                if (id == 1)
                    Theme::instance().set_mode(theme_mode::light);
                else if (id == 2)
                    Theme::instance().set_mode(theme_mode::dark);
                else
                    Theme::instance().set_mode(theme_mode::system);
            });
    header_layout->addWidget(theme_segment_);

    auto* density_caption = new QLabel(QStringLiteral("Density"), header);
    density_caption->setFont(Theme::instance().font(text_style::caption));
    header_layout->addWidget(density_caption);
    density_segment_ = new SegmentedControl(header);
    density_segment_->add_item(QStringLiteral("Comfortable"));
    density_segment_->add_item(QStringLiteral("Compact"), 1);
    connect(density_segment_, &SegmentedControl::current_changed, this,
            [](int id)
            {
                Theme::instance().set_density(id == 1
                                                  ? density_mode::compact
                                                  : density_mode::comfortable);
            });
    header_layout->addWidget(density_segment_);
    root->addWidget(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    content_ = new QWidget(scroll);
    content_layout_ = new QVBoxLayout(content_);
    content_layout_->setContentsMargins(0, 0, 0, 0);
    content_layout_->setSpacing(12);
    scroll->setWidget(content_);
    root->addWidget(scroll, 1);

    add_status_section();
    add_controls_section();
    add_fields_section();
    add_feedback_section();
    add_dialogs_section();
    content_layout_->addStretch(1);
}

void WidgetGallery::set_theme(theme_mode mode)
{
    Theme::instance().set_mode(mode);
    if (theme_segment_ != nullptr)
    {
        theme_segment_->set_current(mode == theme_mode::light  ? 1
                                    : mode == theme_mode::dark ? 2
                                                               : 0);
    }
}

void WidgetGallery::set_density(density_mode density)
{
    Theme::instance().set_density(density);
    if (density_segment_ != nullptr)
        density_segment_->set_current(density == density_mode::compact ? 1 : 0);
}

void WidgetGallery::add_status_section()
{
    QWidget* card_widget = nullptr;
    QVBoxLayout* layout =
        card(QStringLiteral("Status"),
             QStringLiteral("Text plus shape plus colour; never colour alone."),
             content_, card_widget);

    auto* chips = row(card_widget);
    auto* chips_layout = row_layout(chips);
    chips_layout->addWidget(
        new StatusChip(QStringLiteral("Open"), StatusChip::kind::open, chips));
    chips_layout->addWidget(
        new StatusChip(QStringLiteral("Waiting for program"),
                       StatusChip::kind::waiting, chips));
    chips_layout->addWidget(new StatusChip(QStringLiteral("Program connected"),
                                           StatusChip::kind::connected, chips));
    chips_layout->addWidget(new StatusChip(
        QStringLiteral("Studio only"), StatusChip::kind::studio_only, chips));
    chips_layout->addWidget(new StatusChip(QStringLiteral("Closed"),
                                           StatusChip::kind::closed, chips));
    chips_layout->addWidget(new StatusChip(QStringLiteral("Failed"),
                                           StatusChip::kind::failed, chips));
    chips_layout->addWidget(new StatusChip(QStringLiteral("Fixture"),
                                           StatusChip::kind::fixture, chips));
    chips_layout->addStretch(1);
    layout->addWidget(chips);

    auto* small_chips = row(card_widget);
    auto* small_layout = row_layout(small_chips);
    for (const auto kind : {StatusChip::kind::open, StatusChip::kind::waiting,
                            StatusChip::kind::closed, StatusChip::kind::failed})
    {
        auto* chip = new StatusChip(small_chips);
        chip->set_small(true);
        chip->set_kind(kind);
        chip->set_text(
            kind == StatusChip::kind::open      ? QStringLiteral("Open")
            : kind == StatusChip::kind::waiting ? QStringLiteral("Waiting")
            : kind == StatusChip::kind::closed  ? QStringLiteral("Closed")
                                                : QStringLiteral("Failed"));
        small_layout->addWidget(chip);
    }
    small_layout->addStretch(1);
    layout->addWidget(small_chips);

    auto* badges = row(card_widget);
    auto* badges_layout = row_layout(badges);
    badges_layout->addWidget(
        new DirectionBadge(DirectionBadge::direction::rx, badges));
    badges_layout->addWidget(
        new DirectionBadge(DirectionBadge::direction::tx, badges));
    badges_layout->addWidget(
        new DirectionBadge(DirectionBadge::direction::drop, badges));
    badges_layout->addWidget(
        new Tag(QStringLiteral("New"), Tag::variant::n, badges));
    badges_layout->addWidget(
        new Tag(QStringLiteral("Advanced"), Tag::variant::a, badges));
    badges_layout->addWidget(
        new Tag(QStringLiteral("Selected"), Tag::variant::s, badges));
    badges_layout->addWidget(
        new Tag(QStringLiteral("Ready"), Tag::variant::ok, badges));
    badges_layout->addSpacing(10);
    badges_layout->addWidget(new Badge(QStringLiteral("ADVANCED"), badges));
    badges_layout->addSpacing(10);
    auto* open_dot = new StateDot(badges);
    open_dot->set_kind(StateDot::kind::filled);
    open_dot->set_colour(colour_role::ok);
    open_dot->setAccessibleName(QStringLiteral("Open"));
    badges_layout->addWidget(open_dot);
    auto* closed_dot = new StateDot(badges);
    closed_dot->set_kind(StateDot::kind::hollow);
    closed_dot->set_colour(colour_role::ink3);
    closed_dot->setAccessibleName(QStringLiteral("Closed"));
    badges_layout->addWidget(closed_dot);
    auto* failed_dot = new StateDot(badges);
    failed_dot->set_kind(StateDot::kind::cross);
    failed_dot->set_colour(colour_role::bad);
    failed_dot->setAccessibleName(QStringLiteral("Failed"));
    badges_layout->addWidget(failed_dot);
    badges_layout->addWidget(new KeyCap(QStringLiteral("Ctrl+K"), badges));
    badges_layout->addStretch(1);
    layout->addWidget(badges);
    content_layout_->addWidget(card_widget);
}

void WidgetGallery::add_controls_section()
{
    // One card with a row for each family of controls.
    QWidget* card_widget = nullptr;
    QVBoxLayout* layout = card(
        QStringLiteral("Controls"),
        QStringLiteral("Keyboard reachable, visible focus, 24 px minimum."),
        content_, card_widget);

    // Buttons in each role, plus a disabled one and the copy button.
    auto* buttons = row(card_widget);
    auto* buttons_layout = row_layout(buttons);
    buttons_layout->addWidget(
        button(QStringLiteral("Create pair"), "primary", buttons));
    buttons_layout->addWidget(
        button(QStringLiteral("Cancel"), nullptr, buttons));
    buttons_layout->addWidget(
        button(QStringLiteral("Edit address"), "quiet", buttons));
    buttons_layout->addWidget(
        button(QStringLiteral("Remove port"), "danger", buttons));
    buttons_layout->addWidget(
        button(QStringLiteral("Inject"), nullptr, buttons, true));
    auto* disabled_save = button(QStringLiteral("Save"), nullptr, buttons);
    disabled_save->setEnabled(false);
    buttons_layout->addWidget(disabled_save);
    auto* copy = new CopyButton(buttons);
    copy->set_copy_text(QStringLiteral("COM7"), QStringLiteral("COM7"));
    buttons_layout->addWidget(copy);
    buttons_layout->addStretch(1);
    layout->addWidget(buttons);

    // A segmented control with the two-state switches beside it.
    auto* segments = row(card_widget);
    auto* segments_layout = row_layout(segments);
    auto* protocol = new SegmentedControl(segments);
    protocol->add_item(QStringLiteral("TCP"));
    protocol->add_item(QStringLiteral("UDP"), 1);
    protocol->add_item(QStringLiteral("RFC 2217"), 2);
    protocol->setAccessibleName(QStringLiteral("Transport"));
    segments_layout->addWidget(protocol);

    auto* dtr = new Switch(QStringLiteral("DTR"), segments);
    dtr->setChecked(true);
    dtr->setAccessibleName(QStringLiteral("DTR"));
    segments_layout->addWidget(dtr);
    auto* rts = new Switch(QStringLiteral("RTS"), segments);
    rts->setAccessibleName(QStringLiteral("RTS"));
    segments_layout->addWidget(rts);
    segments_layout->addStretch(1);
    layout->addWidget(segments);

    // Pick cards are exclusive choices, so they share one button group.
    auto* pick_row = row(card_widget);
    auto* pick_layout = row_layout(pick_row);
    auto* virtual_port = new PickCard(
        QStringLiteral("Virtual port"),
        QStringLiteral("A port that other programs can open."), pick_row);
    auto* pair = new PickCard(
        QStringLiteral("Linked pair"),
        QStringLiteral("Two ports wired together like a null-modem cable."),
        pick_row);
    auto* group = new QButtonGroup(pick_row);
    group->addButton(virtual_port->radio());
    group->addButton(pair->radio());
    virtual_port->set_selected(true);
    pick_layout->addWidget(virtual_port, 1);
    pick_layout->addWidget(pair, 1);
    layout->addWidget(pick_row);

    // A stepper, shown part way through its steps.
    auto* steps = row(card_widget);
    auto* steps_layout = row_layout(steps);
    auto* stepper = new Stepper(steps);
    stepper->set_steps({QStringLiteral("Device"), QStringLiteral("Programs"),
                        QStringLiteral("Ready")});
    stepper->set_current(1);
    steps_layout->addWidget(stepper);
    steps_layout->addStretch(1);
    layout->addWidget(steps);

    // A collapsible section holding a caption and a field.
    auto* disclosure =
        new Disclosure(QStringLiteral("More options"), card_widget);
    auto* content = new QWidget(disclosure);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->addWidget(
        caption(QStringLiteral("Signal wiring and port kind."), content));
    content_layout->addWidget(new QLineEdit(QStringLiteral("port-1"), content));
    disclosure->set_content(content);
    layout->addWidget(disclosure);

    // Toasts appear over the window, so a button is needed to show one.
    auto* toast_row = row(card_widget);
    auto* toast_layout = row_layout(toast_row);
    auto* toast_button =
        button(QStringLiteral("Show toast"), nullptr, toast_row);
    connect(toast_button, &QPushButton::clicked, this,
            [this]
            {
                ToastManager::instance().show(
                    this, QStringLiteral("Removed terminal and its link."),
                    QStringLiteral("Undo"), [] {});
            });
    toast_layout->addWidget(toast_button);
    toast_layout->addWidget(caption(
        QStringLiteral("Toasts stack at the bottom centre for six seconds."),
        toast_row));
    toast_layout->addStretch(1);
    layout->addWidget(toast_row);

    // Add the finished card to the gallery page.
    content_layout_->addWidget(card_widget);
}

void WidgetGallery::add_fields_section()
{
    QWidget* card_widget = nullptr;
    QVBoxLayout* layout =
        card(QStringLiteral("Fields"),
             QStringLiteral("Label, field, hint and an inline error."),
             content_, card_widget);

    auto* name = new QLineEdit(QStringLiteral("port-1"), card_widget);
    auto* name_row =
        new FormRow(QStringLiteral("Port name"), name,
                    QStringLiteral("A unique suffix is added if this name is "
                                   "already in use."),
                    card_widget);
    layout->addWidget(name_row);

    auto* path = new QLineEdit(QStringLiteral("COM3"), card_widget);
    path->setFont(Theme::instance().font(text_style::data));
    auto* path_row =
        new FormRow(QStringLiteral("Port path"), path, {}, card_widget);
    path_row->set_error(QStringLiteral("COM3 is already used. Try COM9."));
    layout->addWidget(path_row);

    auto* settings = row(card_widget);
    auto* settings_layout = row_layout(settings);
    auto* baud = new QComboBox(settings);
    baud->setFont(Theme::instance().font(text_style::body));
    baud->addItems({QStringLiteral("9600"), QStringLiteral("19200"),
                    QStringLiteral("115200")});
    baud->setCurrentText(QStringLiteral("115200"));
    baud->setAccessibleName(QStringLiteral("Baud rate"));
    settings_layout->addWidget(baud);

    auto* count = new QSpinBox(settings);
    count->setFont(Theme::instance().font(text_style::body));
    count->setRange(1, 32);
    count->setValue(2);
    count->setAccessibleName(QStringLiteral("How many"));
    settings_layout->addWidget(count);

    auto* capture = new QCheckBox(QStringLiteral("Capture traffic"), settings);
    capture->setChecked(true);
    capture->setAccessibleName(QStringLiteral("Capture traffic"));
    settings_layout->addWidget(capture);
    settings_layout->addStretch(1);
    layout->addWidget(settings);

    auto* editor = new QPlainTextEdit(card_widget);
    editor->setFont(Theme::instance().font(text_style::data));
    editor->setPlainText(QStringLiteral("AT+VERSION\r\n"));
    editor->setAccessibleName(QStringLiteral("Payload"));
    editor->setFixedHeight(64);
    layout->addWidget(editor);

    content_layout_->addWidget(card_widget);
}

void WidgetGallery::add_feedback_section()
{
    QWidget* card_widget = nullptr;
    QVBoxLayout* layout = card(QStringLiteral("Banners and messages"),
                               QString(), content_, card_widget);

    layout->addWidget(new NoteBanner(NoteBanner::kind::info,
                                     QStringLiteral("Data flows both ways."),
                                     card_widget));
    layout->addWidget(new NoteBanner(
        NoteBanner::kind::ok, QStringLiteral("Bridge is running on port 4001."),
        card_widget));
    layout->addWidget(new NoteBanner(
        NoteBanner::kind::warn,
        QStringLiteral("Windows will ask for administrator permission."),
        card_widget));
    layout->addWidget(new NoteBanner(
        NoteBanner::kind::danger,
        QStringLiteral("Could not open COM3. Another program is using it."),
        card_widget));

    auto* sentence = new ResultSentence(card_widget);
    sentence->set_result_text(
        QStringLiteral("This creates one port named port-1."));
    layout->addWidget(sentence);

    auto* code = new CodeBlock(card_widget);
    code->set_text(QStringLiteral("vss_cli virtual --kind=auto"),
                   QStringLiteral("Command"));
    layout->addWidget(code);

    content_layout_->addWidget(card_widget);
}

void WidgetGallery::add_dialogs_section()
{
    QWidget* card_widget = nullptr;
    QVBoxLayout* layout =
        card(QStringLiteral("Dialogs"),
             QStringLiteral("Safest action focused, Escape cancels."), content_,
             card_widget);
    auto* buttons = row(card_widget);
    auto* buttons_layout = row_layout(buttons);
    auto* confirm = button(QStringLiteral("Confirm dialog"), nullptr, buttons);
    connect(confirm, &QPushButton::clicked, this,
            [this]
            {
                ConfirmDialog dialog(
                    QStringLiteral("Remove terminal?"),
                    QStringLiteral("Its link to device is removed too. You can "
                                   "undo this straight afterwards."),
                    QStringLiteral("Remove"), QStringLiteral("Cancel"), true,
                    this);
                dialog.exec();
            });
    buttons_layout->addWidget(confirm);

    auto* error = button(QStringLiteral("Error dialog"), nullptr, buttons);
    connect(
        error, &QPushButton::clicked, this,
        [this]
        {
            ErrorDialog dialog(
                QStringLiteral("Could not open COM3"),
                QStringLiteral("Another program is using it, or the device "
                               "was unplugged. Close the other program, then "
                               "try again."),
                QStringLiteral("asio error 5: access denied"), this);
            dialog.exec();
        });
    buttons_layout->addWidget(error);
    buttons_layout->addStretch(1);
    layout->addWidget(buttons);
    content_layout_->addWidget(card_widget);
}

} // namespace vss::gui
