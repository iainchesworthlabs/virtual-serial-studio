//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/licence_notice.hpp"

#include "support/licence_dialog.hpp"
#include "widgets/common.hpp"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QTimer>

namespace vss::gui
{
namespace
{

namespace lic = vss::license;

/// "Dismiss" on the notice of a licence that is ending hides it until this
/// many days before the end. A dismissal is kept with the licence it was
/// for, so a renewed licence starts clean.
constexpr int remind_days_before_end = 7;

QString date_text(const QDate& date)
{
    return date.toString(Qt::ISODate);
}

QDate date_of(const QVariant& value)
{
    return QDate::fromString(value.toString(), Qt::ISODate);
}

QString licence_id_of(const lic::status& status)
{
    return status.value ? QString::fromStdString(status.value->licence_id)
                        : QString();
}

/// True while `today` is before the stored `from` date of a dismissal made
/// for this licence.
bool still_hidden(const QSettings& settings, const QString& date_key,
                  const QString& licence_key, const QString& licence_id,
                  const QDate& today)
{
    if (settings.value(licence_key).toString() != licence_id)
        return false;
    const QDate from = date_of(settings.value(date_key));
    return from.isValid() && today < from;
}

} // namespace

LicenceNotice::LicenceNotice(StudioBackend& backend, QWidget* parent)
    : NoteBanner(parent), backend_(backend)
{
    setObjectName(QStringLiteral("licenceNotice"));
    set_action(QCoreApplication::translate("LicenceNotice", "Licence"),
               [this] { Q_EMIT licence_requested(); });

    // The second action sits beside the first, in the same style.
    second_ = new QPushButton(this);
    second_->setObjectName(QStringLiteral("licenceNoticeSecond"));
    set_role(second_, "quiet");
    second_->setProperty("size", QStringLiteral("small"));
    polish(second_);
    second_->setCursor(Qt::PointingHandCursor);
    if (auto* row = qobject_cast<QHBoxLayout*>(layout()))
        row->addWidget(second_, 0, Qt::AlignVCenter);
    connect(second_, &QPushButton::clicked, this, [this] { dismiss(); });
    hide();

    connect(&backend_, &StudioBackend::licence_changed, this,
            [this] { refresh(); });
    // A reminder for tomorrow comes back while Studio keeps running.
    timer_ = new QTimer(this);
    timer_->setInterval(60 * 60 * 1000);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    timer_->start();
    refresh();
}

void LicenceNotice::set_today(std::function<QDate()> today)
{
    today_ = std::move(today);
    refresh();
}

QDate LicenceNotice::today() const
{
    return today_ ? today_() : QDate::currentDate();
}

bool LicenceNotice::due(const support::licence_notice_text& notice,
                        const lic::status& status)
{
    QSettings settings;
    const QString id = licence_id_of(status);
    const QDate now = today();
    switch (notice.kind)
    {
    case support::notice_kind::expiring:
        return !still_hidden(settings,
                             QStringLiteral("licence/expiringShowFrom"),
                             QStringLiteral("licence/expiringFor"), id, now);
    case support::notice_kind::grace:
        return !still_hidden(settings,
                             QStringLiteral("licence/graceRemindFrom"),
                             QStringLiteral("licence/graceFor"), id, now);
    case support::notice_kind::clock_suspect:
        return !still_hidden(settings,
                             QStringLiteral("licence/clockHiddenUntil"),
                             QStringLiteral("licence/clockFor"), id, now);
    case support::notice_kind::expired:
        // Shown once: on the first start after the grace period. It stays up
        // until it is dismissed, and is not shown again for this licence.
        if (expired_open_ == id && !id.isEmpty())
            return true;
        if (settings.value(QStringLiteral("licence/expiredShownFor"))
                    .toString() == id &&
            !id.isEmpty())
            return false;
        settings.setValue(QStringLiteral("licence/expiredShownFor"), id);
        settings.setValue(QStringLiteral("licence/expiredShownOn"),
                          date_text(now));
        expired_open_ = id;
        return true;
    }
    return false;
}

void LicenceNotice::refresh()
{
    const lic::status status = backend_.licence().status;
    const auto notice = support::notice_for(status);
    if (!notice || !due(*notice, status))
    {
        hide();
        shown_kind_.reset();
        shown_text_.clear();
        return;
    }
    // Setting the text announces the banner, so do it once per notice.
    if (!shown_kind_ || *shown_kind_ != notice->kind ||
        shown_text_ != notice->text)
    {
        set_kind(support::banner_kind(notice->tone));
        set_text(notice->text);
        second_->setText(support::notice_second_action(notice->kind));
        second_->setAccessibleName(second_->text());
    }
    shown_kind_ = notice->kind;
    shown_text_ = notice->text;
    show();
}

void LicenceNotice::dismiss()
{
    if (!shown_kind_)
        return;
    const lic::status status = backend_.licence().status;
    const QString id = licence_id_of(status);
    const QDate now = today();
    QSettings settings;
    switch (*shown_kind_)
    {
    case support::notice_kind::expiring:
    {
        // Hidden until 7 days before the end, then until the last day.
        const int left = status.days_left;
        QDate from;
        if (left > remind_days_before_end)
            from = now.addDays(left - remind_days_before_end);
        else if (left > 0)
            from = now.addDays(left);
        else
            from = now.addDays(1);
        settings.setValue(QStringLiteral("licence/expiringFor"), id);
        settings.setValue(QStringLiteral("licence/expiringShowFrom"),
                          date_text(from));
        break;
    }
    case support::notice_kind::grace:
        settings.setValue(QStringLiteral("licence/graceFor"), id);
        settings.setValue(QStringLiteral("licence/graceRemindFrom"),
                          date_text(now.addDays(1)));
        break;
    case support::notice_kind::clock_suspect:
        settings.setValue(QStringLiteral("licence/clockFor"), id);
        settings.setValue(QStringLiteral("licence/clockHiddenUntil"),
                          date_text(now.addDays(1)));
        break;
    case support::notice_kind::expired:
        expired_open_.clear();
        break;
    }
    refresh();
}

} // namespace vss::gui
