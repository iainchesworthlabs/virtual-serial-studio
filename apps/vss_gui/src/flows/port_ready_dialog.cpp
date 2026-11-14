//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/port_ready_dialog.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

constexpr const char* kDetailsRemembered = "ui/portReadyDetails";

QString json_escape(const QString& value)
{
    QString out;
    out.reserve(value.size());
    for (const QChar character : value)
    {
        if (character == QLatin1Char('\\') || character == QLatin1Char('"'))
            out += QLatin1Char('\\');
        out += character;
    }
    return out;
}

QLabel* key_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    label->setWordWrap(true);
    return label;
}

QLabel* value_label(const QString& text, bool data, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(
        Theme::instance().font(data ? text_style::data : text_style::body));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                   Qt::TextSelectableByKeyboard);
    label->setWordWrap(true);
    label->setAccessibleName(text);
    return label;
}

} // namespace

PortReadyDialog::PortReadyDialog(StudioBackend& backend, vss::endpoint_id port,
                                 const QString& virtual_kind, bool advanced,
                                 platform_kind platform, QWidget* parent)
    : FlowDialog({}, {}, parent), backend_(backend), port_id_(port),
      requested_kind_(virtual_kind), platform_(platform)
{
    const auto view = backend_.port(port_id_);
    if (view)
        port_ = *view;
    else
        port_.id = port_id_;
    studio_only_ = port_.studio_only ||
                   port_.path.startsWith(QStringLiteral("loopback:")) ||
                   port_.path.isEmpty();

    const QString display = studio_only_ ? port_.name : port_.path;
    if (studio_only_)
    {
        set_title_text(tr("%1 is ready inside Studio").arg(port_.name));
        set_subtitle_text(
            tr("The port \"%1\" was created. Only Studio can open it.")
                .arg(port_.name));
    }
    else if (pseudo_terminal_platform(platform_))
    {
        set_title_text(display);
        set_subtitle_text(
            tr("The port \"%1\" was created. Other programs can open it.")
                .arg(port_.name));
    }
    else
    {
        set_title_text(tr("%1 is ready").arg(display));
        set_subtitle_text(
            tr("The port \"%1\" was created. Other programs can open it.")
                .arg(port_.name));
    }

    build(advanced);
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this]
            {
                const auto current = backend_.port(port_id_);
                if (current)
                {
                    port_ = *current;
                    refresh_peer();
                }
            });
    refresh_peer();
    finish_layout(720);
    focus_first(copy_ != nullptr ? static_cast<QWidget*>(copy_)
                                 : static_cast<QWidget*>(primary_button()));
}

void PortReadyDialog::build(bool advanced)
{
    VSS_PROFILE_ZONE("gui.flows.port_ready.build");
    VSS_PROFILE_MESSAGE("gui.dialog.port_ready.opened");
    // Banner showing the path to open. A Studio-only port has no such path,
    // so it shows its name instead.
    path_block_ = new QFrame(this);
    path_block_->setObjectName(QStringLiteral("portReadyPath"));
    auto* path_layout = new QHBoxLayout(path_block_);
    path_layout->setContentsMargins(20, 18, 20, 18);
    path_layout->setSpacing(16);

    auto* path_column = new QVBoxLayout();
    path_column->setContentsMargins(0, 0, 0, 0);
    path_column->setSpacing(2);
    auto* path_caption =
        new QLabel(studio_only_ ? tr("This port is Studio only")
                                : tr("Open this port in your program"),
                   path_block_);
    path_caption->setFont(Theme::instance().font(text_style::caption));
    path_caption->setProperty("muted", true);
    path_column->addWidget(path_caption);
    path_label_ =
        new QLabel(studio_only_ ? port_.name : port_.path, path_block_);
    path_label_->setObjectName(QStringLiteral("portReadyPathValue"));
    path_label_->setFont(Theme::instance().font(text_style::portName));
    path_label_->setWordWrap(true);
    path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                         Qt::TextSelectableByKeyboard);
    path_label_->setAccessibleName(
        studio_only_
            ? tr("This port is Studio only. %1").arg(port_.name)
            : tr("Open this port in your program. %1").arg(port_.path));
    path_column->addWidget(path_label_);
    path_layout->addLayout(path_column, 1);

    // Only a port other programs can open gets a Copy button.
    if (!studio_only_)
    {
        copy_ = new QPushButton(tr("Copy name"), path_block_);
        copy_->setObjectName(QStringLiteral("copyNameButton"));
        copy_->setIcon(IconProvider::instance().icon(QStringLiteral("copy"),
                                                     colour_role::ink, 16));
        copy_->setAccessibleName(tr("Copy name, %1").arg(port_.path));
        copy_->setToolTip(tr("Copy name"));
        copy_->setCursor(Qt::PointingHandCursor);
        connect(copy_, &QPushButton::clicked, this,
                [this]
                {
                    QGuiApplication::clipboard()->setText(port_.path);
                    ToastManager::instance().show(
                        this, tr("%1 copied.").arg(port_.path));
                });
        path_layout->addWidget(copy_, 0, Qt::AlignVCenter);
    }
    body_layout()->addWidget(path_block_);

    // The banner's colours come from the theme, so restyle it when the theme
    // changes.
    const auto apply_path_style = [this]
    {
        if (path_block_ == nullptr)
            return;
        path_block_->setStyleSheet(
            QStringLiteral("QFrame#portReadyPath { background: %1; border: "
                           "1px solid %2; border-radius: 8px; }")
                .arg(Theme::instance()
                         .colour(colour_role::sunken)
                         .name(QColor::HexRgb),
                     Theme::instance()
                         .colour(colour_role::line2)
                         .name(QColor::HexRgb)));
    };
    apply_path_style();
    connect(&Theme::instance(), &Theme::theme_changed, this, apply_path_style);

    // A Studio-only port needs an explanation and a way to set up a driver; a
    // real port instead shows how to use it.
    if (studio_only_)
    {
        // Pick the wording by whether a Windows driver is already present.
        const setup_facts facts = backend_.facts();
        const bool driver_present = windows_platform(platform_) &&
                                    (facts.driver_found || facts.com0com_found);
        driver_note_ = new NoteBanner(
            NoteBanner::kind::warn,
            driver_present
                ? tr("Other programs cannot open this port. It exists only "
                     "inside Studio.")
                : tr("Other programs cannot open this port. Install a driver "
                     "to create ports they can open."),
            this);
        driver_note_->setObjectName(QStringLiteral("portReadyDriverNote"));
        driver_note_->set_action(tr("Open setup check"),
                                 [this] { Q_EMIT setup_check_requested(); });
        body_layout()->addWidget(driver_note_);
    }
    else
    {
        paragraph_ = new QLabel(this);
        paragraph_->setObjectName(QStringLiteral("portReadyParagraph"));
        paragraph_->setTextFormat(Qt::RichText);
        paragraph_->setFont(Theme::instance().font(text_style::body));
        paragraph_->setWordWrap(true);
        paragraph_->setText(
            tr("In your program, choose <b>%1</b> as the serial port. "
               "Settings such as baud rate are chosen by that program. The "
               "port stays available while Virtual Serial Studio is "
               "running.")
                .arg(port_.path.toHtmlEscaped()));
        paragraph_->setAccessibleName(
            tr("In your program, choose %1 as the serial port. Settings such "
               "as baud rate are chosen by that program. The port stays "
               "available while Virtual Serial Studio is running.")
                .arg(port_.path));
        body_layout()->addWidget(paragraph_);

        // Pseudo-terminal paths only exist while Studio runs, so say so.
        if (pseudo_terminal_platform(platform_))
        {
            auto* note = new NoteBanner(
                NoteBanner::kind::info,
                tr("This path exists while Virtual Serial Studio is running."),
                this);
            note->setObjectName(QStringLiteral("portReadyLinuxNote"));
            body_layout()->addWidget(note);
        }
    }

    // Card showing whether another program has the port open. refresh_peer()
    // fills it and hides it for Studio-only ports.
    peer_card_ = new QFrame(this);
    peer_card_->setObjectName(QStringLiteral("portReadyPeer"));
    peer_card_->setProperty("card", true);
    polish(peer_card_);
    auto* peer_layout = new QHBoxLayout(peer_card_);
    peer_layout->setContentsMargins(12, 10, 14, 10);
    peer_layout->setSpacing(12);
    peer_chip_ = new StatusChip(tr("Open"), StatusChip::kind::open, peer_card_);
    peer_layout->addWidget(peer_chip_, 0, Qt::AlignLeft | Qt::AlignVCenter);
    peer_text_ = new QLabel(peer_card_);
    peer_text_->setFont(Theme::instance().font(text_style::body));
    peer_text_->setWordWrap(true);
    peer_text_->setProperty("muted", true);
    peer_layout->addWidget(peer_text_, 1);
    body_layout()->addWidget(peer_card_);

    // Collapsible details: name, kind, path, the saved-setup JSON and the
    // command-line equivalent.
    details_ = new Disclosure(tr("Show details"), this);
    details_->setObjectName(QStringLiteral("portReadyDetails"));
    auto* details_content = new QWidget(details_);
    auto* details_layout = new QVBoxLayout(details_content);
    details_layout->setContentsMargins(0, 0, 0, 0);
    details_layout->setSpacing(14);

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(8);
    grid->addWidget(key_label(tr("Name"), details_content), 0, 0);
    grid->addWidget(value_label(port_.name, true, details_content), 0, 1);
    grid->addWidget(key_label(tr("Kind"), details_content), 1, 0);
    grid->addWidget(value_label(kind_description(), false, details_content), 1,
                    1);
    grid->addWidget(key_label(tr("Path"), details_content), 2, 0);
    grid->addWidget(value_label(port_.path, true, details_content), 2, 1);
    grid->setColumnStretch(1, 1);
    details_layout->addLayout(grid);

    auto* json_label =
        new QLabel(tr("Entry in a saved setup (JSON)"), details_content);
    json_label->setFont(Theme::instance().font(text_style::caption));
    json_label->setProperty("muted", true);
    details_layout->addWidget(json_label);
    auto* json = new CodeBlock(details_content);
    json->set_text(setup_json(), tr("JSON"));
    details_layout->addWidget(json);

    auto* command_label =
        new QLabel(tr("Same thing from the command line"), details_content);
    command_label->setFont(Theme::instance().font(text_style::caption));
    command_label->setProperty("muted", true);
    details_layout->addWidget(command_label);
    auto* command = new CodeBlock(details_content);
    command->set_text(setup_command(), tr("Command"));
    details_layout->addWidget(command);
    details_->set_content(details_content);
    body_layout()->addWidget(details_);

    // Start expanded in advanced mode, unless a remembered choice says
    // otherwise.
    bool expanded = advanced;
    const QSettings settings;
    if (settings.contains(QString::fromLatin1(kDetailsRemembered)))
    {
        expanded =
            settings.value(QString::fromLatin1(kDetailsRemembered)).toBool();
    }
    details_->set_expanded(expanded);
    update_details_titles();
    // Remember whether the user left the details open for next time.
    connect(details_, &Disclosure::expanded_changed, this,
            [this](bool open)
            {
                QSettings().setValue(QString::fromLatin1(kDetailsRemembered),
                                     open);
                update_details_titles();
            });

    // Footer actions. Connect and Create another pass the request on to the
    // caller, then close the dialog.
    auto* connect_button =
        add_footer_button(tr("Connect %1 to something")
                              .arg(studio_only_ ? port_.name : port_.path));
    connect(connect_button, &QPushButton::clicked, this,
            [this]
            {
                Q_EMIT connect_requested(port_id_);
                accept();
            });
    auto* another = add_footer_button(tr("Create another"));
    connect(another, &QPushButton::clicked, this,
            [this]
            {
                Q_EMIT create_another_requested();
                accept();
            });
    auto* done = add_footer_button(tr("Done"), QStringLiteral("primary"));
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    // Defer making Done the default button until control returns to the
    // event loop.
    QTimer::singleShot(0, done, [done] { done->setDefault(true); });
}

void PortReadyDialog::refresh_peer()
{
    VSS_PROFILE_ZONE("gui.flows.port_ready.refresh_peer");
    if (peer_card_ == nullptr || peer_chip_ == nullptr)
        return;

    if (studio_only_)
    {
        peer_card_->setVisible(false);
        return;
    }
    peer_card_->setVisible(true);

    const bool peer_known =
        backend_.caps().peer_open && port_.peer != peer_state::unknown;
    if (peer_known && port_.peer == peer_state::not_open)
    {
        peer_chip_->set_kind(StatusChip::kind::waiting);
        peer_chip_->set_text(tr("Waiting"));
        peer_text_->setText(
            tr("No program has opened %1 yet. In your program, choose exactly "
               "%1. This panel updates when one opens it.")
                .arg(port_.path));
        peer_text_->setVisible(true);
        return;
    }
    if (peer_known && port_.peer == peer_state::open)
    {
        peer_chip_->set_kind(StatusChip::kind::connected);
        peer_chip_->set_text(tr("Program connected"));
        peer_text_->setText(tr("A program is using %1.").arg(port_.path));
        peer_text_->setVisible(true);
        return;
    }

    // The engine cannot observe an external open.  Say only what is true.
    peer_chip_->set_kind(StatusChip::kind::open);
    peer_chip_->set_text(tr("Open"));
    peer_text_->clear();
    peer_text_->setVisible(false);
}

void PortReadyDialog::update_details_titles()
{
    if (details_ == nullptr)
        return;
    details_->set_title(details_->is_expanded() ? tr("Hide details")
                                                : tr("Show details"));
}

QString PortReadyDialog::kind_description() const
{
    const QString token = virtual_kind_token();
    if (port_.path.startsWith(QStringLiteral("loopback:")))
        return tr("In-process loopback");
    if (token == QLatin1String("pty") ||
        port_.path.startsWith(QStringLiteral("/dev/")))
        return tr("Pseudo terminal");
    if (token == QLatin1String("loopback"))
        return tr("In-process loopback");
    if (token == QLatin1String("windows-driver"))
        return tr("Windows driver");
    if (token == QLatin1String("auto") &&
        port_.path.startsWith(QStringLiteral("COM")))
    {
        return backend_.facts().driver_found
                   ? tr("Auto, resolved to the Studio driver")
                   : tr("Auto, resolved to com0com");
    }
    if (port_.kind == vss::endpoint_kind::physical)
        return tr("Real device");
    return tr("Auto");
}

QString PortReadyDialog::virtual_kind_token() const
{
    if (!requested_kind_.isEmpty())
        return requested_kind_;
    if (port_.path.startsWith(QStringLiteral("/dev/")))
        return QStringLiteral("pty");
    if (port_.path.startsWith(QStringLiteral("loopback:")))
        return QStringLiteral("loopback");
    return QStringLiteral("auto");
}

QString PortReadyDialog::setup_json() const
{
    return QStringLiteral(
               "{ \"name\": \"%1\", \"kind\": \"virtual\", \"virtual_kind\": "
               "\"%2\" }")
        .arg(json_escape(port_.name), virtual_kind_token());
}

QString PortReadyDialog::setup_command() const
{
    return QStringLiteral("vss_cli virtual --kind=%1")
        .arg(virtual_kind_token());
}

QString PortReadyDialog::path() const
{
    if (copy_ != nullptr)
        return port_.path;
    return port_.path;
}

} // namespace vss::gui
