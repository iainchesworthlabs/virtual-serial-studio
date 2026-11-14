//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/troubleshoot_dialog.hpp"

#include "support/check_row.hpp"
#include "support/troubleshoot.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/surfaces.hpp"

#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

row_tone tone_for(check_state state)
{
    switch (state)
    {
    case check_state::passed:
        return row_tone::ok;
    case check_state::attention:
        return row_tone::warn;
    case check_state::not_checked:
        break;
    }
    return row_tone::neutral;
}

QVector<port_view> to_ports(const std::vector<port_view>& ports)
{
    QVector<port_view> out;
    out.reserve(static_cast<qsizetype>(ports.size()));
    for (const port_view& port : ports)
        out.push_back(port);
    return out;
}

QVector<link_view> to_links(const std::vector<link_view>& links)
{
    QVector<link_view> out;
    out.reserve(static_cast<qsizetype>(links.size()));
    for (const link_view& link : links)
        out.push_back(link);
    return out;
}

QString escape(const QString& text)
{
    return text.toHtmlEscaped();
}

} // namespace

TroubleshootDialog::TroubleshootDialog(StudioBackend& backend,
                                       vss::endpoint_id port, QWidget* parent)
    : QDialog(parent), backend_(backend), port_id_(port)
{
    build();
    rebuild();
}

void TroubleshootDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.troubleshoot_dialog.build");
    VSS_PROFILE_MESSAGE("gui.dialog.troubleshoot.opened");
    // Window chrome: accessible name, panel surface and the small dialog
    // width, which is also the minimum.
    setWindowTitle(QCoreApplication::translate("Troubleshoot",
                                               "Why is nothing arriving?"));
    setAccessibleName(QCoreApplication::translate("Troubleshoot",
                                                  "Why is nothing arriving?"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")),
        760);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    // The outer layout has no margins so the header, scrolling body and
    // footer set their own padding. The header title is filled in by
    // rebuild(), once the port path is known.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 14);
    header_layout->setSpacing(4);
    title_ = new QLabel(header);
    title_->setFont(Theme::instance().font(text_style::title));
    title_->setWordWrap(true);
    header_layout->addWidget(title_);
    sub_ = new QLabel(
        QCoreApplication::translate(
            "Troubleshoot",
            "Studio checked each step between the device and your program."),
        header);
    sub_->setFont(Theme::instance().font(text_style::body));
    sub_->setProperty("muted", true);
    sub_->setWordWrap(true);
    polish(sub_);
    header_layout->addWidget(sub_);
    outer->addWidget(header);

    // The body scrolls vertically only: long check sentences wrap instead of
    // forcing a horizontal scroll bar.
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget(scroll);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 4, 24, 16);
    body_layout->setSpacing(14);

    // The checks group starts empty. rebuild() adds one row per check and
    // uses the stored host and layout pointers to replace them on refresh.
    auto* checks = new QFrame(body);
    checks->setProperty("group", true);
    checks->setAttribute(Qt::WA_StyledBackground, true);
    polish(checks);
    auto* checks_layout = new QVBoxLayout(checks);
    checks_layout->setContentsMargins(0, 0, 0, 0);
    checks_layout->setSpacing(0);
    checks_host_ = checks;
    checks_layout_ = checks_layout;
    body_layout->addWidget(checks);

    // "Try this" is an info note: the next steps, tinted, with no edge.
    auto* try_card = new QFrame(body);
    try_card->setProperty("note", QStringLiteral("info"));
    try_card->setAttribute(Qt::WA_StyledBackground, true);
    polish(try_card);
    try_card_ = try_card;
    auto* try_layout = new QVBoxLayout(try_card);
    const Theme& theme = Theme::instance();
    try_layout->setContentsMargins(theme.space(3), theme.space(2),
                                   theme.space(3), theme.space(3));
    try_layout->setSpacing(theme.space(1));
    auto* eyebrow = new GroupLabel(
        QCoreApplication::translate("Troubleshoot", "Try this"), try_card);
    try_layout->addWidget(eyebrow);

    // The steps are rich text so the port path can be bold; rebuild() sets
    // the text and escapes the path first.
    steps_ = new QLabel(try_card);
    steps_->setTextFormat(Qt::RichText);
    steps_->setWordWrap(true);
    steps_->setFont(Theme::instance().font(text_style::body));
    steps_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    try_layout->addWidget(steps_);

    // Two small actions under the steps: copy the port path, and re-run the
    // checks. Neither is a default button, so Enter still means Close.
    auto* buttons = new QHBoxLayout();
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(8);
    copy_ = make_dialog_button(
        QCoreApplication::translate("Troubleshoot", "Copy port path"), nullptr,
        try_card);
    copy_->setProperty("size", QStringLiteral("small"));
    copy_->setIcon(IconProvider::instance().icon(QStringLiteral("copy"),
                                                 colour_role::ink, 16));
    polish(copy_);
    copy_->setAutoDefault(false);
    connect(copy_, &QPushButton::clicked, this, &TroubleshootDialog::copy_path);
    buttons->addWidget(copy_);
    auto* again = make_dialog_button(
        QCoreApplication::translate("Troubleshoot", "Check again"), nullptr,
        try_card);
    again->setProperty("size", QStringLiteral("small"));
    again->setIcon(IconProvider::instance().icon(QStringLiteral("refresh"),
                                                 colour_role::ink, 16));
    polish(again);
    again->setAutoDefault(false);
    connect(again, &QPushButton::clicked, this,
            &TroubleshootDialog::refresh_checks);
    buttons->addWidget(again);
    buttons->addStretch(1);
    try_layout->addLayout(buttons);
    body_layout->addWidget(try_card);

    // A closing hint that points at the diagnostic bundle in the footer.
    auto* muted = new QLabel(
        QCoreApplication::translate(
            "Troubleshoot",
            "A diagnostic bundle collects the setup, the log and the "
            "environment to send to whoever helps you. Traffic contents are "
            "left out unless you include them."),
        body);
    muted->setFont(Theme::instance().font(text_style::caption));
    muted->setProperty("muted", true);
    muted->setWordWrap(true);
    polish(muted);
    body_layout->addWidget(muted);
    // The stretch keeps the content at the top when the dialog is taller
    // than it needs to be.
    body_layout->addStretch(1);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // Footer: the diagnostic bundle request on the left, then Close as the
    // default. The dialog only signals the request; the caller opens the
    // bundle dialog.
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    auto* bundle = make_dialog_button(
        QCoreApplication::translate("Troubleshoot", "Create diagnostic bundle"),
        "quiet", footer);
    bundle->setAutoDefault(false);
    connect(bundle, &QPushButton::clicked, this,
            &TroubleshootDialog::bundle_requested);
    footer_layout->addWidget(bundle);
    auto* close =
        make_dialog_button(QCoreApplication::translate("Troubleshoot", "Close"),
                           "primary", footer);
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

void TroubleshootDialog::rebuild()
{
    VSS_PROFILE_ZONE("gui.support.troubleshoot_dialog.rebuild");
    // Step 1: pick the port. With none given, default to the first one.
    const QVector<port_view> ports = to_ports(backend_.ports());
    if (port_id_ == 0 && !ports.isEmpty())
        port_id_ = ports.first().id;

    // The port may have been removed since the dialog opened; then there is
    // no selection and the path stays empty.
    const port_view* selected = nullptr;
    for (const port_view& port : ports)
    {
        if (port.id == port_id_)
        {
            selected = &port;
            break;
        }
    }
    path_ = selected != nullptr ? port_display_path(*selected) : QString();

    // Step 2: name the port in the title, falling back to the generic title
    // when there is no path to show.
    const QString title =
        path_.isEmpty()
            ? QCoreApplication::translate("Troubleshoot",
                                          "Why is nothing arriving?")
            : QCoreApplication::translate("Troubleshoot",
                                          "Why is nothing arriving on %1?")
                  .arg(path_);
    title_->setText(title);
    setWindowTitle(title);
    setAccessibleName(title);
    // The copy button and the "Try this" card only make sense with a port,
    // so hide them when there is none.
    if (copy_ != nullptr)
    {
        copy_->setText(
            path_.isEmpty()
                ? QCoreApplication::translate("Troubleshoot", "Copy port path")
                : QCoreApplication::translate("Troubleshoot", "Copy %1")
                      .arg(path_));
        copy_->setAccessibleName(copy_->text());
        copy_->setToolTip(copy_->text());
        copy_->setVisible(!path_.isEmpty());
    }
    if (try_card_ != nullptr)
        try_card_->setVisible(!path_.isEmpty());
    if (steps_ != nullptr && !path_.isEmpty())
    {
        // The visible text is rich text, so escape the path. The accessible
        // name below is plain text: the same steps without the markup.
        const QString shown = escape(path_);
        steps_->setText(
            QCoreApplication::translate(
                "Troubleshoot",
                "1. In your program, choose <b>%1</b> as the serial port.<br>"
                "2. If the program shows an error, another program may be "
                "using "
                "%1. Close it, then try again.<br>"
                "3. Some programs only list ports when they start. Restart the "
                "program after creating the port.")
                .arg(shown));
        steps_->setAccessibleName(
            QCoreApplication::translate(
                "Troubleshoot",
                "1. In your program, choose %1 as the serial port. 2. If the "
                "program shows an error, another program may be using %1. "
                "Close "
                "it, then try again. 3. Some programs only list ports when "
                "they "
                "start. Restart the program after creating the port.")
                .arg(path_));
    }

    // Step 3: gather the facts from the backend and let the pure report
    // builder decide what each check row says.
    troubleshoot_facts facts;
    facts.port = port_id_;
    facts.ports = ports;
    facts.links = to_links(backend_.links());
    facts.peer_open_known = backend_.caps().peer_open;
    facts.now = vss::clock_type::now();
    const troubleshoot_report report = build_troubleshoot_report(facts);

    // Step 4: replace the old rows. Taking them out of the layout first and
    // using deleteLater() lets pending events for them finish safely.
    while (QLayoutItem* item = checks_layout_->takeAt(0))
    {
        if (QWidget* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    rows_.clear();

    // Rows are separated by dividers, none before the first. rows_ keeps the
    // widgets so check_widget() can reach them.
    bool first = true;
    for (const check_row& row : report.rows)
    {
        if (!first)
            checks_layout_->addWidget(make_group_divider(checks_host_));
        first = false;
        CheckRowWidget::spec spec;
        spec.tone = tone_for(row.state);
        spec.icon = row.icon;
        spec.title = row.title;
        spec.chip = row.chip;
        spec.sentence = row.sentence;
        spec.highlight = row.highlight;
        spec.framed = false;
        auto* widget = new CheckRowWidget(spec, checks_host_);
        widget->setAccessibleName(tr("%1, %2").arg(row.title, row.chip));
        checks_layout_->addWidget(widget);
        rows_.push_back(widget);
    }
}

QString TroubleshootDialog::title_text() const
{
    return title_ != nullptr ? title_->text() : QString();
}

CheckRowWidget* TroubleshootDialog::check_widget(int index) const
{
    if (index < 0 || index >= rows_.size())
        return nullptr;
    return rows_.at(index);
}

void TroubleshootDialog::refresh_checks()
{
    rebuild();
}

void TroubleshootDialog::copy_path()
{
    if (path_.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(path_);
    ToastManager::instance().show(
        this,
        QCoreApplication::translate("Troubleshoot", "%1 copied.").arg(path_));
}

} // namespace vss::gui::support
