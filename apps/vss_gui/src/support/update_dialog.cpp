//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/update_dialog.hpp"

#include "support/build_info.hpp"
#include "support/update_text.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <QAccessible>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMargins>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace vss::gui::support
{
namespace
{

namespace upd = vss::update;
using updates::download_failure;
using updates::download_state;
using updates::install_start;
using updates::package_install_result;

/// The width of the dialog (screens 12, "Update").
constexpr int kDialogWidth = 620;
/// The summary shows at most this many lines and scrolls beyond them.
constexpr int kSummaryLines = 8;
/// The progress is announced once for each step of this many percent.
constexpr int kAnnounceStep = 10;
/// Space the body leaves at both sides, as the neighbouring dialogs do.
constexpr int kBodyMargin = 24;

QLabel* text_label(QWidget* parent, text_style style = text_style::body)
{
    auto* label = new QLabel(parent);
    label->setFont(Theme::instance().font(style));
    label->setWordWrap(true);
    return label;
}

QLabel* muted_label(QWidget* parent)
{
    QLabel* label = text_label(parent);
    label->setProperty("muted", true);
    polish(label);
    return label;
}

QWidget* make_box(QWidget* parent, QVBoxLayout* body)
{
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    body->addWidget(box);
    return box;
}

/// Tells assistive technology about a change, politely: it waits for the
/// person to finish what they were hearing.
void announce_politely(QWidget* widget, const QString& text)
{
    QAccessibleAnnouncementEvent event(widget, text);
    event.setPoliteness(QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

/// The folder that holds `path`, written as the system writes it.
QString folder_text(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absolutePath());
}

/// An install whose step stops Studio, so the ports stop with it.
bool install_stops_studio(const std::optional<upd::file_kind>& kind)
{
    return kind.has_value() && (*kind == upd::file_kind::windows_installer ||
                                *kind == upd::file_kind::macos_dmg);
}

bool is_package(upd::file_kind kind)
{
    return kind == upd::file_kind::linux_deb ||
           kind == upd::file_kind::linux_rpm;
}

int percent_of(const updates::snapshot& state)
{
    if (state.total == 0)
        return 0;
    const std::uint64_t scaled = std::uint64_t{100} * state.received;
    return static_cast<int>(std::min<std::uint64_t>(scaled / state.total, 100));
}

} // namespace

UpdateDialog::UpdateDialog(updates::UpdateService& service,
                           StudioBackend& backend, QWidget* parent)
    : UpdateDialog(service, backend, hooks{}, parent)
{
}

UpdateDialog::UpdateDialog(updates::UpdateService& service,
                           StudioBackend& backend, hooks system,
                           QWidget* parent)
    : QDialog(parent), service_(service), backend_(backend),
      system_(std::move(system))
{
    if (!system_.open_url)
        system_.open_url = [](const QUrl& url)
        { QDesktopServices::openUrl(url); };
    if (!system_.show_in_folder)
        system_.show_in_folder = [](const QString& path)
        {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        };
    if (!system_.package_install_available)
        system_.package_install_available = &updates::package_install_available;
    if (!system_.install_package)
        system_.install_package = &updates::install_package;
    if (!system_.running_ports)
        system_.running_ports = [this]
        { return static_cast<int>(backend_.ports().size()); };

    build();
    refresh();
    connect(&service_, &updates::UpdateService::changed, this,
            &UpdateDialog::refresh);
    connect(&service_, &updates::UpdateService::download_progress, this,
            &UpdateDialog::update_progress);
}

void UpdateDialog::build()
{
    const QString title = QCoreApplication::translate(
        "UpdateDialog", "Update Virtual Serial Studio");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(kDialogWidth);
    resize(kDialogWidth, 400);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    header_ = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header_);
    header_layout->setContentsMargins(kBodyMargin, 20, kBodyMargin, 14);
    header_layout->setSpacing(4);
    add_dialog_heading(header_layout, title, {}, header_);
    outer->addWidget(header_);

    // The body scrolls inside the dialog, so it stays usable at 200 percent
    // text.
    scroll_ = new QScrollArea(this);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setFocusPolicy(Qt::NoFocus);
    body_ = new QWidget(scroll_);
    body_layout_ = new QVBoxLayout(body_);
    body_layout_->setContentsMargins(kBodyMargin, 4, kBodyMargin, 16);
    body_layout_->setSpacing(14);
    scroll_->setWidget(body_);
    outer->addWidget(scroll_, 1);

    // -- The offer ------------------------------------------------------------
    offer_box_ = make_box(body_, body_layout_);
    auto* offer_layout = qobject_cast<QVBoxLayout*>(offer_box_->layout());
    offer_sentence_ = text_label(offer_box_);
    offer_sentence_->setObjectName(QStringLiteral("updateSentence"));
    offer_layout->addWidget(offer_sentence_);

    new_heading_ = text_label(offer_box_, text_style::heading);
    new_heading_->setObjectName(QStringLiteral("updateNewHeading"));
    new_heading_->setText(
        QCoreApplication::translate("UpdateDialog", "What is new"));
    offer_layout->addWidget(new_heading_);

    // The summary is a label that is told its text is plain, in a frame that
    // scrolls. Whatever the summary holds, it is drawn as the characters it
    // is: no markup is read and no link is made.
    summary_ = new QScrollArea(offer_box_);
    summary_->setObjectName(QStringLiteral("updateSummary"));
    summary_->setFrameShape(QFrame::NoFrame);
    summary_->setWidgetResizable(true);
    summary_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The keyboard scrolls it once it has the focus.
    summary_->setFocusPolicy(Qt::StrongFocus);
    summary_->setAccessibleName(new_heading_->text());
    summary_text_ = text_label(summary_);
    summary_text_->setObjectName(QStringLiteral("updateSummaryText"));
    summary_text_->setTextFormat(Qt::PlainText);
    summary_text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_text_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    summary_->setWidget(summary_text_);
    offer_layout->addWidget(summary_);

    notes_link_ = new QLabel(offer_box_);
    notes_link_->setObjectName(QStringLiteral("updateNotesLink"));
    notes_link_->setFont(Theme::instance().font(text_style::body));
    notes_link_->setTextFormat(Qt::RichText);
    notes_link_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    notes_link_->setText(
        QStringLiteral("<a href=\"notes\">%1</a>")
            .arg(QCoreApplication::translate("UpdateDialog", "Release notes")
                     .toHtmlEscaped()));
    connect(notes_link_, &QLabel::linkActivated, this,
            [this](const QString&)
            {
                const auto& offer = service_.state().offer;
                if (offer.has_value() && !offer->notes_url.isEmpty())
                    system_.open_url(offer->notes_url);
            });
    offer_layout->addWidget(notes_link_);

    file_line_ = muted_label(offer_box_);
    file_line_->setObjectName(QStringLiteral("updateFileLine"));
    offer_layout->addWidget(file_line_);

    ports_note_ = new NoteBanner(NoteBanner::kind::warn, {}, offer_box_);
    ports_note_->setObjectName(QStringLiteral("updatePortsNote"));
    offer_layout->addWidget(ports_note_);

    // The "Plugins" heading and a row for each plugin with an update go here,
    // between the file line and the note. The service has no plugin data yet,
    // so there is nothing to show (screens 12, "Update").

    // -- The download and the check -------------------------------------------
    progress_box_ = make_box(body_, body_layout_);
    auto* progress_layout = qobject_cast<QVBoxLayout*>(progress_box_->layout());
    progress_text_ = text_label(progress_box_);
    progress_text_->setObjectName(QStringLiteral("updateProgressText"));
    progress_layout->addWidget(progress_text_);
    bar_ = new QProgressBar(progress_box_);
    bar_->setObjectName(QStringLiteral("updateProgress"));
    bar_->setRange(0, 100);
    bar_->setValue(0);
    bar_->setTextVisible(false);
    bar_->setAccessibleName(
        QCoreApplication::translate("UpdateDialog", "Download progress"));
    progress_layout->addWidget(bar_);
    amount_ = muted_label(progress_box_);
    amount_->setObjectName(QStringLiteral("updateAmount"));
    progress_layout->addWidget(amount_);

    // -- The checked file -----------------------------------------------------
    ready_box_ = make_box(body_, body_layout_);
    auto* ready_layout = qobject_cast<QVBoxLayout*>(ready_box_->layout());
    ready_text_ = text_label(ready_box_);
    ready_text_->setObjectName(QStringLiteral("updateReadyText"));
    ready_layout->addWidget(ready_text_);
    command_ = new CodeBlock(ready_box_);
    command_->setObjectName(QStringLiteral("updateCommand"));
    ready_layout->addWidget(command_);
    // One note for each result, rather than one that changes its kind: a
    // banner keeps the text colour it was first drawn with.
    done_note_ = new NoteBanner(
        NoteBanner::kind::ok,
        QCoreApplication::translate("UpdateDialog",
                                    "Restart Studio to use the new version."),
        ready_box_);
    done_note_->setObjectName(QStringLiteral("updateDoneNote"));
    ready_layout->addWidget(done_note_);
    failed_note_ = new NoteBanner(
        NoteBanner::kind::danger,
        QCoreApplication::translate(
            "UpdateDialog",
            "The package could not be installed. Use the command above."),
        ready_box_);
    failed_note_->setObjectName(QStringLiteral("updateFailedNote"));
    ready_layout->addWidget(failed_note_);

    // -- A failure ------------------------------------------------------------
    failure_box_ = make_box(body_, body_layout_);
    auto* failure_layout = qobject_cast<QVBoxLayout*>(failure_box_->layout());
    failure_title_ = text_label(failure_box_, text_style::heading);
    failure_title_->setObjectName(QStringLiteral("updateFailureTitle"));
    failure_layout->addWidget(failure_title_);
    failure_body_ = text_label(failure_box_);
    failure_body_->setObjectName(QStringLiteral("updateFailureBody"));
    failure_body_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    failure_layout->addWidget(failure_body_);

    body_layout_->addStretch(1);

    // -- Footer, left to right ------------------------------------------------
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    footer_ = footer;
    const auto add_button =
        [footer, footer_layout](const QString& text, const char* object_name,
                                const char* role = nullptr) -> QPushButton*
    {
        QPushButton* button = make_dialog_button(text, role, footer);
        button->setObjectName(QString::fromLatin1(object_name));
        button->setAccessibleName(text);
        button->setAutoDefault(false);
        button->setVisible(false);
        footer_layout->addWidget(button);
        return button;
    };
    skip_ = add_button(
        QCoreApplication::translate("UpdateDialog", "Skip this version"),
        "updateSkip", "quiet");
    // "Skip this version" sits at the left, apart from the others.
    footer_layout->removeWidget(skip_);
    footer_layout->insertWidget(0, skip_);
    not_now_ = add_button(
        QCoreApplication::translate("UpdateDialog", "Not now"), "updateNotNow");
    cancel_ = add_button(QCoreApplication::translate("UpdateDialog", "Cancel"),
                         "updateCancel");
    show_folder_ = add_button(
        QCoreApplication::translate("UpdateDialog", "Show in folder"),
        "updateShowFolder");
    download_ = add_button(
        QCoreApplication::translate("UpdateDialog", "Download and install"),
        "updateDownload");
    downloads_page_ = add_button(
        QCoreApplication::translate("UpdateDialog", "Open the downloads page"),
        "updateDownloadsPage");
    install_now_ =
        add_button(QCoreApplication::translate("UpdateDialog", "Install now"),
                   "updateInstallNow");
    open_image_ = add_button(
        QCoreApplication::translate("UpdateDialog", "Open the disk image"),
        "updateOpenImage");
    install_package_ =
        add_button(QCoreApplication::translate("UpdateDialog", "Install"),
                   "updateInstallPackage");
    try_again_ =
        add_button(QCoreApplication::translate("UpdateDialog", "Try again"),
                   "updateTryAgain");
    close_ = add_button(QCoreApplication::translate("UpdateDialog", "Close"),
                        "updateClose");
    outer->addWidget(footer);

    connect(skip_, &QPushButton::clicked, this, &UpdateDialog::skip_version);
    connect(not_now_, &QPushButton::clicked, this,
            [this] { QDialog::reject(); });
    connect(cancel_, &QPushButton::clicked, this,
            &UpdateDialog::cancel_download);
    connect(show_folder_, &QPushButton::clicked, this,
            &UpdateDialog::show_in_folder);
    connect(download_, &QPushButton::clicked, this,
            [this] { service_.start_download(); });
    connect(downloads_page_, &QPushButton::clicked, this,
            &UpdateDialog::open_downloads_page);
    connect(install_now_, &QPushButton::clicked, this,
            &UpdateDialog::install_now);
    connect(open_image_, &QPushButton::clicked, this,
            &UpdateDialog::install_now);
    connect(install_package_, &QPushButton::clicked, this,
            &UpdateDialog::install_package);
    connect(try_again_, &QPushButton::clicked, this, &UpdateDialog::try_again);
    connect(close_, &QPushButton::clicked, this, &UpdateDialog::close_failure);
}

// -- what is on show ----------------------------------------------------------

bool UpdateDialog::has_offer() const
{
    const updates::snapshot& state = service_.state();
    if (!state.offer.has_value())
        return false;
    return state.state == updates::check_state::available ||
           state.state == updates::check_state::checking ||
           state.download != download_state::none;
}

void UpdateDialog::close_when_no_offer()
{
    // Not from inside the caller's stack: `exec` may not have started yet.
    QTimer::singleShot(0, this,
                       [this]
                       {
                           if (!has_offer())
                               QDialog::reject();
                       });
}

void UpdateDialog::refresh()
{
    if (!has_offer())
    {
        close_when_no_offer();
        return;
    }
    const updates::snapshot& state = service_.state();
    if (state.download != download_state::ready)
    {
        start_failed_ = false;
        package_note_ = package_note::none;
    }
    switch (state.download)
    {
    case download_state::none:
        show_offer();
        break;
    case download_state::downloading:
        show_progress(false);
        break;
    case download_state::verifying:
        show_progress(true);
        break;
    case download_state::ready:
        if (start_failed_)
        {
            const QString name = QFileInfo(state.ready_path).fileName();
            show_failure(
                QCoreApplication::translate("UpdateDialog",
                                            "Could not start the installer"),
                QCoreApplication::translate(
                    "UpdateDialog", "Studio could not start %1. It is in %2.")
                    .arg(name, folder_text(state.ready_path)),
                false);
        }
        else
        {
            show_ready();
        }
        break;
    case download_state::failed:
    {
        QString body;
        switch (state.download_error)
        {
        case download_failure::mismatch:
            body = QCoreApplication::translate(
                "UpdateDialog",
                "The download was not the file that was published, so "
                "it was deleted.");
            break;
        case download_failure::save_failed:
            // The dialog is not told which folder the service tried, so the
            // sentence does not name one.
            body = QCoreApplication::translate(
                "UpdateDialog", "Studio could not save the download.");
            break;
        case download_failure::unreachable:
        case download_failure::none:
            body = QCoreApplication::translate(
                "UpdateDialog",
                "Studio could not reach the update server. Check the "
                "connection and try again.");
            break;
        }
        show_failure(QCoreApplication::translate(
                         "UpdateDialog", "Could not download the update"),
                     body, true);
        break;
    }
    }
    fit_to_content();
}

void UpdateDialog::set_buttons(std::initializer_list<QPushButton*> visible,
                               QPushButton* primary)
{
    const std::array<QPushButton*, 11> all = {
        skip_,        not_now_,    cancel_,
        show_folder_, download_,   downloads_page_,
        install_now_, open_image_, install_package_,
        try_again_,   close_};
    const bool primary_was_ready = primary != nullptr && !primary->isHidden() &&
                                   primary->isEnabled() && primary->isDefault();
    for (QPushButton* button : all)
    {
        const bool shown = std::ranges::find(visible, button) != visible.end();
        button->setVisible(shown);
        button->setEnabled(!installing_);
        const bool is_primary = button == primary;
        if (is_primary)
            button->setProperty("role", QStringLiteral("primary"));
        else if (button == skip_)
            button->setProperty("role", QStringLiteral("quiet"));
        else
            button->setProperty("role", QVariant());
        polish(button);
        button->setDefault(is_primary);
    }
    // The first control to use gets the focus when the page changes, not on
    // every refresh of the same page.
    if (primary != nullptr && !primary_was_ready && primary->isEnabled())
        primary->setFocus(Qt::OtherFocusReason);
}

void UpdateDialog::show_offer()
{
    const updates::snapshot& state = service_.state();
    if (!state.offer.has_value())
        return;
    const updates::studio_offer& offer = *state.offer;
    page_ = page::offer;
    progress_box_->setVisible(false);
    ready_box_->setVisible(false);
    failure_box_->setVisible(false);
    offer_box_->setVisible(true);

    offer_sentence_->setText(
        QCoreApplication::translate("UpdateDialog",
                                    "Version %1 is available. You have %2.")
            .arg(offer.version_text, current_build_info().version));

    const bool has_summary = !offer.summary.trimmed().isEmpty();
    new_heading_->setVisible(has_summary);
    summary_->setVisible(has_summary);
    if (summary_text_->text() != offer.summary)
        summary_text_->setText(offer.summary);

    const bool has_notes =
        offer.notes_url.isValid() && !offer.notes_url.isEmpty();
    notes_link_->setVisible(has_notes);
    notes_link_->setAccessibleName(
        QCoreApplication::translate(
            "UpdateDialog",
            "Release notes for version %1, opens in your browser")
            .arg(offer.version_text));

    if (offer.file.has_value())
    {
        file_line_->setText(
            QCoreApplication::translate("UpdateDialog", "%1, %2")
                .arg(offer.file->name, update_size_text(offer.file->size)));
    }
    else
    {
        file_line_->setText(QCoreApplication::translate(
            "UpdateDialog",
            "There is no package for this kind of install. Download one "
            "from virtual-serial-studio.com."));
    }

    const int ports = system_.running_ports ? system_.running_ports() : 0;
    const bool warn = ports > 0 && offer.file.has_value() &&
                      install_stops_studio(state.install_kind);
    ports_note_->setVisible(warn);
    if (warn)
    {
        ports_note_->set_text(
            ports == 1 ? QCoreApplication::translate(
                             "UpdateDialog",
                             "%n port is running. Installing stops it.",
                             nullptr, ports)
                       : QCoreApplication::translate(
                             "UpdateDialog",
                             "%n ports are running. Installing stops them.",
                             nullptr, ports));
    }

    if (offer.file.has_value())
        set_buttons({skip_, not_now_, download_}, download_);
    else
        set_buttons({skip_, not_now_, downloads_page_}, downloads_page_);
}

void UpdateDialog::show_progress(bool checking)
{
    const updates::snapshot& state = service_.state();
    if (page_ != page::downloading && page_ != page::checking)
        announced_step_ = 0;
    page_ = checking ? page::checking : page::downloading;
    offer_box_->setVisible(false);
    ready_box_->setVisible(false);
    failure_box_->setVisible(false);
    progress_box_->setVisible(true);

    if (checking)
    {
        progress_text_->setText(QCoreApplication::translate(
            "UpdateDialog", "Checking the download..."));
    }
    else
    {
        const QString name = state.offer.has_value() && state.offer->file
                                 ? state.offer->file->name
                                 : QString();
        progress_text_->setText(
            QCoreApplication::translate("UpdateDialog", "Downloading %1...")
                .arg(name));
    }
    update_progress();
    set_buttons({cancel_}, cancel_);
    // Nothing can be pressed while the file is checked.
    cancel_->setEnabled(!checking && !installing_);
}

void UpdateDialog::update_progress()
{
    const updates::snapshot& state = service_.state();
    if (state.download != download_state::downloading &&
        state.download != download_state::verifying)
        return;
    const int percent =
        state.download == download_state::verifying ? 100 : percent_of(state);
    bar_->setValue(percent);
    amount_->setText(QCoreApplication::translate("UpdateDialog", "%1 of %2")
                         .arg(update_size_text(state.received),
                              update_size_text(state.total)));
    announce_progress(percent);
}

void UpdateDialog::announce_progress(int percent)
{
    const int step = percent / kAnnounceStep;
    if (step <= announced_step_)
        return;
    announced_step_ = step;
    announce_politely(bar_,
                      QCoreApplication::translate("UpdateDialog", "%1 percent")
                          .arg(step * kAnnounceStep));
}

void UpdateDialog::show_ready()
{
    const updates::snapshot& state = service_.state();
    page_ = page::ready;
    offer_box_->setVisible(false);
    progress_box_->setVisible(false);
    failure_box_->setVisible(false);
    ready_box_->setVisible(true);
    command_->setVisible(false);
    done_note_->setVisible(false);
    failed_note_->setVisible(false);

    const std::optional<upd::file_kind>& kind = state.install_kind;
    if (kind == upd::file_kind::windows_installer)
    {
        ready_text_->setText(QCoreApplication::translate(
            "UpdateDialog", "The update is ready. Studio will close and "
                            "the installer will start. Windows asks for "
                            "an administrator."));
        set_buttons({not_now_, install_now_}, install_now_);
    }
    else if (kind == upd::file_kind::macos_dmg)
    {
        ready_text_->setText(QCoreApplication::translate(
            "UpdateDialog",
            "The update is ready. Quit Studio, then drag Virtual Serial "
            "Studio onto Applications in the window that opens."));
        set_buttons({not_now_, open_image_}, open_image_);
    }
    else if (kind.has_value() && is_package(*kind))
    {
        ready_text_->setText(QCoreApplication::translate(
            "UpdateDialog", "The update is ready. Install it with this "
                            "command, then start Studio again."));
        command_->set_text(
            updates::package_install_command(*kind, state.ready_path),
            QCoreApplication::translate("UpdateDialog", "Install command"));
        command_->setVisible(true);
        const bool installed = package_note_ == package_note::done;
        const bool can_install =
            !installed && system_.package_install_available(*kind);
        done_note_->setVisible(installed);
        failed_note_->setVisible(package_note_ == package_note::failed);
        if (can_install)
            set_buttons({show_folder_, install_package_, close_},
                        install_package_);
        else
            set_buttons({show_folder_, close_}, close_);
    }
    else
    {
        ready_text_->setText(
            QCoreApplication::translate(
                "UpdateDialog",
                "The download is in %1. This copy of Studio was unpacked from "
                "an archive, so unpack the new one in its place.")
                .arg(folder_text(state.ready_path)));
        set_buttons({show_folder_, close_}, show_folder_);
    }
}

void UpdateDialog::show_failure(const QString& title, const QString& body,
                                bool can_retry)
{
    page_ = page::failure;
    offer_box_->setVisible(false);
    progress_box_->setVisible(false);
    ready_box_->setVisible(false);
    failure_box_->setVisible(true);
    failure_title_->setText(title);
    failure_body_->setText(body);
    if (can_retry)
        set_buttons({try_again_, close_}, try_again_);
    else
        set_buttons({show_folder_, close_}, show_folder_);
}

// -- size ---------------------------------------------------------------------

void UpdateDialog::fit_summary()
{
    if (summary_->isHidden())
        return;
    const int most =
        kSummaryLines *
        std::max(1, QFontMetrics(summary_text_->font()).lineSpacing());
    const QMargins around = summary_->contentsMargins();
    const int full = std::max(width(), kDialogWidth) - 2 * kBodyMargin -
                     around.left() - around.right();
    int text_height = summary_text_->heightForWidth(std::max(50, full));
    if (text_height > most)
    {
        // It will scroll, and the bar takes some of the width.
        const int narrower =
            full - summary_->verticalScrollBar()->sizeHint().width();
        text_height = summary_text_->heightForWidth(std::max(50, narrower));
    }
    summary_->setFixedHeight(std::min(text_height, most) + around.top() +
                             around.bottom());
}

void UpdateDialog::fit_to_content()
{
    if (fitting_)
        return;
    fitting_ = true;
    fit_summary();

    const int width_now = std::max(width(), kDialogWidth);
    const int body_width = width_now;
    body_layout_->activate();
    const int content = body_layout_->hasHeightForWidth()
                            ? body_layout_->totalHeightForWidth(body_width)
                            : body_layout_->sizeHint().height();
    const QScreen* screen = this->screen() != nullptr
                                ? this->screen()
                                : QGuiApplication::primaryScreen();
    const int available =
        screen != nullptr ? screen->availableGeometry().height() : 600;
    QVBoxLayout* outer = qobject_cast<QVBoxLayout*>(layout());
    const int header_height = outer->itemAt(0)->hasHeightForWidth()
                                  ? outer->itemAt(0)->heightForWidth(width_now)
                                  : header_->sizeHint().height();
    const int chrome = header_height + footer_->sizeHint().height();
    const int most = std::max(80, available * 9 / 10 - chrome);
    const int wanted = std::clamp(content, 80, most);
    if (scroll_->minimumHeight() != wanted)
        scroll_->setMinimumHeight(wanted);
    outer->activate();
    const QSize hint = minimumSizeHint();
    resize(std::max(width(), hint.width()), hint.height());
    fitting_ = false;
}

void UpdateDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    fit_to_content();
}

bool UpdateDialog::event(QEvent* event)
{
    const bool handled = QDialog::event(event);
    // Text that grows (the person's text size, or a test's) asks the layout
    // again; the height the body is given follows it.
    if (event->type() == QEvent::LayoutRequest && !fitting_ && isVisible() &&
        has_offer())
    {
        QTimer::singleShot(0, this, [this] { fit_to_content(); });
    }
    return handled;
}

// -- what the buttons do ------------------------------------------------------

void UpdateDialog::reject()
{
    const download_state download = service_.state().download;
    if (download == download_state::downloading ||
        download == download_state::verifying)
    {
        // Escape and the title bar's close button cancel; they never leave a
        // download running behind a closed window.
        cancel_download();
        return;
    }
    QDialog::reject();
}

void UpdateDialog::skip_version()
{
    const auto& offer = service_.state().offer;
    if (!offer.has_value())
        return;
    const QString version = offer->version_text;
    service_.skip_offered_version();
    Q_EMIT toast_requested(
        QCoreApplication::translate("UpdateDialog",
                                    "Studio will not mention %1 again.")
            .arg(version));
    QDialog::accept();
}

void UpdateDialog::cancel_download()
{
    service_.cancel_download();
    refresh();
}

void UpdateDialog::open_downloads_page()
{
    system_.open_url(QUrl(QString::fromLatin1(updates::kDownloadsPage)));
    QDialog::accept();
}

void UpdateDialog::install_now()
{
    switch (service_.install_ready_file())
    {
    case install_start::started:
        QDialog::accept();
        return;
    case install_start::declined:
        // The person said no to Windows: nothing is wrong, so nothing is shown.
        return;
    case install_start::failed:
    case install_start::not_supported:
        start_failed_ = true;
        refresh();
        return;
    }
}

void UpdateDialog::install_package()
{
    const updates::snapshot& state = service_.state();
    if (!state.install_kind.has_value() || !state.offer.has_value() ||
        !state.offer->file.has_value())
        return;
    installing_ = true;
    package_note_ = package_note::none;
    refresh();
    system_.install_package(
        *state.install_kind, state.ready_path, state.offer->file->sha512, this,
        [this](package_install_result result) { on_package_result(result); });
}

void UpdateDialog::on_package_result(package_install_result result)
{
    installing_ = false;
    switch (result)
    {
    case package_install_result::done:
        package_note_ = package_note::done;
        break;
    case package_install_result::declined:
        package_note_ = package_note::none;
        break;
    case package_install_result::failed:
    case package_install_result::not_available:
        package_note_ = package_note::failed;
        break;
    }
    refresh();
}

void UpdateDialog::show_in_folder()
{
    system_.show_in_folder(service_.state().ready_path);
}

void UpdateDialog::try_again()
{
    service_.clear_download_failure();
    service_.start_download();
}

void UpdateDialog::close_failure()
{
    service_.clear_download_failure();
    QDialog::accept();
}

} // namespace vss::gui::support
