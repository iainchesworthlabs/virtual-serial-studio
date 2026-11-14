//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "dialogs/common.hpp"
#include "language/bidi.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

QString parity_letter(vss::parity_mode parity)
{
    switch (parity)
    {
    case vss::parity_mode::none:
        return QStringLiteral("N");
    case vss::parity_mode::odd:
        return QStringLiteral("O");
    case vss::parity_mode::even:
        return QStringLiteral("E");
    case vss::parity_mode::mark:
        return QStringLiteral("M");
    case vss::parity_mode::space:
        return QStringLiteral("S");
    }
    return QStringLiteral("N");
}

QString stop_label(vss::stop_bits_mode stop)
{
    switch (stop)
    {
    case vss::stop_bits_mode::one:
        return QStringLiteral("1");
    case vss::stop_bits_mode::one_point_five:
        return QStringLiteral("1.5");
    case vss::stop_bits_mode::two:
        return QStringLiteral("2");
    }
    return QStringLiteral("1");
}

} // namespace

QString serial_short_label(const vss::serial_settings& settings)
{
    return QStringLiteral("%1 %2%3%4")
        .arg(QString::number(settings.baud_rate))
        .arg(QString::number(settings.data_bits))
        .arg(parity_letter(settings.parity))
        .arg(stop_label(settings.stop_bits));
}

QString serial_settings_summary(const vss::serial_settings& settings)
{
    QString parity;
    switch (settings.parity)
    {
    case vss::parity_mode::none:
        parity = QObject::tr("no parity");
        break;
    case vss::parity_mode::odd:
        parity = QObject::tr("odd parity");
        break;
    case vss::parity_mode::even:
        parity = QObject::tr("even parity");
        break;
    case vss::parity_mode::mark:
        parity = QObject::tr("mark parity");
        break;
    case vss::parity_mode::space:
        parity = QObject::tr("space parity");
        break;
    }

    QString flow;
    switch (settings.flow_control)
    {
    case vss::flow_control_mode::none:
        flow = QObject::tr("no flow control");
        break;
    case vss::flow_control_mode::software:
        flow = QObject::tr("software flow control (XON and XOFF)");
        break;
    case vss::flow_control_mode::hardware:
        flow = QObject::tr("hardware flow control (RTS and CTS)");
        break;
    }

    return QObject::tr("%1 baud, %2 data bits, %3, %4 stop %5, %6")
        .arg(QString::number(settings.baud_rate),
             QString::number(settings.data_bits), parity,
             stop_label(settings.stop_bits),
             settings.stop_bits == vss::stop_bits_mode::one
                 ? QObject::tr("bit")
                 : QObject::tr("bits"),
             flow);
}

QString serial_uri(const QString& path, const vss::serial_settings& settings)
{
    QString uri = QStringLiteral("serial://%1?baud=%2")
                      .arg(path, QString::number(settings.baud_rate));
    if (settings.data_bits != 8)
        uri += QStringLiteral("&data=%1").arg(settings.data_bits);
    if (settings.parity != vss::parity_mode::none)
    {
        const auto parity = [](vss::parity_mode mode) -> const char*
        {
            switch (mode)
            {
            case vss::parity_mode::odd:
                return "odd";
            case vss::parity_mode::even:
                return "even";
            case vss::parity_mode::mark:
                return "mark";
            case vss::parity_mode::space:
                return "space";
            case vss::parity_mode::none:
                break;
            }
            return "none";
        };
        uri += QStringLiteral("&parity=%1")
                   .arg(QString::fromLatin1(parity(settings.parity)));
    }
    if (settings.stop_bits != vss::stop_bits_mode::one)
        uri += QStringLiteral("&stop=%1").arg(stop_label(settings.stop_bits));
    if (settings.flow_control != vss::flow_control_mode::none)
        uri +=
            QStringLiteral("&flow=%1")
                .arg(settings.flow_control == vss::flow_control_mode::hardware
                         ? QStringLiteral("hardware")
                         : QStringLiteral("software"));
    return uri;
}

QString port_combo_label(const port_view& port)
{
    if (port.path.startsWith(QStringLiteral("loopback:")))
        return QObject::tr("%1 (Studio only)").arg(isolate_name(port.name));
    if (port.path.isEmpty() || port.path == port.name)
        return isolate_name(port.name);
    return QObject::tr("%1 (%2)")
        .arg(isolate_name(port.name), isolate_name(port.path));
}

bool windows_platform(const setup_facts& facts)
{
    if (!facts.platform.isEmpty())
        return facts.platform == QLatin1String("windows");
#if defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

QString join_dialog_names(const QStringList& names)
{
    if (names.isEmpty())
        return {};
    if (names.size() == 1)
        return names.first();
    if (names.size() == 2)
        return QObject::tr("%1 and %2").arg(names.at(0), names.at(1));
    QString result;
    for (int index = 0; index + 1 < names.size(); ++index)
    {
        if (index > 0)
            result += QStringLiteral(", ");
        result += names.at(index);
    }
    return QObject::tr("%1 and %2").arg(result, names.last());
}

QString html_escape(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    escaped.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    escaped.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return escaped;
}

QString bold_name(const QString& text)
{
    return QStringLiteral("<b>%1</b>").arg(html_escape(isolate_name(text)));
}

QFont semibold(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

void prepare_dialog(QDialog* dialog, const QString& accessible_title, int width,
                    int height)
{
    dialog->setWindowTitle(accessible_title);
    dialog->setAccessibleName(accessible_title);
    dialog->setProperty("surface", QStringLiteral("panel"));
    polish(dialog);
    dialog->resize(width, height);
    dialog->setMinimumSize(std::max(480, width / 2), 360);
}

QFrame* make_dialog_footer(QWidget* parent, QHBoxLayout*& layout)
{
    auto* footer = new QFrame(parent);
    footer->setObjectName(QStringLiteral("dialogFooter"));
    footer->setAttribute(Qt::WA_StyledBackground);
    footer->setFrameShape(QFrame::NoFrame);
    layout = new QHBoxLayout(footer);
    set_dialog_footer_margins(layout);
    // The dialogs that use this footer inset their whole layout by the dialog
    // padding already, so the footer adds none at the sides.
    const QMargins margins = layout->contentsMargins();
    layout->setContentsMargins(0, margins.top(), 0, margins.bottom());
    layout->addStretch(1);
    return footer;
}

QPushButton* make_dialog_action_button(const QString& text, const char* role,
                                       QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    if (role != nullptr)
        set_role(button, role);
    return button;
}

QLabel* make_dialog_title(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::title));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QLabel* make_dialog_subtitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::body));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setProperty("secondary", true);
    polish(label);
    return label;
}

QLabel* make_field_label(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(semibold(Theme::instance().font(text_style::caption)));
    return label;
}

void fill_port_combo(QComboBox* combo, const std::vector<port_view>& ports)
{
    VSS_PROFILE_ZONE("gui.dialogs.fill_port_combo");
    combo->clear();
    for (const port_view& port : ports)
    {
        combo->addItem(port_combo_label(port),
                       QVariant::fromValue<qulonglong>(port.id));
    }
}

vss::endpoint_id combo_port_id(const QComboBox* combo)
{
    if (combo == nullptr || combo->currentIndex() < 0)
        return 0;
    return static_cast<vss::endpoint_id>(combo->currentData().toULongLong());
}

bool select_combo_port(QComboBox* combo, vss::endpoint_id id)
{
    for (int index = 0; index < combo->count(); ++index)
    {
        if (static_cast<vss::endpoint_id>(
                combo->itemData(index).toULongLong()) == id)
        {
            combo->setCurrentIndex(index);
            return true;
        }
    }
    return false;
}

InlineError::InlineError(QWidget* parent) : QFrame(parent)
{
    setProperty("note", QStringLiteral("danger"));
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(8);

    auto* icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance()
                        .icon(QStringLiteral("fault"), colour_role::bad, 16)
                        .pixmap(16, 16));
    icon->setFixedSize(16, 16);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(icon, 0, Qt::AlignTop);

    auto* column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    title_ = new QLabel(this);
    title_->setFont(semibold(Theme::instance().font(text_style::body)));
    title_->setWordWrap(true);
    body_ = new QLabel(this);
    body_->setFont(Theme::instance().font(text_style::body));
    body_->setWordWrap(true);
    column->addWidget(title_);
    column->addWidget(body_);
    layout->addLayout(column, 1);

    copy_ = new QPushButton(tr("Copy details"), this);
    set_role(copy_, "quiet");
    copy_->setProperty("size", QStringLiteral("small"));
    polish(copy_);
    copy_->setCursor(Qt::PointingHandCursor);
    copy_->hide();
    layout->addWidget(copy_, 0, Qt::AlignTop);
    connect(copy_, &QPushButton::clicked, this, [this] { copy_details(); });

    hide();
    repaint_on_theme_change(this);
}

void InlineError::show_error(const QString& title, const QString& body,
                             const QString& details)
{
    title_->setText(title);
    body_->setText(body);
    details_ = details;
    copy_->setVisible(!details.isEmpty());
    copy_->setToolTip(tr("Copy details"));
    show();
    refresh_accessibility();
}

void InlineError::clear()
{
    details_.clear();
    title_->clear();
    body_->clear();
    copy_->hide();
    hide();
}

bool InlineError::is_showing() const
{
    return !isHidden();
}

QString InlineError::title() const
{
    return title_ == nullptr ? QString() : title_->text();
}

QString InlineError::body() const
{
    return body_ == nullptr ? QString() : body_->text();
}

QString InlineError::details() const
{
    return details_;
}

void InlineError::refresh_accessibility()
{
    setAccessibleName(tr("Error: %1. %2").arg(title(), body()));
    announce_name_change(this);
}

void InlineError::copy_details()
{
    QGuiApplication::clipboard()->setText(details_);
    copy_->setToolTip(tr("Copied"));
    QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())), tr("Copied"),
                       copy_);
}

} // namespace vss::gui
