//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Update dialog (docs/gui-design/docs/screens/12-updates.md, "Update").
// It reads one object, the `UpdateService`, and follows its `changed()` and
// `download_progress()` signals: what is on show is a function of the
// service's state, so the same dialog is right whether it is opened for a new
// offer, during a download or on a file that is already checked. It never
// quits the program and never touches a port; the service and the window do
// that.
//

#pragma once

#include "backend/studio_backend.hpp"
#include "updates/package_install.hpp"
#include "updates/update_service.hpp"

#include <vss/update/feed.hpp>

#include <QDialog>
#include <QString>
#include <QUrl>

#include <functional>
#include <initializer_list>

class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QWidget;

namespace vss::gui
{
class CodeBlock;
class NoteBanner;
} // namespace vss::gui

namespace vss::gui::support
{

class UpdateDialog final : public QDialog
{
    Q_OBJECT

public:
    /// What the dialog asks of the system. The program uses the defaults;
    /// a test replaces them so nothing leaves the test. A function left
    /// empty gets its default.
    struct hooks
    {
        /// Opens an address in the browser. Default: QDesktopServices::openUrl.
        std::function<void(const QUrl&)> open_url;
        /// Shows the folder that holds `path` in the system's file manager.
        /// Default: opens the folder with QDesktopServices::openUrl.
        std::function<void(const QString& path)> show_in_folder;
        /// Default: updates::package_install_available.
        std::function<bool(vss::update::file_kind)> package_install_available;
        /// Default: updates::install_package.
        std::function<void(
            vss::update::file_kind, const QString& path,
            const vss::update::sha512_digest&, QObject* context,
            std::function<void(updates::package_install_result)>)>
            install_package;
        /// How many ports are running now. Default: counted from the backend.
        std::function<int()> running_ports;
    };

    UpdateDialog(updates::UpdateService& service, StudioBackend& backend,
                 QWidget* parent = nullptr);
    UpdateDialog(updates::UpdateService& service, StudioBackend& backend,
                 hooks system, QWidget* parent);

    enum class page
    {
        offer,
        downloading,
        checking,
        ready,
        failure
    };

    page current_page() const noexcept { return page_; }

Q_SIGNALS:
    /// A sentence for the window to show as a toast (after "Skip this
    /// version").
    void toast_requested(const QString& text);

protected:
    /// Escape and the title bar's close button: "Not now" before a download,
    /// "Cancel" during one (the dialog then stays open on the offer page).
    void reject() override;
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;

private:
    void build();
    void refresh();
    void show_progress(bool checking);
    void update_progress();
    void show_offer();
    void show_ready();
    void show_failure(const QString& title, const QString& body,
                      bool can_retry);
    void set_buttons(std::initializer_list<QPushButton*> visible,
                     QPushButton* primary);
    void fit_to_content();
    void fit_summary();
    void close_when_no_offer();
    bool has_offer() const;
    void announce_progress(int percent);

    void skip_version();
    void cancel_download();
    void open_downloads_page();
    void install_now();
    void install_package();
    void on_package_result(updates::package_install_result result);
    void show_in_folder();
    void try_again();
    void close_failure();

    updates::UpdateService& service_;
    StudioBackend& backend_;
    hooks system_;
    page page_ = page::offer;

    /// The installer could not be started; the failure page is shown over a
    /// `ready` service.
    bool start_failed_ = false;
    /// A package install is running.
    bool installing_ = false;
    /// What the last package install came to, shown on the ready page.
    enum class package_note
    {
        none,
        done,
        failed
    };
    package_note package_note_ = package_note::none;
    /// The highest ten-percent step already announced, or -1.
    int announced_step_ = -1;
    bool fitting_ = false;

    QScrollArea* scroll_ = nullptr;
    QWidget* header_ = nullptr;
    QWidget* footer_ = nullptr;
    QWidget* body_ = nullptr;
    QVBoxLayout* body_layout_ = nullptr;

    // The offer page.
    QWidget* offer_box_ = nullptr;
    QLabel* offer_sentence_ = nullptr;
    QLabel* new_heading_ = nullptr;
    QScrollArea* summary_ = nullptr;
    QLabel* summary_text_ = nullptr;
    QLabel* notes_link_ = nullptr;
    QLabel* file_line_ = nullptr;
    NoteBanner* ports_note_ = nullptr;

    // The download and the check.
    QWidget* progress_box_ = nullptr;
    QLabel* progress_text_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* amount_ = nullptr;

    // The checked file.
    QWidget* ready_box_ = nullptr;
    QLabel* ready_text_ = nullptr;
    CodeBlock* command_ = nullptr;
    NoteBanner* done_note_ = nullptr;
    NoteBanner* failed_note_ = nullptr;

    // A failure.
    QWidget* failure_box_ = nullptr;
    QLabel* failure_title_ = nullptr;
    QLabel* failure_body_ = nullptr;

    // The footer, left to right.
    QPushButton* skip_ = nullptr;
    QPushButton* not_now_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* show_folder_ = nullptr;
    QPushButton* download_ = nullptr;
    QPushButton* downloads_page_ = nullptr;
    QPushButton* install_now_ = nullptr;
    QPushButton* open_image_ = nullptr;
    QPushButton* install_package_ = nullptr;
    QPushButton* try_again_ = nullptr;
    QPushButton* close_ = nullptr;
};

} // namespace vss::gui::support
