//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/com_number_dialog.hpp"

#include "backend/com_numbers.hpp"
#include "flows/common.hpp"
#include "widgets/controls.hpp"

#include <vss/core/profile.hpp>

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

/// How many free numbers the list offers.  Anything else can be typed.
constexpr std::size_t kOffered = 48;

QString com_text(unsigned number)
{
    return QStringLiteral("COM%1").arg(number);
}

} // namespace

ComNumberDialog::ComNumberDialog(StudioBackend& backend, vss::endpoint_id port,
                                 QWidget* parent)
    : FlowDialog(tr("Change COM number"),
                 tr("Programs open the port by this number."), parent),
      backend_(backend), port_(port)
{
    const auto view = backend_.port(port);
    old_number_ = view ? view->com_number : 0U;

    auto* layout = new QVBoxLayout();
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    number_ = new QComboBox(this);
    number_->setObjectName(QStringLiteral("comNumber"));
    number_->setEditable(true);
    number_->setInsertPolicy(QComboBox::NoInsert);
    number_->setMaxVisibleItems(10);
    number_->setMinimumWidth(150);
    number_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("(?:[Cc][Oo][Mm])?[0-9]{0,3}")),
        number_));
    number_->setAccessibleName(tr("COM number"));
    number_row_ = new FormRow(
        tr("New COM number"), number_,
        tr("Windows restarts the port to change its number, so no program "
           "may have it open. The list holds free numbers. You can type "
           "another."),
        this);
    layout->addWidget(number_row_);

    auto* uses_label = new QLabel(tr("Numbers in use"), this);
    uses_label->setObjectName(QStringLiteral("comNumberUsesLabel"));
    uses_label->setProperty("muted", true);
    layout->addWidget(uses_label);
    uses_ = new QListWidget(this);
    uses_->setObjectName(QStringLiteral("comNumberUses"));
    uses_->setSelectionMode(QAbstractItemView::NoSelection);
    uses_->setAccessibleName(tr("Numbers in use"));
    uses_->setMinimumHeight(120);
    uses_label->setBuddy(uses_);
    layout->addWidget(uses_, 1);
    body_layout()->addLayout(layout, 1);

    load_numbers();

    auto* cancel = add_footer_button(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* apply =
        add_footer_button(tr("Change number"), QStringLiteral("primary"));
    connect(apply, &QPushButton::clicked, this, &ComNumberDialog::change);

    connect(number_, &QComboBox::currentTextChanged, this,
            [this] { update_state(); });

    update_state();
    finish_layout(500);
    setMinimumHeight(440);
    focus_first(number_);

    QWidget::setTabOrder(number_, uses_);
    QWidget::setTabOrder(uses_, cancel);
    QWidget::setTabOrder(cancel, apply);
}

unsigned ComNumberDialog::number() const
{
    QString text = number_->currentText().trimmed();
    if (text.startsWith(QLatin1String("com"), Qt::CaseInsensitive))
        text.remove(0, 3);
    bool ok = false;
    const unsigned value = text.toUInt(&ok);
    return ok ? value : 0U;
}

void ComNumberDialog::set_number(unsigned number)
{
    number_->setEditText(com_text(number));
}

void ComNumberDialog::load_numbers()
{
    in_use_ = com_number_uses(backend_, port_);
    const QString typed = number_->currentText();

    const QSignalBlocker block(number_);
    number_->clear();
    for (const unsigned candidate :
         free_com_numbers(in_use_, 1, kOffered + 1))
    {
        if (candidate != old_number_)
            number_->addItem(com_text(candidate));
    }
    number_->setEditText(typed.isEmpty() && old_number_ != 0
                             ? com_text(old_number_)
                             : typed);

    uses_->clear();
    for (const auto& [value, use] : in_use_)
    {
        uses_->addItem(tr("%1: %2").arg(com_text(value), com_use_text(use)));
    }
    if (old_number_ != 0)
    {
        uses_->insertItem(
            0, tr("%1: this port now").arg(com_text(old_number_)));
    }
}

ComNumberDialog::~ComNumberDialog()
{
    if (worker_.joinable())
        worker_.join();
}

QString ComNumberDialog::com_name() const
{
    const auto view = backend_.port(port_);
    return view ? view->path : QString();
}

void ComNumberDialog::done(int result)
{
    // The restart cannot be taken back half way, so the dialog stays until the
    // answer is in.
    if (busy_)
        return;
    FlowDialog::done(result);
}

QString ComNumberDialog::problem() const
{
    const auto view = backend_.port(port_);
    if (!view || !view->driver_port || !backend_.caps().com_numbers)
        return tr("Not available in this build");
    if (view->peer == peer_state::open)
    {
        return com_number_refusal(vss::make_error_code(vss::errc::device_busy),
                                  number(), true);
    }
    const unsigned value = number();
    if (value < 1 || value > kLastGuiComNumber)
        return tr("Choose a number from 1 to 255.");
    if (value == old_number_)
        return {};
    if (const auto found = in_use_.find(value); found != in_use_.end())
    {
        return tr("%1 is used by %2. Choose a free number.")
            .arg(com_text(value), com_use_text(found->second));
    }
    return {};
}

void ComNumberDialog::update_state()
{
    number_row_->clear_error();
    const QString reason = problem();
    if (!reason.isEmpty())
        number_row_->set_error(reason);
    set_primary_enabled(reason.isEmpty() && !busy_ && number() != old_number_);
}

void ComNumberDialog::change()
{
    VSS_PROFILE_ZONE("gui.flows.com_number.change");
    if (busy_)
        return;
    const unsigned number = this->number();
    const QString reason = problem();
    if (!reason.isEmpty() || number == old_number_)
        return;

    if (worker_.joinable())
        worker_.join();
    busy_ = true;
    number_->setEnabled(false);
    set_primary_text(tr("Changing"));
    set_primary_enabled(false);
    number_row_->clear_error();

    // The worker calls the backend and posts a plain value back; it touches no
    // widget.  The destructor joins it before the dialog goes.
    worker_ = std::thread(
        [this, number]
        {
            const auto outcome = backend_.set_com_number(port_, number);
            QMetaObject::invokeMethod(
                this, [this, number, outcome] { on_finished(number, outcome); },
                Qt::QueuedConnection);
        });
}

void ComNumberDialog::on_finished(unsigned number,
                                  const vss::result<QString>& outcome)
{
    VSS_PROFILE_ZONE("gui.flows.com_number.finished");
    if (worker_.joinable())
        worker_.join();
    busy_ = false;
    number_->setEnabled(true);
    set_primary_text(tr("Change number"));

    if (!outcome)
    {
        load_numbers();
        update_state();
        number_row_->set_error(com_number_refusal(outcome.error(), number, true));
        number_->setFocus(Qt::OtherFocusReason);
        set_primary_enabled(true);
        Q_EMIT error_shown(tr("Could not change the COM number"));
        return;
    }
    backend_.refresh();
    accept();
}

} // namespace vss::gui
