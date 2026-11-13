//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/add_fixture_dialog.hpp"

#include "advanced/chrome.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace vss::gui
{
AddFixtureDialog::AddFixtureDialog(StudioBackend& backend,
                                   vss::endpoint_id selected_port,
                                   QWidget* parent)
    : QDialog(parent), backend_(backend), selected_port_(selected_port)
{
    build();
    update_result();
}

void AddFixtureDialog::build()
{
    VSS_PROFILE_ZONE("gui.advanced.add_fixture.build");
    VSS_PROFILE_MESSAGE("gui.dialog.add_fixture.opened");
    setWindowTitle(tr("Add a mock fixture"));
    setAccessibleName(tr("Add a mock fixture"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(Theme::instance().metric(
               QStringLiteral("layout.dialogWidths.standard")),
           620);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 0);
    layout->setSpacing(14);
    layout->addWidget(make_header(
        tr("Add a mock fixture"),
        tr("A simulated device for testing without hardware."),
        this, true, {}));

    // Name and baud rate share one row; the name column takes the spare width.
    auto* fields = new QGridLayout();
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setHorizontalSpacing(12);
    fields->setVerticalSpacing(10);

    name_ = new QLineEdit(QStringLiteral("modem"), this);
    name_->setAccessibleName(tr("Name"));
    name_row_ = new FormRow(tr("Name"), name_, {}, this);
    fields->addWidget(name_row_, 0, 0);

    baud_ = new QComboBox(this);
    baud_->setAccessibleName(tr("Baud rate"));
    for (int value : {115200, 9600, 57600})
        baud_->addItem(QString::number(value), value);
    auto* baud_row = new FormRow(tr("Baud rate"), baud_, {}, this);
    fields->addWidget(baud_row, 0, 1);
    fields->setColumnStretch(0, 1);
    fields->setColumnMinimumWidth(1, 200);
    layout->addLayout(fields);

    // The port to attach the fixture to.  Id 0 stands for "no port", so the
    // fixture can be added now and linked later.
    target_ = new QComboBox(this);
    target_->setAccessibleName(tr("Connect it to"));
    target_->addItem(tr("Nothing yet"), static_cast<qulonglong>(0));
    const auto ports = backend_.ports();
    for (const auto& port : ports)
    {
        const QString label =
            port.path.isEmpty()
                ? tr("%1, two-way").arg(port.name)
                : tr("%1 (%2), two-way").arg(port.name, port.path);
        target_->addItem(label, static_cast<qulonglong>(port.id));
    }
    // Pre-select the port that was selected when the dialog was opened.
    if (selected_port_ != 0)
    {
        const auto found =
            target_->findData(static_cast<qulonglong>(selected_port_));
        if (found >= 0)
            target_->setCurrentIndex(found);
    }
    auto* target_row = new FormRow(tr("Connect it to"), target_,
                                   tr("You can link it later from Connect "
                                      "ports."),
                                   this);
    layout->addWidget(target_row);

    // The banner is filled in by update_result() to say what Add will do.
    result_ = new NoteBanner(NoteBanner::kind::info, {}, this);
    layout->addWidget(result_);

    // A static card that lists the faults a fixture can raise.
    auto* card = new QFrame(this);
    card->setProperty("group", true);
    polish(card);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(14, 12, 14, 12);
    card_layout->setSpacing(6);
    auto* card_title = new QLabel(tr("What a fixture can simulate"), card);
    card_title->setFont(Theme::instance().font(text_style::caption));
    card_title->setProperty("muted", true);
    card_layout->addWidget(card_title);
    auto* card_body = new QLabel(
        tr("Parity errors, framing errors, overruns, breaks, hang-ups and "
           "disconnects. Each is reported as an error event on the port."),
        card);
    card_body->setFont(Theme::instance().font(text_style::body));
    card_body->setWordWrap(true);
    set_ink2(card_body);
    card_layout->addWidget(card_body);
    layout->addWidget(card);

    // Hidden until set_error() has a message to show.  The colour is set by
    // style sheet, so it is re-applied whenever the theme changes.
    error_ = new QLabel(this);
    error_->setFont(Theme::instance().font(text_style::caption));
    error_->setWordWrap(true);
    const auto apply_error_colour = [this]
    {
        error_->setStyleSheet(QStringLiteral("color: %1")
                                  .arg(Theme::instance()
                                           .colour(colour_role::bad)
                                           .name(QColor::HexRgb)));
    };
    apply_error_colour();
    connect(&Theme::instance(), &Theme::theme_changed, error_,
            [apply_error_colour] { apply_error_colour(); });
    error_->hide();
    layout->addWidget(error_);
    layout->addStretch(1);

    // Footer buttons: Cancel rejects; Add runs add_fixture().
    QHBoxLayout* footer_layout = nullptr;
    auto* footer = make_footer(this, footer_layout);
    auto* cancel = make_button(tr("Cancel"), nullptr, footer);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(cancel);
    add_ = make_button(tr("Add fixture"), "primary", footer);
    add_->setAccessibleName(tr("Add fixture"));
    connect(add_, &QPushButton::clicked, this,
            [this] { static_cast<void>(add_fixture()); });
    footer_layout->addWidget(add_);
    layout->addWidget(footer);

    // A backend without fixture support leaves the dialog readable but Add
    // disabled, with the standard "not available" tooltip.
    if (!backend_.caps().fixtures)
        degrade(add_);

    // Any change to an input refreshes the summary banner.
    connect(name_, &QLineEdit::textChanged, this, [this] { update_result(); });
    connect(baud_, &QComboBox::currentIndexChanged, this,
            [this] { update_result(); });
    connect(target_, &QComboBox::currentIndexChanged, this,
            [this] { update_result(); });

    // Focus the name field now, and again once the event loop runs so the
    // focus still holds after the dialog is shown.
    name_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, name_,
                       [this] { name_->setFocus(Qt::OtherFocusReason); });
}

QString AddFixtureDialog::fixture_name() const
{
    return name_ == nullptr ? QString() : name_->text().trimmed();
}

void AddFixtureDialog::set_fixture_name(const QString& name)
{
    name_->setText(name);
}

int AddFixtureDialog::baud_rate() const
{
    return baud_ == nullptr ? 115200 : baud_->currentData().toInt();
}

void AddFixtureDialog::set_baud_rate(int baud)
{
    const int index = baud_->findData(baud);
    if (index >= 0)
    {
        const QSignalBlocker blocker(baud_);
        baud_->setCurrentIndex(index);
    }
    update_result();
}

vss::endpoint_id AddFixtureDialog::target_port() const
{
    if (target_ == nullptr)
        return 0;
    return static_cast<vss::endpoint_id>(target_->currentData().toULongLong());
}

void AddFixtureDialog::set_target_port(vss::endpoint_id id)
{
    const int index = target_->findData(static_cast<qulonglong>(id));
    const QSignalBlocker blocker(target_);
    target_->setCurrentIndex(index >= 0 ? index : 0);
    update_result();
}

QString AddFixtureDialog::result_text() const
{
    return result_ == nullptr ? QString()
                              : result_->text()
                                    .remove(QStringLiteral("<b>"))
                                    .remove(QStringLiteral("</b>"));
}

void AddFixtureDialog::update_result()
{
    VSS_PROFILE_ZONE("gui.advanced.add_fixture.update_result");
    const QString name =
        fixture_name().isEmpty() ? QStringLiteral("modem") : fixture_name();
    const QString baud = QString::number(baud_rate());
    const vss::endpoint_id target = target_port();
    QString other;
    if (target != 0)
    {
        if (const auto port = backend_.port(target))
            other = port->name;
    }

    QString html;
    QString plain;
    if (!other.isEmpty())
    {
        if (backend_.caps().faults && backend_.caps().line_matrix)
        {
            html = tr("Studio adds a fixture called <b>%1</b> at %2 baud, "
                      "linked two-way to <b>%3</b>. After it exists you can "
                      "inject faults and change its line wiring from the "
                      "Inspector.")
                       .arg(name, baud, other);
            plain = tr("Studio adds a fixture called %1 at %2 baud, linked "
                       "two-way to %3. After it exists you can inject faults "
                       "and change its line wiring from the Inspector.")
                        .arg(name, baud, other);
        }
        else
        {
            html = tr("Studio adds a fixture called <b>%1</b> at %2 baud, "
                      "linked two-way to <b>%3</b>. After it exists you can "
                      "open it from the Inspector.")
                       .arg(name, baud, other);
            plain = tr("Studio adds a fixture called %1 at %2 baud, linked "
                       "two-way to %3. After it exists you can open it from "
                       "the Inspector.")
                        .arg(name, baud, other);
        }
    }
    else
    {
        html = tr("Studio adds a fixture called <b>%1</b> at %2 baud. It is "
                  "not linked to another port yet.")
                   .arg(name, baud);
        plain = tr("Studio adds a fixture called %1 at %2 baud. It is not "
                   "linked to another port yet.")
                    .arg(name, baud);
    }
    result_->set_text(html);
    result_->setAccessibleName(plain);
}

void AddFixtureDialog::set_error(const QString& text)
{
    error_->setText(text);
    error_->setAccessibleName(text);
    error_->show();
    result_->setAccessibleName(text);
}

void AddFixtureDialog::clear_error()
{
    error_->clear();
    error_->hide();
    update_result();
}

bool AddFixtureDialog::add_fixture()
{
    VSS_PROFILE_ZONE_NAMED(kProfileZone, "gui.advanced.add_fixture.add");
    VSS_PROFILE_ZONE_TEXT(kProfileZone, fixture_name().toStdString());
    clear_error();
    if (!backend_.caps().fixtures)
    {
        set_error(not_available_text());
        return false;
    }
    const QString name = fixture_name();
    if (name.isEmpty())
    {
        set_error(tr("Enter a name for the fixture."));
        return false;
    }

    vss::serial_settings settings;
    settings.baud_rate = static_cast<unsigned>(baud_rate());
    const auto created =
        backend_.create_mock_port(name, settings, QStringLiteral("scripted"));
    if (!created)
    {
        set_error(tr("Could not add the fixture: %1")
                      .arg(vss::gui::error_text(created.error())));
        return false;
    }

    const vss::endpoint_id target = target_port();
    if (target != 0)
    {
        vss::link_options options;
        options.emulate_modem_lines = true;
        options.policy = vss::merge_policy::interleave;
        const auto linked = backend_.connect_ports(*created, target, options);
        if (!linked)
        {
            static_cast<void>(backend_.remove_port(*created));
            set_error(tr("Could not link the fixture: %1")
                          .arg(vss::gui::error_text(linked.error())));
            return false;
        }
    }

    QString actual = name;
    if (const auto port = backend_.port(*created))
        actual = port->name;
    Q_EMIT message(tr("Added a mock fixture called %1.").arg(actual));
    accept();
    return true;
}

} // namespace vss::gui
