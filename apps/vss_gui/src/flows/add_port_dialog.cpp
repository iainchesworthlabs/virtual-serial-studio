//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/add_port_dialog.hpp"

#include "backend/driver_error.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

QString unique_name(const QString& base, QSet<QString>& taken)
{
    QString stem =
        base.trimmed().isEmpty() ? QStringLiteral("port") : base.trimmed();
    if (!taken.contains(stem))
    {
        taken.insert(stem);
        return stem;
    }
    for (int suffix = 2;; ++suffix)
    {
        const QString candidate = QStringLiteral("%1-%2").arg(stem).arg(suffix);
        if (!taken.contains(candidate))
        {
            taken.insert(candidate);
            return candidate;
        }
    }
}

QString plural_names(const QVector<QString>& names)
{
    if (names.size() <= 1)
        return names.value(0);
    if (names.size() == 2)
        return QCoreApplication::translate("AddPortDialog", "%1 and %2")
            .arg(names.at(0), names.at(1));
    if (names.size() == 3)
        return QCoreApplication::translate("AddPortDialog", "%1, %2 and %3")
            .arg(names.at(0), names.at(1), names.at(2));
    return QCoreApplication::translate("AddPortDialog", "%1, %2 and so on")
        .arg(names.at(0), names.at(1));
}

} // namespace

AddPortDialog::AddPortDialog(StudioBackend& backend, bool advanced,
                             mode initial, platform_kind platform,
                             QWidget* parent)
    : FlowDialog(tr("Add a port"), tr("What should the new port be?"), parent),
      backend_(backend), platform_(platform)
{
    build(advanced, initial);

    auto* cancel = add_footer_button(tr("Cancel"));
    cancel_ = cancel;
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* primary =
        add_footer_button(tr("Create port"), QStringLiteral("primary"));
    connect(primary, &QPushButton::clicked, this,
            [this]
            {
                if (mode_ == mode::virtual_port)
                    create_virtual_ports();
                else if (mode_ == mode::linked_pair)
                    create_pair();
                else if (mode_ == mode::real_device)
                {
                    wants_attach_ = true;
                    accept();
                }
            });

    set_mode(initial);
    finish_layout(780);
    if (initial == mode::linked_pair && pair_panel_ != nullptr)
        focus_first(pair_panel_->pair_name_edit());
    else
        focus_first(name_);

    QWidget::setTabOrder(cards_.value(0)->radio(), name_);
    QWidget::setTabOrder(name_, count_);
    QWidget::setTabOrder(count_, kind_);
    QWidget::setTabOrder(kind_, cancel_);
    QWidget::setTabOrder(cancel_, primary_button());
    if (pair_panel_ != nullptr)
    {
        QWidget::setTabOrder(pair_panel_->pair_name_edit(),
                             pair_panel_->first_port_edit());
        QWidget::setTabOrder(pair_panel_->first_port_edit(),
                             pair_panel_->second_port_edit());
        QWidget::setTabOrder(pair_panel_->second_port_edit(), cancel_);
    }
}

void AddPortDialog::build(bool advanced, mode initial)
{
    VSS_PROFILE_ZONE("gui.flows.add_port.build");
    VSS_PROFILE_MESSAGE("gui.dialog.add_port.opened");
    advanced_ = advanced;

    auto* cards_layout = new QVBoxLayout();
    cards_layout->setContentsMargins(0, 0, 0, 0);
    cards_layout->setSpacing(12);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);

    const auto add_card = [this, cards_layout,
                           group](mode value, const QString& title,
                                  const QString& description,
                                  bool advanced_only) -> PickCard*
    {
        auto* card = new PickCard(title, description, this);
        card->setObjectName(
            QStringLiteral("addPortCard%1").arg(static_cast<int>(value)));
        cards_.push_back(card);
        group->addButton(card->radio(), static_cast<int>(value));
        card->setVisible(!advanced_only || advanced_);
        cards_layout->addWidget(card);
        return card;
    };

    add_card(mode::virtual_port, tr("Virtual port"),
             tr("A port that other programs can open. Use it when a program "
                "needs a COM port and this PC has none."),
             false);
    add_card(mode::linked_pair, tr("Linked pair"),
             tr("Two ports wired together like a null-modem cable, so two "
                "programs can talk to each other."),
             false);
    add_card(mode::real_device, tr("Real device"),
             tr("Open a serial device that is plugged into this PC, so you "
                "can share it."),
             false);
    add_card(mode::network_endpoint, tr("Network endpoint"),
             tr("A TCP, UDP or RFC 2217 address, used as a port."), true);
    add_card(mode::mock_fixture, tr("Mock fixture"),
             tr("A pretend device you can script, for testing without "
                "hardware."),
             true);

    auto* more = new QFrame(this);
    more->setObjectName(QStringLiteral("addPortMore"));
    auto* more_layout = new QHBoxLayout(more);
    more_layout->setContentsMargins(10, 10, 12, 10);
    more_layout->setSpacing(10);
    auto* more_title = new QLabel(tr("More types"), more);
    more_title->setFont(
        flow_semibold(Theme::instance().font(text_style::body)));
    more_layout->addWidget(more_title);
    more_layout->addWidget(new Badge(tr("ADVANCED"), more));
    more_types_ = new QLabel(
        tr("Network endpoint and mock fixture. Turn on Advanced mode to add "
           "them."),
        more);
    more_types_->setFont(Theme::instance().font(text_style::body));
    more_types_->setProperty("muted", true);
    more_types_->setWordWrap(true);
    more_layout->addWidget(more_types_, 1);
    advanced_button_ = new QPushButton(tr("Turn on Advanced mode"), more);
    advanced_button_->setProperty("role", QStringLiteral("quiet"));
    advanced_button_->setProperty("size", QStringLiteral("small"));
    advanced_button_->setCursor(Qt::PointingHandCursor);
    advanced_button_->setAccessibleName(tr("Turn on Advanced mode"));
    polish(advanced_button_);
    connect(advanced_button_, &QPushButton::clicked, this,
            &AddPortDialog::advanced_requested);
    more_layout->addWidget(advanced_button_);
    more_row_ = more;
    cards_layout->addWidget(more);

    const auto apply_more_style = [more]
    {
        more->setStyleSheet(
            QStringLiteral("QFrame#addPortMore { background: %1; border: 1px "
                           "dashed %2; border-radius: 6px; }")
                .arg(Theme::instance()
                         .colour(colour_role::panel)
                         .name(QColor::HexRgb),
                     Theme::instance()
                         .colour(colour_role::control)
                         .name(QColor::HexRgb)));
    };
    apply_more_style();
    connect(&Theme::instance(), &Theme::theme_changed, this, apply_more_style);

    body_layout()->addLayout(cards_layout);
    body_layout()->addSpacing(4);

    settings_title_ = new QLabel(this);
    settings_title_->setObjectName(QStringLiteral("addPortSettingsTitle"));
    settings_title_->setFont(
        flow_semibold(Theme::instance().font(text_style::body)));
    body_layout()->addWidget(settings_title_);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("addPortPages"));
    virtual_page_ = new QWidget(pages_);
    pair_page_ = new QWidget(pages_);
    real_page_ = new QWidget(pages_);
    network_page_ = new QWidget(pages_);
    mock_page_ = new QWidget(pages_);
    pages_->addWidget(virtual_page_);
    pages_->addWidget(pair_page_);
    pages_->addWidget(real_page_);
    pages_->addWidget(network_page_);
    pages_->addWidget(mock_page_);
    body_layout()->addWidget(pages_);

    build_virtual_page();
    build_pair_page();
    build_real_page();
    build_placeholder_page(mode::network_endpoint);
    build_placeholder_page(mode::mock_fixture);

    connect(group, &QButtonGroup::idClicked, this,
            [this](int id) { set_mode(static_cast<mode>(id)); });

    if (initial == mode::network_endpoint || initial == mode::mock_fixture)
        set_advanced(true);
    set_advanced(advanced);
}

void AddPortDialog::build_virtual_page()
{
    auto* layout = new QVBoxLayout(virtual_page_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    name_ = new QLineEdit(QStringLiteral("port-1"), virtual_page_);
    name_->setObjectName(QStringLiteral("addPortName"));
    name_->setAccessibleName(tr("Name"));
    name_row_ = new FormRow(tr("Name"), name_, {}, virtual_page_);

    count_ = new QSpinBox(virtual_page_);
    count_->setObjectName(QStringLiteral("addPortCount"));
    count_->setRange(0, 999);
    count_->setValue(1);
    count_->setMinimumWidth(150);
    count_->setAccessibleName(tr("How many"));
    count_row_ = new FormRow(tr("How many"), count_, {}, virtual_page_);

    auto* name_and_count = new QHBoxLayout();
    name_and_count->setSpacing(12);
    name_and_count->addWidget(name_row_, 1);
    name_and_count->addWidget(count_row_, 0);
    layout->addLayout(name_and_count);

    kind_ = new QComboBox(virtual_page_);
    kind_->setObjectName(QStringLiteral("addPortKind"));
    kind_->addItem(tr("Best available (recommended)"), 0);
    kind_->addItem(tr("Pseudo terminal"), 1);
    kind_->addItem(tr("In-process loopback"), 2);
    kind_->setAccessibleName(tr("Kind"));
    if (windows_platform(platform_))
    {
        if (auto* model = qobject_cast<QStandardItemModel*>(kind_->model()))
        {
            if (QStandardItem* item = model->item(1))
            {
                item->setEnabled(false);
                item->setToolTip(tr("Not available in this build"));
            }
        }
    }
    com_ = new QSpinBox(virtual_page_);
    com_->setObjectName(QStringLiteral("addPortCom"));
    com_->setRange(0, 255);
    com_->setSpecialValueText(tr("Automatic"));
    com_->setPrefix(QStringLiteral("COM"));
    com_->setValue(0);
    com_->setMinimumWidth(150);
    com_->setAccessibleName(tr("COM number"));
    com_row_ = new FormRow(
        tr("COM number"), com_,
        tr("Programs that open a fixed name such as COM3 find the port again "
           "every run. Automatic takes a free number above the ports this PC "
           "already has, never COM1 or COM2. A number needs the Studio driver "
           "and its helper service, and is used when one port is made."),
        virtual_page_);
    layout->addWidget(com_row_);

    layout->addWidget(new FormRow(
        tr("Kind"), kind_,
        tr("Best available uses the Studio driver or com0com on Windows, and a "
           "pseudo terminal on Linux and macOS. Up to 32 at once. Several "
           "ports are named port-1, port-2 and so on."),
        virtual_page_));

    result_ = new ResultSentence(virtual_page_);
    result_->setObjectName(QStringLiteral("addPortResult"));
    layout->addWidget(result_);
    layout->addStretch(1);

    connect(name_, &QLineEdit::textChanged, this,
            [this]
            {
                name_row_->clear_error();
                refresh_result();
            });
    connect(count_, &QSpinBox::valueChanged, this,
            [this]
            {
                validate_count();
                refresh_com_row();
                refresh_result();
            });
    connect(kind_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                refresh_com_row();
                refresh_result();
            });
    connect(com_, &QSpinBox::valueChanged, this,
            [this]
            {
                com_row_->clear_error();
                refresh_result();
            });
    refresh_com_row();
    refresh_result();
}

void AddPortDialog::build_pair_page()
{
    auto* layout = new QVBoxLayout(pair_page_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    pair_panel_ = new PairPanel(backend_, advanced_, platform_, pair_page_);
    pair_panel_->setObjectName(QStringLiteral("addPortPairPanel"));
    layout->addWidget(pair_panel_);
    connect(pair_panel_, &PairPanel::state_changed, this,
            [this]
            {
                if (mode_ == mode::linked_pair)
                    set_primary_enabled(pair_panel_->can_create());
            });
    connect(pair_panel_, &PairPanel::advanced_requested, this,
            &AddPortDialog::advanced_requested);
    connect(pair_panel_, &PairPanel::setup_check_requested, this,
            &AddPortDialog::setup_check_requested);
}

void AddPortDialog::build_real_page()
{
    auto* layout = new QVBoxLayout(real_page_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* note = new NoteBanner(
        NoteBanner::kind::info,
        tr("Next chooses the device and its serial settings. Studio opens the "
           "device once and creates the port your programs open."),
        real_page_);
    note->setObjectName(QStringLiteral("addPortRealNote"));
    layout->addWidget(note);
    layout->addStretch(1);
}

void AddPortDialog::build_placeholder_page(mode value)
{
    QWidget* page =
        value == mode::network_endpoint ? network_page_ : mock_page_;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* note = new NoteBanner(
        NoteBanner::kind::info,
        value == mode::network_endpoint
            ? tr("Network endpoints arrive in a later work package. Nothing "
                 "has been created.")
            : tr("Mock fixtures arrive in a later work package. Nothing has "
                 "been created."),
        page);
    layout->addWidget(note);
    layout->addStretch(1);
}

void AddPortDialog::set_advanced(bool advanced)
{
    advanced_ = advanced;
    if (pair_panel_ != nullptr)
        pair_panel_->set_advanced(advanced);
    for (int index = 0; index < cards_.size(); ++index)
    {
        const bool advanced_only = index >= 3;
        cards_.at(index)->setVisible(!advanced_only || advanced_);
    }
    if (more_row_ != nullptr)
        more_row_->setVisible(!advanced_);
    if (!advanced_ &&
        (mode_ == mode::network_endpoint || mode_ == mode::mock_fixture))
    {
        set_mode(mode::virtual_port);
    }
    refresh_mode();
}

void AddPortDialog::set_mode(mode value)
{
    mode_ = value;
    const int index = static_cast<int>(value);
    if (index >= 0 && index < cards_.size())
    {
        cards_.at(index)->set_selected(true);
        if (value == mode::network_endpoint || value == mode::mock_fixture)
            set_advanced(true);
    }
    refresh_mode();
}

void AddPortDialog::refresh_mode()
{
    VSS_PROFILE_ZONE("gui.flows.add_port.refresh_mode");
    if (pages_ == nullptr)
        return;
    pages_->setCurrentIndex(static_cast<int>(mode_));

    QString title;
    switch (mode_)
    {
    case mode::virtual_port:
        title = tr("Virtual port settings");
        break;
    case mode::linked_pair:
        title = tr("Pair settings");
        break;
    case mode::real_device:
        title = tr("Real device settings");
        break;
    case mode::network_endpoint:
        title = tr("Network endpoint settings");
        break;
    case mode::mock_fixture:
        title = tr("Mock fixture settings");
        break;
    }
    settings_title_->setText(title);

    QString primary;
    bool enabled = true;
    switch (mode_)
    {
    case mode::virtual_port:
        primary = tr("Create port");
        break;
    case mode::linked_pair:
        primary = tr("Create pair");
        enabled = pair_panel_ != nullptr && pair_panel_->can_create();
        break;
    case mode::real_device:
        primary = tr("Next");
        break;
    case mode::network_endpoint:
    case mode::mock_fixture:
        primary = tr("Create port");
        enabled = false;
        break;
    }
    set_primary_text(primary);
    set_primary_enabled(enabled);

    if (isVisible())
    {
        if (mode_ == mode::linked_pair && pair_panel_ != nullptr)
            focus_first(pair_panel_->pair_name_edit());
        else if (mode_ == mode::virtual_port)
            focus_first(name_);
        else
            focus_first(primary_button());
    }
}

void AddPortDialog::refresh_result()
{
    VSS_PROFILE_ZONE("gui.flows.add_port.refresh_result");
    if (result_ == nullptr)
        return;
    const QVector<QString> names = planned_names();
    if (names.isEmpty())
        return;
    if (names.size() == 1)
    {
        result_->set_result_text(
            tr("This creates one port named <b>%1</b>. Studio then shows the "
               "name to open in your program, such as COM7.")
                .arg(names.at(0).toHtmlEscaped()),
            tr("This creates one port named %1. Studio then shows the name to "
               "open in your program, such as COM7.")
                .arg(names.at(0)));
        return;
    }
    const QString display = plural_names(names);
    const int count = static_cast<int>(names.size());
    result_->set_result_text(
        tr("This creates %1 ports named <b>%2</b>. Studio then shows the "
           "names to open in your programs, such as COM7 and COM8.")
            .arg(count)
            .arg(display.toHtmlEscaped()),
        tr("This creates %1 ports named %2. Studio then shows the names to "
           "open in your programs, such as COM7 and COM8.")
            .arg(count)
            .arg(display));
}

bool AddPortDialog::com_number_visible() const
{
    return windows_platform(platform_) && backend_.caps().com_numbers &&
           kind_ != nullptr && kind_->currentData().toInt() == 0;
}

void AddPortDialog::refresh_com_row()
{
    if (com_row_ == nullptr)
        return;
    com_row_->setVisible(com_number_visible());
    // One number names one port, so a batch takes Automatic.
    const bool single = count_ == nullptr || count_->value() == 1;
    com_->setEnabled(single);
    if (!single)
        com_->setValue(0);
}

void AddPortDialog::validate_count()
{
    if (count_row_ == nullptr)
        return;
    const int value = count_->value();
    if (value < 1 || value > 32)
    {
        count_row_->set_error(tr("Choose a number from 1 to 32."));
        return;
    }
    count_row_->clear_error();
}

QVector<QString> AddPortDialog::planned_names() const
{
    if (name_ == nullptr)
        return {};
    QSet<QString> taken;
    for (const auto& port : backend_.ports())
        taken.insert(port.name);
    const QString base = name_->text().trimmed().isEmpty()
                             ? QStringLiteral("port")
                             : name_->text().trimmed();
    const int count =
        std::clamp(count_ == nullptr ? 1 : count_->value(), 1, 32);
    if (count == 1)
        return {unique_name(base, taken)};

    QString stem = base;
    static const QRegularExpression numbered(QStringLiteral("^(.*?)-[0-9]+$"));
    const auto match = numbered.match(base);
    if (match.hasMatch() && !match.captured(1).isEmpty())
        stem = match.captured(1);

    QVector<QString> names;
    names.reserve(count);
    for (int index = 1; index <= count; ++index)
    {
        names.push_back(
            unique_name(QStringLiteral("%1-%2").arg(stem).arg(index), taken));
    }
    return names;
}

QString AddPortDialog::validation_error() const
{
    if (count_ != nullptr && (count_->value() < 1 || count_->value() > 32))
        return tr("Choose a number from 1 to 32.");
    if (name_ == nullptr || name_->text().trimmed().isEmpty())
        return tr("Enter a name for the port.");
    return {};
}

void AddPortDialog::create_virtual_ports()
{
    VSS_PROFILE_ZONE("gui.flows.add_port.create_virtual");
    validate_count();
    name_row_->clear_error();
    if (!validation_error().isEmpty())
    {
        if (name_->text().trimmed().isEmpty())
            name_row_->set_error(tr("Enter a name for the port."));
        if (count_->value() < 1 || count_->value() > 32)
            count_->setFocus(Qt::OtherFocusReason);
        return;
    }

    const int kind = kind_->currentData().toInt();
    const unsigned com_number = com_number_visible() && com_->isEnabled()
                                    ? static_cast<unsigned>(com_->value())
                                    : 0U;
    com_row_->clear_error();
    const QVector<QString> names = planned_names();
    QVector<vss::endpoint_id> created;
    for (const QString& target : names)
    {
        vss::result<vss::endpoint_id> added =
            com_number != 0
                ? backend_.create_driver_port(target, true, com_number)
            : kind == 1 ? backend_.create_virtual_port(
                            target, vss::virtual_port_kind::pty, true)
            : kind == 2 ? backend_.create_virtual_port(
                              target, vss::virtual_port_kind::loopback, true)
                        : backend_.create_auto_port(target, true);
        if (!added)
        {
            for (const vss::endpoint_id id : created)
                static_cast<void>(backend_.remove_port(id));
            // A missing driver, a stopped helper, an old helper and a refused
            // user all come back as the same few codes. Ask what is wrong, so
            // the message names it and offers the set-up.
            const vss::error_code failure =
                kind == 2 ? added.error()
                          : driver_aware_error(backend_, added.error());
            const bool setup_problem = driver_problem_of(failure).has_value();
            if (com_number != 0 && !setup_problem)
            {
                // The refusal is about the number, so it is said beside the
                // number and the form stays as it was.
                com_row_->set_error(
                    com_number_refusal(failure, com_number, false));
                com_->setFocus(Qt::OtherFocusReason);
                Q_EMIT error_shown(tr("Could not create the port"));
                return;
            }
            flow_error error;
            error.title = created.size() + 1 == 1
                              ? tr("Could not create the port")
                              : tr("Could not create the ports");
            error.body = error_body(
                failure,
                tr("Studio could not create the port. Nothing was changed."),
                tr(" Try a different name or kind, then try again."));
            error.details = error_details(failure);
            apply_driver_problem(error, failure);
            show_error(error);
            return;
        }
        created.push_back(*added);
    }

    created_ports_ = created;
    created_kind_ = kind == 1   ? QStringLiteral("pty")
                    : kind == 2 ? QStringLiteral("loopback")
                                : QStringLiteral("auto");
    backend_.refresh();
    accept();
}

void AddPortDialog::create_pair()
{
    VSS_PROFILE_ZONE("gui.flows.add_port.create_pair");
    flow_error error;
    auto outcome = pair_panel_->create_pair(&error);
    if (!outcome)
    {
        if (!error.title.isEmpty())
            show_error(error);
        return;
    }
    pair_outcome_ = std::move(outcome);
    created_ports_ = {pair_outcome_->first, pair_outcome_->second};
    created_kind_ = QStringLiteral("pair");
    accept();
}

void AddPortDialog::show_error(const flow_error& error)
{
    Q_EMIT error_shown(error.title);
    ErrorDialog dialog(error.title, error.body, error.details, this);
    bool set_up = false;
    if (error.offer_setup)
    {
        dialog.add_action(
            tr("Set up Virtual Serial Studio"),
            [&dialog, &set_up]
            {
                set_up = true;
                dialog.accept();
            },
            QStringLiteral("primary"));
    }
    dialog.exec();
    // Said after the error is gone, so the two dialogs do not stack.
    if (set_up)
        Q_EMIT driver_setup_requested();
}

PickCard* AddPortDialog::mode_card(mode value) const
{
    const int index = static_cast<int>(value);
    if (index < 0 || index >= cards_.size())
        return nullptr;
    return cards_.at(index);
}

} // namespace vss::gui
