//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "backend/studio_backend.hpp"
#include "support/elevated_step.hpp"

#include <QDialog>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

class QLabel;
class QPushButton;
class QTimer;

namespace vss::gui
{
class NoteBanner;
}

namespace vss::gui::support
{

/// "Set up Virtual Serial Studio": the one guided action that installs or
/// repairs the driver and the helper service on Windows.
///
/// The dialog is the user's agreement. It says what is wrong (the heading), what
/// the set-up installs and why, that Windows will show one permission prompt and
/// what to check in it, that nothing is downloaded, and what happens to ports
/// that are open. Nothing is changed until the button is pressed. The set-up
/// runs on a thread of its own, so the window keeps answering through the
/// prompt, and when it ends the dialog asks the backend again and says whether
/// the PC is now ready.
class DriverSetupDialog final : public QDialog
{
    Q_OBJECT

public:
    using runner = std::function<driver_setup_report()>;

    /// `problem` is what the last check found; it only decides the heading.
    DriverSetupDialog(const StudioBackend& backend, driver_problem problem,
                      QWidget* parent = nullptr);
    ~DriverSetupDialog() override;

    /// Replaces the step that the button runs, for tests; and says whether the
    /// build can set up at all. The default is `set_up_driver` and
    /// `driver_setup_available()`.
    void set_runner(runner step, bool available);

    /// True once a run ended with the PC ready.
    bool is_ready() const noexcept { return ready_; }
    /// True while the set-up runs.
    bool is_running() const noexcept { return running_; }

    QPushButton* setup_button() const noexcept { return setup_; }
    QPushButton* copy_button() const noexcept { return copy_; }
    NoteBanner* result_note() const noexcept { return result_; }

    /// Presses "Set up Virtual Serial Studio".
    void begin();

    void reject() override;

Q_SIGNALS:
    /// The set-up ended and the check afterwards found nothing wrong.
    void set_up_finished();

private:
    struct outcome
    {
        driver_setup_report report;
        driver_check check;
    };

    void build(driver_problem problem);
    void poll();
    void show_unavailable();
    void show_outcome(const outcome& done);
    QString details_text() const;

    const StudioBackend& backend_;
    runner step_;
    bool available_ = false;

    QPushButton* setup_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* close_ = nullptr;
    NoteBanner* result_ = nullptr;
    QLabel* package_ = nullptr;

    // The worker writes `done_` once and the GUI thread reads it after `flag_`
    // says so; the timer on the GUI thread polls `flag_`.
    struct shared
    {
        std::atomic<bool> finished{false};
        outcome done;
    };
    std::shared_ptr<shared> shared_;
    std::thread worker_;
    QTimer* timer_ = nullptr;
    bool running_ = false;
    bool ran_ = false;
    bool ready_ = false;
    outcome last_;
};

} // namespace vss::gui::support
