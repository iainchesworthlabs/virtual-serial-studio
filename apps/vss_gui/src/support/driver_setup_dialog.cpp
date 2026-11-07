//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/driver_setup_dialog.hpp"

#include "backend/driver_error.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>

namespace vss::gui::support
{
namespace
{

QLabel* paragraph(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::body));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

DriverSetupDialog::DriverSetupDialog(const StudioBackend& backend,
                                     driver_problem problem, QWidget* parent)
    : QDialog(parent), backend_(backend), step_(set_up_driver),
      available_(driver_setup_available()),
      shared_(std::make_shared<shared>())
{
    build(problem);
}

DriverSetupDialog::~DriverSetupDialog()
{
    if (worker_.joinable())
        worker_.join();
}

void DriverSetupDialog::set_runner(runner step, bool available)
{
    step_ = std::move(step);
    available_ = available;
    if (setup_ != nullptr)
        setup_->setEnabled(available_ && !running_);
    show_unavailable();
}

void DriverSetupDialog::show_unavailable()
{
    // Before a run, the note says why the button is off; once there is a result
    // the note belongs to it.
    if (result_ == nullptr || running_ || ran_)
        return;
    if (available_)
    {
        result_->hide();
        return;
    }
    vss::permissions::step_result none;
    none.status = vss::permissions::step_status::unsupported;
    result_->set_kind(NoteBanner::kind::warn);
    result_->set_text(report_for_driver_setup(none).message);
    result_->show();
}

void DriverSetupDialog::build(driver_problem problem)
{
    VSS_PROFILE_ZONE("gui.support.driver_setup.build");
    VSS_PROFILE_MESSAGE("gui.dialog.driver_setup.opened");
    const QString title =
        QCoreApplication::translate("DriverSetup", "Set up Virtual Serial Studio");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 0);
    layout->setSpacing(12);

    add_dialog_heading(
        layout, title,
        problem == driver_problem::none
            ? QCoreApplication::translate(
                  "DriverSetup",
                  "Install or repair the driver and helper this PC needs.")
            : driver_problem_title(problem),
        this);

    layout->addWidget(paragraph(
        QCoreApplication::translate(
            "DriverSetup",
            "Windows only lets a driver make COM ports that other programs "
            "can open. Setup installs the Virtual Serial Studio driver and a "
            "small helper service that makes ports for you, so administrator "
            "rights are needed once, now, and not each time you add a port."),
        this));
    layout->addWidget(paragraph(
        QCoreApplication::translate(
            "DriverSetup",
            "Windows will show one permission prompt. It names the program "
            "and its publisher: choose Yes if they match. Studio itself keeps "
            "running without administrator rights. Nothing is downloaded: the "
            "files come from this copy of Virtual Serial Studio."),
        this));
    layout->addWidget(paragraph(
        QCoreApplication::translate(
            "DriverSetup",
            "The helper restarts, so ports that are open now are closed. "
            "Studio offers to make them again."),
        this));

    const QString directory = driver_package_dir();
    if (!directory.isEmpty())
    {
        package_ = paragraph(
            QCoreApplication::translate("DriverSetup", "Files: %1")
                .arg(directory),
            this);
        layout->addWidget(package_);
    }

    result_ = new NoteBanner(NoteBanner::kind::info, {}, this);
    result_->hide();
    layout->addWidget(result_);

    show_unavailable();
    if (available_ && !driver_setup_command().isEmpty())
    {
        layout->addWidget(paragraph(
            QCoreApplication::translate(
                "DriverSetup",
                "The same setup as a command, if you prefer to run it "
                "yourself in a terminal opened as administrator:"),
            this));
        auto* block = new CodeBlock(this);
        block->set_text(
            driver_setup_command(),
            QCoreApplication::translate("DriverSetup", "Command"));
        layout->addWidget(block);
    }
    layout->addStretch(1);

    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);

    copy_ = make_dialog_button(
        QCoreApplication::translate("DriverSetup", "Copy details"), "quiet",
        footer);
    copy_->setAccessibleName(
        QCoreApplication::translate("DriverSetup", "Copy details"));
    connect(copy_, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(details_text()); });
    footer_layout->addWidget(copy_);

    setup_ = make_dialog_button(title, "primary", footer);
    setup_->setAccessibleName(title);
    setup_->setAutoDefault(false);
    setup_->setEnabled(available_);
    setup_->setToolTip(QCoreApplication::translate(
        "DriverSetup",
        "Windows will ask for administrator permission before anything is "
        "installed."));
    connect(setup_, &QPushButton::clicked, this, [this] { begin(); });
    footer_layout->addWidget(setup_);

    close_ = make_dialog_button(
        QCoreApplication::translate("DriverSetup", "Close"), nullptr, footer);
    close_->setDefault(true);
    close_->setAccessibleName(
        QCoreApplication::translate("DriverSetup", "Close"));
    connect(close_, &QPushButton::clicked, this, &QDialog::reject);
    footer_layout->addWidget(close_);
    layout->addWidget(footer);

    timer_ = new QTimer(this);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });

    (available_ ? setup_ : close_)->setFocus(Qt::OtherFocusReason);
}

void DriverSetupDialog::begin()
{
    VSS_PROFILE_ZONE("gui.support.driver_setup.begin");
    if (!available_ || running_ || !step_)
        return;
    running_ = true;
    ran_ = true;
    setup_->setEnabled(false);
    close_->setEnabled(false);
    result_->set_kind(NoteBanner::kind::info);
    result_->set_text(QCoreApplication::translate(
        "DriverSetup",
        "Setting up. Answer the Windows permission prompt, then wait: this "
        "can take a minute."));
    result_->show();

    if (worker_.joinable())
        worker_.join();
    shared_ = std::make_shared<shared>();
    auto state = shared_;
    const runner step = step_;
    const StudioBackend* backend = &backend_;
    worker_ = std::thread(
        [state, step, backend]
        {
            VSS_PROFILE_THREAD("vss.gui.driver_setup");
            outcome done;
            done.report = step();
            // Ask again, so the dialog says what is true now and not what the
            // script claimed.
            done.check = backend->check_driver();
            state->done = std::move(done);
            state->finished.store(true, std::memory_order_release);
        });
    timer_->start();
}

void DriverSetupDialog::poll()
{
    if (!shared_ || !shared_->finished.load(std::memory_order_acquire))
        return;
    timer_->stop();
    if (worker_.joinable())
        worker_.join();
    running_ = false;
    close_->setEnabled(true);
    last_ = shared_->done;
    show_outcome(last_);
}

void DriverSetupDialog::show_outcome(const outcome& done)
{
    const bool installed = done.report.installed();
    ready_ = installed && done.check.problem == driver_problem::none;

    QString text = done.report.message;
    NoteBanner::kind kind = NoteBanner::kind::warn;
    if (ready_)
    {
        kind = NoteBanner::kind::ok;
    }
    else if (installed &&
             done.report.result == driver_setup_result::done)
    {
        // The script said yes and the check disagrees: say so, and what the
        // check found, and do not claim success.
        text = QCoreApplication::translate(
            "DriverSetup",
            "Setup finished, but this PC still cannot make ports: %1. Copy the "
            "details if you ask for help.")
                   .arg(driver_problem_title(done.check.problem));
    }
    result_->set_kind(kind);
    result_->set_text(text);
    result_->show();

    if (done.report.result == driver_setup_result::done_restart)
        result_->set_kind(NoteBanner::kind::info);

    // Another try is allowed after a failure or a declined prompt; after a
    // success the button has nothing left to do.
    setup_->setEnabled(available_ && !ready_);
    if (setup_->isEnabled())
        setup_->setFocus(Qt::OtherFocusReason);
    else
        close_->setFocus(Qt::OtherFocusReason);

    if (ready_)
        Q_EMIT set_up_finished();
}

QString DriverSetupDialog::details_text() const
{
    QString text;
    if (!last_.check.details.isEmpty())
        text += QStringLiteral("check: %1\n").arg(last_.check.details);
    if (last_.report.exit_code >= 0)
        text += QStringLiteral("set-up script exit code: %1\n")
                    .arg(last_.report.exit_code);
    if (const QString command = driver_setup_command(); !command.isEmpty())
        text += QStringLiteral("command: %1\n").arg(command);
    const QString before = last_driver_details();
    if (!before.isEmpty())
        text += QStringLiteral("before set-up: %1\n").arg(before);
    return text;
}

void DriverSetupDialog::reject()
{
    // The set-up has the machine in its hands until it ends.
    if (running_)
        return;
    QDialog::reject();
}

} // namespace vss::gui::support
