//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Update dialog (docs/gui-design/docs/screens/12-updates.md, "Update").
// The dialog reads the real `UpdateService`, which is driven with the fakes of
// the service's own tests (a fake transport, clock, probe and checked file),
// so every page and every failure is shown from here and nothing leaves the
// test. What the dialog asks of the system (the browser, the file manager,
// the package tool, the count of running ports) goes through `hooks`, which
// record the calls.
//

#include "fixture.hpp"
#include "qt_input.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/build_info.hpp"
#include "support/update_dialog.hpp"
#include "support/update_text.hpp"
#include "theme/theme.hpp"
#include "updates/package_install.hpp"
#include "widgets/notes.hpp"

#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QList>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace vss::gui::updates;
using namespace vss::gui::updates::testing;
using vss::gui::FakeStudioBackend;
using vss::gui::support::update_size_text;
using vss::gui::support::UpdateDialog;

namespace
{

using vss::gui::updates::package_install_result;

// -- what the hooks saw
// --------------------------------------------------------

struct package_call
{
    upd::file_kind kind = upd::file_kind::linux_deb;
    QString path;
    upd::sha512_digest sha512{};
};

struct recorded
{
    std::vector<QUrl> urls;
    std::vector<QString> folders;
    std::vector<package_call> packages;
    bool package_available = false;
    int ports = 0;
    /// The callback of the install that is running, until the test answers.
    std::function<void(package_install_result)> finish;
};

void settle()
{
    for (int turn = 0; turn < 5; ++turn)
        QCoreApplication::processEvents();
}

/// A service with its fakes, a backend and a dialog whose hooks record.
struct rig
{
    harness h;
    FakeStudioBackend backend;
    std::shared_ptr<recorded> seen = std::make_shared<recorded>();
    QStringList toasts;
    std::unique_ptr<UpdateDialog> dialog;

    UpdateDialog::hooks make_hooks() const
    {
        UpdateDialog::hooks system;
        const std::shared_ptr<recorded> log = seen;
        system.open_url = [log](const QUrl& url) { log->urls.push_back(url); };
        system.show_in_folder = [log](const QString& path)
        { log->folders.push_back(path); };
        system.package_install_available = [log](upd::file_kind)
        { return log->package_available; };
        system.install_package =
            [log](upd::file_kind kind, const QString& path,
                  const upd::sha512_digest& digest, QObject*,
                  std::function<void(package_install_result)> done)
        {
            log->packages.push_back({kind, path, digest});
            log->finish = std::move(done);
        };
        system.running_ports = [log] { return log->ports; };
        return system;
    }

    void open(bool with_ports_hook = true)
    {
        UpdateDialog::hooks system = make_hooks();
        if (!with_ports_hook)
            system.running_ports = nullptr;
        dialog = std::make_unique<UpdateDialog>(*h.service, backend,
                                                std::move(system), nullptr);
        QObject::connect(dialog.get(), &UpdateDialog::toast_requested,
                         dialog.get(), [this](const QString& text)
                         { toasts.push_back(text); });
        dialog->show();
        settle();
    }
};

std::string name_for(upd::file_kind kind)
{
    switch (kind)
    {
    case upd::file_kind::windows_installer:
        return "studio-0.3.0-windows-x86_64.exe";
    case upd::file_kind::windows_zip:
        return "studio-0.3.0-windows-x86_64.zip";
    case upd::file_kind::macos_dmg:
        return "studio-0.3.0-macos-x86_64.dmg";
    case upd::file_kind::macos_archive:
        return "studio-0.3.0-macos-x86_64.zip";
    case upd::file_kind::linux_deb:
        return "studio-0.3.0-linux-x86_64.deb";
    case upd::file_kind::linux_rpm:
        return "studio-0.3.0-linux-x86_64.rpm";
    case upd::file_kind::linux_archive:
        return "studio-0.3.0-linux-x86_64.tar.gz";
    }
    return "studio-0.3.0";
}

struct feed_options
{
    upd::file_kind kind = upd::file_kind::windows_installer;
    QByteArray bytes = payload_bytes(100000);
    std::string summary = "What is new.";
    /// The feed names a different digest from the bytes' own.
    bool wrong_digest = false;
};

/// A service with an update on offer. The test may set fields of `r.h` (a
/// failing disk, a probe) before this makes the service.
void offer(rig& r, const feed_options& options = {})
{
    release_spec spec = make_release("0.3.0", "stable", options.bytes);
    spec.files[0].kind = options.kind;
    spec.files[0].name = name_for(options.kind);
    spec.summary = options.summary;
    if (options.wrong_digest)
        spec.files[0].sha512_hex = hex_of(payload_bytes(5000, 'q'));
    r.h.probe->answer = options.kind;
    r.h.create();
    r.h.check_with(feed_body(5, {spec}));
    REQUIRE_EQ(r.h.state().state, check_state::available);
}

/// Every byte of `bytes` in pieces of `piece`, and the end of the transfer.
void deliver_all(rig& r, const QByteArray& bytes, int piece = 40000)
{
    for (qsizetype at = 0; at < bytes.size(); at += piece)
        REQUIRE(r.h.transport.send_chunk(QByteArrayView(bytes).mid(at, piece)));
    r.h.transport.finish_download(transport_error::none);
}

/// Downloads and checks the offer: the service ends `ready`.
void make_ready(rig& r, const QByteArray& bytes = payload_bytes(100000))
{
    r.h.service->start_download();
    REQUIRE_EQ(r.h.state().download, download_state::downloading);
    deliver_all(r, bytes);
    r.h.pump_download();
    REQUIRE_EQ(r.h.state().download, download_state::ready);
    settle();
}

// -- reading the dialog
// --------------------------------------------------------

template <class Widget>
Widget* part(const UpdateDialog& dialog, const char* name)
{
    auto* found = dialog.findChild<Widget*>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return found;
}

QPushButton* button(const UpdateDialog& dialog, const char* name)
{
    return part<QPushButton>(dialog, name);
}

bool shown(const UpdateDialog& dialog, const QWidget* widget)
{
    return widget->isVisibleTo(&dialog);
}

QString text_of(const UpdateDialog& dialog, const char* name)
{
    return part<QLabel>(dialog, name)->text();
}

/// The footer's buttons that are on show, left to right.
QStringList footer_texts(const UpdateDialog& dialog)
{
    QStringList texts;
    const auto* footer =
        dialog.findChild<QFrame*>(QStringLiteral("dialogFooter"));
    REQUIRE(footer != nullptr);
    for (const auto* push : footer->findChildren<QPushButton*>())
    {
        if (shown(dialog, push))
            texts << push->text();
    }
    return texts;
}

/// The text of the one visible footer button that is the default.
QString default_text(const UpdateDialog& dialog)
{
    QStringList defaults;
    for (const auto* push : dialog.findChildren<QPushButton*>())
    {
        if (shown(dialog, push) && push->isDefault())
            defaults << push->text();
    }
    REQUIRE_EQ(defaults.size(), 1);
    return defaults.front();
}

void click(const UpdateDialog& dialog, const char* name)
{
    QPushButton* push = button(dialog, name);
    REQUIRE(shown(dialog, push));
    REQUIRE(push->isEnabled());
    push->click();
    settle();
}

void press_escape(UpdateDialog& dialog)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &press);
    settle();
}

QString folder_of(const QString& path)
{
    return QDir::toNativeSeparators(QFileInfo(path).absolutePath());
}

QString this_version()
{
    return vss::gui::support::current_build_info().version;
}

/// "<file name>, <size>" for the default file of `offer`.
QString file_line(const std::string& name, std::uint64_t size)
{
    return QStringLiteral("%1, %2").arg(QString::fromStdString(name),
                                        update_size_text(size));
}

using page = UpdateDialog::page;

} // namespace

// -- the offer
// -----------------------------------------------------------------

TEST_CASE("update dialog: the offer page says both versions, what is new, the "
          "file and the buttons")
{
    rig r;
    offer(r);
    r.open();
    const UpdateDialog& d = *r.dialog;

    REQUIRE(r.dialog->current_page() == page::offer);
    REQUIRE_EQ(r.dialog->windowTitle(),
               QStringLiteral("Update Virtual Serial Studio"));
    REQUIRE_EQ(r.dialog->width(), 620);
    REQUIRE_EQ(text_of(d, "updateSentence"),
               QStringLiteral("Version 0.3.0 is available. You have %1.")
                   .arg(this_version()));
    REQUIRE_EQ(text_of(d, "updateNewHeading"), QStringLiteral("What is new"));
    REQUIRE(shown(d, part<QLabel>(d, "updateNewHeading")));
    const auto* summary = part<QScrollArea>(d, "updateSummary");
    REQUIRE(shown(d, summary));
    const auto* summary_text = part<QLabel>(d, "updateSummaryText");
    REQUIRE_EQ(summary_text->text(), QStringLiteral("What is new."));
    REQUIRE(summary_text->textFormat() == Qt::PlainText);
    REQUIRE_EQ(text_of(d, "updateFileLine"),
               file_line(name_for(upd::file_kind::windows_installer), 100000));
    REQUIRE(!shown(d, part<QWidget>(d, "updatePortsNote")));
    REQUIRE_EQ(footer_texts(d),
               QStringList({QStringLiteral("Skip this version"),
                            QStringLiteral("Not now"),
                            QStringLiteral("Download and "
                                           "install")}));
    REQUIRE_EQ(default_text(d), QStringLiteral("Download and install"));
    REQUIRE_EQ(button(d, "updateDownload")->property("role").toString(),
               QStringLiteral("primary"));
    REQUIRE_EQ(button(d, "updateSkip")->property("role").toString(),
               QStringLiteral("quiet"));
    // Nothing of the other pages is on show.
    REQUIRE(!shown(d, part<QWidget>(d, "updateProgress")));
    REQUIRE(!shown(d, part<QWidget>(d, "updateReadyText")));
    REQUIRE(!shown(d, part<QWidget>(d, "updateFailureTitle")));
    // Nothing has been asked of the service or the system.
    REQUIRE_EQ(r.h.transport.download_requests, 0);
    REQUIRE(r.seen->urls.empty());
}

TEST_CASE("update dialog: a summary is left out, with its heading, when it is "
          "empty")
{
    rig r;
    feed_options options;
    options.summary = "";
    offer(r, options);
    r.open();
    const UpdateDialog& d = *r.dialog;
    REQUIRE(!shown(d, part<QLabel>(d, "updateNewHeading")));
    REQUIRE(!shown(d, part<QScrollArea>(d, "updateSummary")));
    // The rest of the page is there.
    REQUIRE(shown(d, part<QLabel>(d, "updateSentence")));
    REQUIRE(shown(d, part<QLabel>(d, "updateFileLine")));
}

TEST_CASE("update dialog: a summary full of markup stays literal text")
{
    rig r;
    feed_options options;
    options.summary = R"(<b>bold</b> &amp; <a href=\"https://example.org\">)"
                      R"(link</a><script>x</script>)";
    offer(r, options);
    r.open();
    const UpdateDialog& d = *r.dialog;
    const auto* summary_text = part<QLabel>(d, "updateSummaryText");
    const QString literal = QStringLiteral(
        "<b>bold</b> &amp; <a href=\"https://example.org\">link</a>"
        "<script>x</script>");
    REQUIRE_EQ(summary_text->text(), literal);
    // The label is told the text is plain, so it draws every character and
    // makes no link.
    REQUIRE(summary_text->textFormat() == Qt::PlainText);
    REQUIRE(
        !(summary_text->textInteractionFlags() & Qt::LinksAccessibleByMouse));
    // The text is on no other label, and the only link on the page is the one
    // for the release notes.
    int links = 0;
    for (const auto* label : d.findChildren<QLabel*>())
    {
        if (label != summary_text)
            REQUIRE(!label->text().contains(QStringLiteral("example.org")));
        if (label->textFormat() != Qt::PlainText &&
            label->text().contains(QStringLiteral("<a href")))
            ++links;
    }
    REQUIRE_EQ(links, 1);
}

TEST_CASE("update dialog: the summary shows at most eight lines and scrolls")
{
    rig r;
    feed_options options;
    options.summary.clear();
    for (int line = 1; line <= 30; ++line)
        options.summary += "Change number " + std::to_string(line) + "\\n";
    offer(r, options);
    r.open();
    const UpdateDialog& d = *r.dialog;
    const auto* summary = part<QScrollArea>(d, "updateSummary");
    const auto* summary_text = part<QLabel>(d, "updateSummaryText");
    const QFontMetrics metrics(summary_text->font());
    // The part that shows the text holds eight lines and no part of a ninth.
    REQUIRE_EQ(summary->viewport()->height(), 8 * metrics.lineSpacing());
    REQUIRE(summary->verticalScrollBar()->maximum() > 0);
    REQUIRE(
        summary_text->text().startsWith(QStringLiteral("Change number 1\n")));
    REQUIRE(summary_text->text().contains(QStringLiteral("Change number 30")));

    // A short summary takes the lines it needs and does not scroll.
    rig shorter;
    feed_options two;
    two.summary = "First.\\nSecond.";
    offer(shorter, two);
    shorter.open();
    const auto* small = part<QScrollArea>(*shorter.dialog, "updateSummary");
    const auto* small_text = part<QLabel>(*shorter.dialog, "updateSummaryText");
    REQUIRE_EQ(small->viewport()->height(),
               2 * QFontMetrics(small_text->font()).lineSpacing());
    REQUIRE_EQ(small->verticalScrollBar()->maximum(), 0);
}

TEST_CASE("update dialog: the release notes link opens the address through the "
          "hook and has the spec's accessible name")
{
    rig r;
    offer(r);
    r.open();
    const UpdateDialog& d = *r.dialog;
    auto* link = part<QLabel>(d, "updateNotesLink");
    REQUIRE(shown(d, link));
    REQUIRE(link->text().contains(QStringLiteral("Release notes")));
    REQUIRE_EQ(link->accessibleName(),
               QStringLiteral("Release notes for version 0.3.0, opens in your "
                              "browser"));
    REQUIRE(link->focusPolicy() & Qt::TabFocus);
    Q_EMIT link->linkActivated(QStringLiteral("notes"));
    REQUIRE_EQ(r.seen->urls.size(), std::size_t{1});
    REQUIRE_EQ(r.seen->urls[0],
               QUrl(QStringLiteral("https://example.org/notes/0.3.0")));
    // Following it changes nothing else.
    REQUIRE(r.dialog->isVisible());
    REQUIRE(r.dialog->current_page() == page::offer);
}

TEST_CASE("update dialog: there is no release notes link when the feed gave no "
          "notes address")
{
    rig r;
    offer(r);
    // The signed feed always carries an address, so the offer is changed in
    // place here: the service tells the dialog that it changed.
    const_cast<studio_offer&>(*r.h.state().offer).notes_url.clear();
    Q_EMIT r.h.service->changed();
    r.open();
    const UpdateDialog& d = *r.dialog;
    REQUIRE(!shown(d, part<QLabel>(d, "updateNotesLink")));
    REQUIRE(shown(d, part<QLabel>(d, "updateFileLine")));
    REQUIRE(!shown(d, part<QLabel>(d, "updateNotesLink")));
}

TEST_CASE("update dialog: the note about running ports is for the installs "
          "that stop Studio, and has the right plural")
{
    struct row
    {
        upd::file_kind kind = upd::file_kind::windows_installer;
        int ports = 0;
        const char* note = ""; // empty: hidden
    };
    const row rows[] = {
        {upd::file_kind::windows_installer, 0, ""},
        {upd::file_kind::windows_installer, 1,
         "1 port is running. Installing stops it."},
        {upd::file_kind::windows_installer, 3,
         "3 ports are running. Installing stops them."},
        {upd::file_kind::macos_dmg, 0, ""},
        {upd::file_kind::macos_dmg, 1,
         "1 port is running. Installing stops it."},
        {upd::file_kind::macos_dmg, 2,
         "2 ports are running. Installing stops them."},
        {upd::file_kind::linux_deb, 3, ""},
        {upd::file_kind::linux_rpm, 3, ""},
        {upd::file_kind::windows_zip, 3, ""},
        {upd::file_kind::macos_archive, 3, ""},
        {upd::file_kind::linux_archive, 3, ""},
    };
    for (const row& item : rows)
    {
        rig r;
        r.seen->ports = item.ports;
        feed_options options;
        options.kind = item.kind;
        offer(r, options);
        r.open();
        const auto* note =
            part<vss::gui::NoteBanner>(*r.dialog, "updatePortsNote");
        const QString expected = QString::fromLatin1(item.note);
        REQUIRE_EQ(shown(*r.dialog, note), !expected.isEmpty());
        if (!expected.isEmpty())
        {
            REQUIRE_EQ(note->text(), expected);
            REQUIRE(note->get_kind() == vss::gui::NoteBanner::kind::warn);
        }
    }
}

TEST_CASE("update dialog: the ports are counted from the backend unless a hook "
          "says otherwise")
{
    rig r;
    for (const char* name : {"one", "two"})
    {
        const auto made = r.backend.create_virtual_port(
            QString::fromLatin1(name), vss::virtual_port_kind::loopback, true);
        REQUIRE(made.has_value());
    }
    offer(r);
    r.open(false);
    const auto* note = part<vss::gui::NoteBanner>(*r.dialog, "updatePortsNote");
    REQUIRE(shown(*r.dialog, note));
    REQUIRE_EQ(note->text(),
               QStringLiteral("2 ports are running. Installing stops them."));
}

TEST_CASE("update dialog: with no package for this install the file line says "
          "so and the primary button opens the downloads page")
{
    rig r;
    // The feed has a Windows installer only.
    release_spec spec = make_release("0.3.0");
    r.h.probe->answer = upd::file_kind::linux_rpm;
    r.h.create();
    r.h.check_with(feed_body(5, {spec}));
    REQUIRE_EQ(r.h.state().state, check_state::available);
    r.open();
    const UpdateDialog& d = *r.dialog;

    REQUIRE_EQ(text_of(d, "updateFileLine"),
               QStringLiteral("There is no package for this kind of install. "
                              "Download one from virtual-serial-studio.com."));
    REQUIRE_EQ(footer_texts(d),
               QStringList({QStringLiteral("Skip this version"),
                            QStringLiteral("Not now"),
                            QStringLiteral("Open the downloads page")}));
    REQUIRE_EQ(default_text(d), QStringLiteral("Open the downloads page"));
    REQUIRE(!shown(d, part<QWidget>(d, "updatePortsNote")));

    click(d, "updateDownloadsPage");
    REQUIRE_EQ(r.seen->urls.size(), std::size_t{1});
    REQUIRE_EQ(r.seen->urls[0], QUrl(QString::fromLatin1(kDownloadsPage)));
    REQUIRE(!r.dialog->isVisible());
    REQUIRE_EQ(r.h.transport.download_requests, 0);
}

TEST_CASE("update dialog: Skip this version stores it, asks for the toast and "
          "closes; Not now closes and changes nothing")
{
    {
        rig r;
        offer(r);
        r.open();
        click(*r.dialog, "updateSkip");
        REQUIRE_EQ(r.h.service->skipped_version(), QStringLiteral("0.3.0"));
        REQUIRE_EQ(r.toasts, QStringList({QStringLiteral(
                                 "Studio will not mention 0.3.0 again.")}));
        REQUIRE(!r.dialog->isVisible());
    }
    {
        rig r;
        offer(r);
        r.open();
        click(*r.dialog, "updateNotNow");
        REQUIRE(!r.dialog->isVisible());
        REQUIRE(r.toasts.isEmpty());
        REQUIRE_EQ(r.h.service->skipped_version(), QString());
        REQUIRE_EQ(r.h.state().download, download_state::none);
        REQUIRE_EQ(r.h.transport.download_requests, 0);
        REQUIRE_EQ(r.h.state().state, check_state::available);
    }
    {
        // Escape is "Not now" before a download.
        rig r;
        offer(r);
        r.open();
        press_escape(*r.dialog);
        REQUIRE(!r.dialog->isVisible());
        REQUIRE_EQ(r.h.service->skipped_version(), QString());
    }
}

// -- the download
// --------------------------------------------------------------

TEST_CASE("update dialog: the downloading page follows the pieces as they "
          "arrive")
{
    rig r;
    const QByteArray bytes = payload_bytes(100000);
    offer(r);
    r.open();
    const UpdateDialog& d = *r.dialog;

    click(d, "updateDownload");
    REQUIRE_EQ(r.h.state().download, download_state::downloading);
    REQUIRE(r.dialog->current_page() == page::downloading);
    REQUIRE_EQ(
        text_of(d, "updateProgressText"),
        QStringLiteral("Downloading studio-0.3.0-windows-x86_64.exe..."));
    const auto* bar = part<QProgressBar>(d, "updateProgress");
    REQUIRE(shown(d, bar));
    REQUIRE_EQ(bar->accessibleName(), QStringLiteral("Download progress"));
    REQUIRE_EQ(bar->minimum(), 0);
    REQUIRE_EQ(bar->maximum(), 100);
    REQUIRE_EQ(bar->value(), 0);
    REQUIRE_EQ(text_of(d, "updateAmount"),
               QStringLiteral("%1 of %2")
                   .arg(update_size_text(0), update_size_text(100000)));
    REQUIRE_EQ(footer_texts(d), QStringList({QStringLiteral("Cancel")}));
    REQUIRE(!shown(d, part<QWidget>(d, "updateSentence")));
    REQUIRE(!shown(d, part<QWidget>(d, "updateSummary")));

    std::uint64_t seen = 0;
    for (const int piece : {10000, 15000, 25000, 30000})
    {
        REQUIRE(r.h.transport.send_chunk(
            QByteArrayView(bytes).mid(static_cast<qsizetype>(seen), piece)));
        seen += static_cast<std::uint64_t>(piece);
        REQUIRE_EQ(bar->value(), static_cast<int>(seen / 1000));
        REQUIRE_EQ(text_of(d, "updateAmount"),
                   QStringLiteral("%1 of %2")
                       .arg(update_size_text(seen), update_size_text(100000)));
    }
    REQUIRE(r.dialog->current_page() == page::downloading);
}

TEST_CASE("update dialog: Cancel deletes what arrived and returns to the offer "
          "page, showing nothing else")
{
    rig r;
    offer(r);
    r.open();
    const UpdateDialog& d = *r.dialog;
    click(d, "updateDownload");
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(20000))));
    REQUIRE_EQ(r.h.cache_entries().size(), 1);

    click(d, "updateCancel");
    r.h.transport.deliver_cancelled();
    settle();
    REQUIRE_EQ(r.h.transport.cancels, 1);
    REQUIRE_EQ(r.h.state().download, download_state::none);
    REQUIRE(r.h.cache_entries().isEmpty());
    REQUIRE(r.dialog->isVisible());
    REQUIRE(r.dialog->current_page() == page::offer);
    REQUIRE_EQ(footer_texts(d),
               QStringList({QStringLiteral("Skip this version"),
                            QStringLiteral("Not now"),
                            QStringLiteral("Download and install")}));
    REQUIRE(!shown(d, part<QWidget>(d, "updateFailureTitle")));
    REQUIRE(!shown(d, part<QWidget>(d, "updateProgress")));
    REQUIRE(r.toasts.isEmpty());

    // And it can start again.
    click(d, "updateDownload");
    REQUIRE_EQ(r.h.transport.download_requests, 2);
}

TEST_CASE("update dialog: Escape and the title bar's close button cancel a "
          "download and do not close the dialog")
{
    for (const bool use_escape : {true, false})
    {
        rig r;
        offer(r);
        r.open();
        click(*r.dialog, "updateDownload");
        REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(5000))));
        if (use_escape)
            press_escape(*r.dialog);
        else
        {
            r.dialog->close();
            settle();
        }
        r.h.transport.deliver_cancelled();
        settle();
        REQUIRE(r.dialog->isVisible());
        REQUIRE(r.dialog->current_page() == page::offer);
        REQUIRE_EQ(r.h.state().download, download_state::none);
        REQUIRE_EQ(r.h.transport.cancels, 1);
        REQUIRE(r.h.cache_entries().isEmpty());

        // Now it is "Not now".
        press_escape(*r.dialog);
        REQUIRE(!r.dialog->isVisible());
    }
}

TEST_CASE("update dialog: while the download is checked the bar is full and no "
          "button is enabled")
{
    rig r;
    offer(r);
    r.open();
    const UpdateDialog& d = *r.dialog;
    click(d, "updateDownload");
    deliver_all(r, payload_bytes(100000));
    // The bytes are here and are being hashed.
    REQUIRE_EQ(r.h.state().download, download_state::verifying);
    REQUIRE(r.dialog->current_page() == page::checking);
    REQUIRE_EQ(text_of(d, "updateProgressText"),
               QStringLiteral("Checking the download..."));
    REQUIRE_EQ(part<QProgressBar>(d, "updateProgress")->value(), 100);
    int visible = 0;
    for (auto* push : d.findChildren<QPushButton*>())
    {
        if (!shown(d, push))
            continue;
        ++visible;
        REQUIRE(!push->isEnabled());
    }
    REQUIRE(visible >= 1);

    r.h.pump_download();
    REQUIRE_EQ(r.h.state().download, download_state::ready);
    settle();
    REQUIRE(r.dialog->current_page() == page::ready);
}

TEST_CASE("update dialog: Escape while the file is checked cancels too")
{
    rig r;
    offer(r);
    r.open();
    click(*r.dialog, "updateDownload");
    deliver_all(r, payload_bytes(100000));
    REQUIRE(r.dialog->current_page() == page::checking);
    press_escape(*r.dialog);
    REQUIRE(r.dialog->isVisible());
    REQUIRE(r.dialog->current_page() == page::offer);
    REQUIRE_EQ(r.h.state().download, download_state::none);
    r.h.pump_download();
    REQUIRE_EQ(r.h.state().download, download_state::none);
}

// -- the checked file, by kind of install
// ---------------------------------------

TEST_CASE("update dialog: the ready page for each of the seven kinds of "
          "install")
{
    struct row
    {
        upd::file_kind kind = upd::file_kind::windows_installer;
        QString sentence; // %1 stands for the folder of the file
        QStringList buttons;
        QString primary;
    };
    const QString installed = QStringLiteral(
        "The update is ready. Install it with this command, then start Studio "
        "again.");
    const QString archive = QStringLiteral(
        "The download is in %1. This copy of Studio was unpacked from an "
        "archive, so unpack the new one in its place.");
    const row rows[] = {
        {upd::file_kind::windows_installer,
         QStringLiteral("The update is ready. Studio will close and the "
                        "installer will start. Windows asks for an "
                        "administrator."),
         {QStringLiteral("Not now"), QStringLiteral("Install now")},
         QStringLiteral("Install now")},
        {upd::file_kind::macos_dmg,
         QStringLiteral("The update is ready. Quit Studio, then drag Virtual "
                        "Serial Studio onto Applications in the window that "
                        "opens."),
         {QStringLiteral("Not now"), QStringLiteral("Open the disk image")},
         QStringLiteral("Open the disk image")},
        {upd::file_kind::linux_deb,
         installed,
         {QStringLiteral("Show in folder"), QStringLiteral("Close")},
         QStringLiteral("Close")},
        {upd::file_kind::linux_rpm,
         installed,
         {QStringLiteral("Show in folder"), QStringLiteral("Close")},
         QStringLiteral("Close")},
        {upd::file_kind::windows_zip,
         archive,
         {QStringLiteral("Show in folder"), QStringLiteral("Close")},
         QStringLiteral("Show in folder")},
        {upd::file_kind::macos_archive,
         archive,
         {QStringLiteral("Show in folder"), QStringLiteral("Close")},
         QStringLiteral("Show in folder")},
        {upd::file_kind::linux_archive,
         archive,
         {QStringLiteral("Show in folder"), QStringLiteral("Close")},
         QStringLiteral("Show in folder")},
    };
    for (const row& item : rows)
    {
        rig r;
        feed_options options;
        options.kind = item.kind;
        offer(r, options);
        r.open();
        const UpdateDialog& d = *r.dialog;
        click(d, "updateDownload");
        deliver_all(r, options.bytes);
        r.h.pump_download();
        settle();
        REQUIRE_EQ(r.h.state().download, download_state::ready);
        REQUIRE(r.dialog->current_page() == page::ready);

        const QString path = r.h.state().ready_path;
        REQUIRE_EQ(text_of(d, "updateReadyText"),
                   item.sentence.contains(QStringLiteral("%1"))
                       ? item.sentence.arg(folder_of(path))
                       : item.sentence);
        REQUIRE_EQ(footer_texts(d), item.buttons);
        REQUIRE_EQ(default_text(d), item.primary);
        REQUIRE_EQ(shown(d, part<QWidget>(d, "updateCommand")),
                   item.kind == upd::file_kind::linux_deb ||
                       item.kind == upd::file_kind::linux_rpm);
        REQUIRE(!shown(d, part<QWidget>(d, "updateSentence")));
        REQUIRE(!shown(d, part<QWidget>(d, "updateProgress")));
        REQUIRE(!shown(d, part<QWidget>(d, "updateFailureTitle")));
        REQUIRE(!shown(d, part<QWidget>(d, "updateDoneNote")));
        REQUIRE(!shown(d, part<QWidget>(d, "updateFailedNote")));
    }
}

TEST_CASE(
    "update dialog: Not now closes and the checked file is kept, so a new "
    "dialog shows the ready page at once")
{
    rig r;
    offer(r);
    r.open();
    make_ready(r);
    REQUIRE(r.dialog->current_page() == page::ready);
    click(*r.dialog, "updateNotNow");
    REQUIRE(!r.dialog->isVisible());
    REQUIRE_EQ(r.h.state().download, download_state::ready);
    REQUIRE(r.h.service->ready_file() != nullptr);
    REQUIRE(r.h.installs.empty());

    r.open();
    REQUIRE(r.dialog->current_page() == page::ready);
    REQUIRE_EQ(footer_texts(*r.dialog),
               QStringList(
                   {QStringLiteral("Not now"), QStringLiteral("Install now")}));
    REQUIRE_EQ(r.h.transport.download_requests, 1);
}

TEST_CASE("update dialog: Install now with started closes the dialog and the "
          "service asks the window to quit")
{
    rig r;
    offer(r);
    r.open();
    make_ready(r);
    r.h.install_answer = install_start::started;
    click(*r.dialog, "updateInstallNow");
    REQUIRE_EQ(r.h.installs.size(), std::size_t{1});
    REQUIRE(r.h.installs[0].kind == upd::file_kind::windows_installer);
    REQUIRE_EQ(r.h.quits, 1);
    REQUIRE(!r.dialog->isVisible());
}

TEST_CASE("update dialog: Install now declined stays on the page and shows "
          "nothing")
{
    rig r;
    offer(r);
    r.open();
    make_ready(r);
    r.h.install_answer = install_start::declined;
    click(*r.dialog, "updateInstallNow");
    REQUIRE_EQ(r.h.installs.size(), std::size_t{1});
    REQUIRE_EQ(r.h.quits, 0);
    REQUIRE(r.dialog->isVisible());
    REQUIRE(r.dialog->current_page() == page::ready);
    REQUIRE(!shown(*r.dialog, part<QWidget>(*r.dialog, "updateFailureTitle")));
    REQUIRE_EQ(footer_texts(*r.dialog),
               QStringList(
                   {QStringLiteral("Not now"), QStringLiteral("Install now")}));
    // The person may say yes the next time.
    r.h.install_answer = install_start::started;
    click(*r.dialog, "updateInstallNow");
    REQUIRE_EQ(r.h.quits, 1);
    REQUIRE(!r.dialog->isVisible());
}

TEST_CASE("update dialog: when the installer cannot be started the failure "
          "page names the file and the folder")
{
    rig r;
    offer(r);
    r.open();
    make_ready(r);
    r.h.install_answer = install_start::failed;
    click(*r.dialog, "updateInstallNow");
    const UpdateDialog& d = *r.dialog;
    REQUIRE(r.dialog->isVisible());
    REQUIRE(r.dialog->current_page() == page::failure);
    REQUIRE_EQ(r.h.quits, 0);
    REQUIRE_EQ(text_of(d, "updateFailureTitle"),
               QStringLiteral("Could not start the installer"));
    REQUIRE_EQ(text_of(d, "updateFailureBody"),
               QStringLiteral("Studio could not start "
                              "studio-0.3.0-windows-x86_64.exe. It is in %1.")
                   .arg(folder_of(r.h.state().ready_path)));
    REQUIRE_EQ(footer_texts(d), QStringList({QStringLiteral("Show in folder"),
                                             QStringLiteral("Close")}));
    // "Show in folder" takes the place of "Try again".
    REQUIRE(!shown(d, button(d, "updateTryAgain")));

    click(d, "updateShowFolder");
    REQUIRE_EQ(r.seen->folders.size(), std::size_t{1});
    REQUIRE_EQ(r.seen->folders[0], r.h.state().ready_path);

    // Close leaves the checked file where it is.
    click(d, "updateClose");
    REQUIRE(!r.dialog->isVisible());
    REQUIRE_EQ(r.h.state().download, download_state::ready);

    // Anything else the system says is a failure too.
    rig other;
    offer(other);
    other.open();
    make_ready(other);
    other.h.install_answer = install_start::not_supported;
    click(*other.dialog, "updateInstallNow");
    REQUIRE(other.dialog->current_page() == page::failure);
}

TEST_CASE("update dialog: Open the disk image closes on started and shows the "
          "installer failure otherwise")
{
    {
        rig r;
        feed_options options;
        options.kind = upd::file_kind::macos_dmg;
        offer(r, options);
        r.open();
        make_ready(r);
        click(*r.dialog, "updateOpenImage");
        REQUIRE_EQ(r.h.installs.size(), std::size_t{1});
        REQUIRE(r.h.installs[0].kind == upd::file_kind::macos_dmg);
        // Only a Windows installer asks Studio to close.
        REQUIRE_EQ(r.h.quits, 0);
        REQUIRE(!r.dialog->isVisible());
    }
    {
        rig r;
        feed_options options;
        options.kind = upd::file_kind::macos_dmg;
        offer(r, options);
        r.open();
        make_ready(r);
        r.h.install_answer = install_start::failed;
        click(*r.dialog, "updateOpenImage");
        REQUIRE(r.dialog->current_page() == page::failure);
        REQUIRE_EQ(text_of(*r.dialog, "updateFailureTitle"),
                   QStringLiteral("Could not start the installer"));
        REQUIRE_EQ(text_of(*r.dialog, "updateFailureBody"),
                   QStringLiteral("Studio could not start "
                                  "studio-0.3.0-macos-x86_64.dmg. It is in %1.")
                       .arg(folder_of(r.h.state().ready_path)));
    }
}

TEST_CASE("update dialog: deb and rpm show the command, and Install only when "
          "the system can do it")
{
    for (const upd::file_kind kind :
         {upd::file_kind::linux_deb, upd::file_kind::linux_rpm})
    {
        for (const bool available : {false, true})
        {
            rig r;
            r.seen->package_available = available;
            feed_options options;
            options.kind = kind;
            offer(r, options);
            r.open();
            make_ready(r);
            const UpdateDialog& d = *r.dialog;
            const QString expected = QStringLiteral("%1 '%2'").arg(
                kind == upd::file_kind::linux_deb
                    ? QStringLiteral("sudo apt install")
                    : QStringLiteral("sudo dnf install"),
                r.h.state().ready_path);
            const auto* command = part<vss::gui::CodeBlock>(d, "updateCommand");
            REQUIRE(shown(d, command));
            REQUIRE_EQ(command->text(), expected);
            REQUIRE_EQ(command->text(),
                       package_install_command(kind, r.h.state().ready_path));
            REQUIRE_EQ(command->accessibleName(),
                       QStringLiteral("Install command"));
            if (available)
            {
                REQUIRE_EQ(footer_texts(d),
                           QStringList({QStringLiteral("Show in folder"),
                                        QStringLiteral("Install"),
                                        QStringLiteral("Close")}));
                REQUIRE_EQ(default_text(d), QStringLiteral("Install"));
            }
            else
            {
                REQUIRE_EQ(footer_texts(d),
                           QStringList({QStringLiteral("Show in folder"),
                                        QStringLiteral("Close")}));
            }
            REQUIRE(!shown(d, part<QWidget>(d, "updateDoneNote")));
            REQUIRE(!shown(d, part<QWidget>(d, "updateFailedNote")));
        }
    }
}

TEST_CASE("update dialog: Show in folder passes the path of the file to the "
          "hook and Close closes")
{
    rig r;
    feed_options options;
    options.kind = upd::file_kind::linux_archive;
    offer(r, options);
    r.open();
    make_ready(r);
    click(*r.dialog, "updateShowFolder");
    REQUIRE_EQ(r.seen->folders.size(), std::size_t{1});
    REQUIRE_EQ(r.seen->folders[0], r.h.state().ready_path);
    REQUIRE(r.dialog->isVisible());
    click(*r.dialog, "updateClose");
    REQUIRE(!r.dialog->isVisible());
    REQUIRE_EQ(r.h.state().download, download_state::ready);
}

TEST_CASE("update dialog: Install runs the package install with the kind, the "
          "path and the offer's SHA-512, and shows each of the four results")
{
    struct row
    {
        const char* note = ""; // empty: no note
        package_install_result result = package_install_result::done;
        bool kept_install_button = false;
    };
    const row rows[] = {
        {"Restart Studio to use the new version.", package_install_result::done,
         false},
        {"", package_install_result::declined, true},
        {"The package could not be installed. Use the command above.",
         package_install_result::failed, true},
        {"The package could not be installed. Use the command above.",
         package_install_result::not_available, true},
    };
    for (const row& item : rows)
    {
        rig r;
        r.seen->package_available = true;
        feed_options options;
        options.kind = upd::file_kind::linux_deb;
        offer(r, options);
        r.open();
        make_ready(r);
        const UpdateDialog& d = *r.dialog;

        click(d, "updateInstallPackage");
        REQUIRE_EQ(r.seen->packages.size(), std::size_t{1});
        REQUIRE(r.seen->packages[0].kind == upd::file_kind::linux_deb);
        REQUIRE_EQ(r.seen->packages[0].path, r.h.state().ready_path);
        const std::optional<offered_file>& offered = require_offer(r.h).file;
        REQUIRE(offered.has_value());
        REQUIRE(r.seen->packages[0].sha512 == offered->sha512);
        // The buttons are disabled while it runs.
        for (const auto* push : d.findChildren<QPushButton*>())
        {
            if (shown(d, push))
                REQUIRE(!push->isEnabled());
        }
        REQUIRE(r.dialog->isVisible());

        r.seen->finish(item.result);
        settle();
        const QString note = QString::fromLatin1(item.note);
        const bool done = item.result == package_install_result::done;
        const auto* banner = part<vss::gui::NoteBanner>(
            d, done ? "updateDoneNote" : "updateFailedNote");
        const auto* other = part<vss::gui::NoteBanner>(
            d, done ? "updateFailedNote" : "updateDoneNote");
        REQUIRE_EQ(shown(d, banner), !note.isEmpty());
        REQUIRE(!shown(d, other));
        if (!note.isEmpty())
        {
            REQUIRE_EQ(banner->text(), note);
            REQUIRE(banner->get_kind() ==
                    (done ? vss::gui::NoteBanner::kind::ok
                          : vss::gui::NoteBanner::kind::danger));
        }
        // Every visible button works again.
        for (const auto* push : d.findChildren<QPushButton*>())
        {
            if (shown(d, push))
                REQUIRE(push->isEnabled());
        }
        REQUIRE_EQ(shown(d, button(d, "updateInstallPackage")),
                   item.kept_install_button);
        REQUIRE(r.dialog->current_page() == page::ready);
        REQUIRE(r.dialog->isVisible());
        // The command is still there to use.
        REQUIRE(shown(d, part<QWidget>(d, "updateCommand")));
    }
}

// -- failures
// ------------------------------------------------------------------

TEST_CASE("update dialog: a download that cannot reach the server shows the "
          "failure, and Try again starts a second download")
{
    rig r;
    offer(r);
    r.open();
    const UpdateDialog& d = *r.dialog;
    click(d, "updateDownload");
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(3000))));
    r.h.transport.finish_download(transport_error::network);
    settle();
    REQUIRE_EQ(r.h.state().download_error, download_failure::unreachable);
    REQUIRE(r.dialog->current_page() == page::failure);
    REQUIRE_EQ(text_of(d, "updateFailureTitle"),
               QStringLiteral("Could not download the update"));
    REQUIRE_EQ(text_of(d, "updateFailureBody"),
               QStringLiteral("Studio could not reach the update server. "
                              "Check the connection and try again."));
    REQUIRE_EQ(footer_texts(d), QStringList({QStringLiteral("Try again"),
                                             QStringLiteral("Close")}));
    REQUIRE_EQ(default_text(d), QStringLiteral("Try again"));
    REQUIRE(!shown(d, part<QWidget>(d, "updateSentence")));

    click(d, "updateTryAgain");
    REQUIRE_EQ(r.h.transport.download_requests, 2);
    REQUIRE_EQ(r.h.state().download, download_state::downloading);
    REQUIRE(r.dialog->current_page() == page::downloading);
    REQUIRE_EQ(part<QProgressBar>(d, "updateProgress")->value(), 0);
    // The second one can finish.
    deliver_all(r, payload_bytes(100000));
    r.h.pump_download();
    settle();
    REQUIRE(r.dialog->current_page() == page::ready);
}

TEST_CASE("update dialog: a download that is not the published file shows the "
          "mismatch failure")
{
    rig r;
    feed_options options;
    options.wrong_digest = true;
    options.bytes = payload_bytes(5000);
    offer(r, options);
    r.open();
    const UpdateDialog& d = *r.dialog;
    click(d, "updateDownload");
    deliver_all(r, options.bytes);
    r.h.pump_download();
    settle();
    REQUIRE_EQ(r.h.state().download_error, download_failure::mismatch);
    REQUIRE(r.dialog->current_page() == page::failure);
    REQUIRE_EQ(text_of(d, "updateFailureTitle"),
               QStringLiteral("Could not download the update"));
    REQUIRE_EQ(text_of(d, "updateFailureBody"),
               QStringLiteral("The download was not the file that was "
                              "published, so it was deleted."));
    REQUIRE_EQ(footer_texts(d), QStringList({QStringLiteral("Try again"),
                                             QStringLiteral("Close")}));
}

TEST_CASE("update dialog: a disk that cannot be written shows the save "
          "failure")
{
    rig r;
    r.h.use_failing_file = true;
    r.h.failing_limit = 5000;
    feed_options options;
    options.bytes = payload_bytes(20000);
    offer(r, options);
    r.open();
    const UpdateDialog& d = *r.dialog;
    click(d, "updateDownload");
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(4000))));
    REQUIRE(!r.h.transport.send_chunk(QByteArrayView(payload_bytes(4000))));
    r.h.transport.finish_download(transport_error::cancelled);
    settle();
    REQUIRE_EQ(r.h.state().download_error, download_failure::save_failed);
    REQUIRE(r.dialog->current_page() == page::failure);
    REQUIRE_EQ(text_of(d, "updateFailureTitle"),
               QStringLiteral("Could not download the update"));
    REQUIRE_EQ(text_of(d, "updateFailureBody"),
               QStringLiteral("Studio could not save the download."));
    REQUIRE_EQ(footer_texts(d), QStringList({QStringLiteral("Try again"),
                                             QStringLiteral("Close")}));
}

TEST_CASE("update dialog: Close on a download failure clears it and closes; "
          "opening the dialog again shows the offer")
{
    rig r;
    offer(r);
    r.open();
    click(*r.dialog, "updateDownload");
    r.h.transport.finish_download(transport_error::timeout);
    settle();
    REQUIRE(r.dialog->current_page() == page::failure);
    click(*r.dialog, "updateClose");
    REQUIRE(!r.dialog->isVisible());
    REQUIRE_EQ(r.h.state().download, download_state::none);
    REQUIRE_EQ(r.h.state().download_error, download_failure::none);

    r.open();
    REQUIRE(r.dialog->current_page() == page::offer);
}

TEST_CASE("update dialog: opened on a failed download it shows the failure at "
          "once")
{
    rig r;
    offer(r);
    r.h.service->start_download();
    r.h.transport.finish_download(transport_error::network);
    REQUIRE_EQ(r.h.state().download, download_state::failed);
    r.open();
    REQUIRE(r.dialog->current_page() == page::failure);
    REQUIRE_EQ(
        footer_texts(*r.dialog),
        QStringList({QStringLiteral("Try again"), QStringLiteral("Close")}));
}

TEST_CASE("update dialog: opened during a download it shows the progress at "
          "once")
{
    rig r;
    offer(r);
    r.h.service->start_download();
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(50000))));
    r.open();
    REQUIRE(r.dialog->current_page() == page::downloading);
    REQUIRE_EQ(part<QProgressBar>(*r.dialog, "updateProgress")->value(), 50);
}

// -- the offer goes away
// ---------------------------------------------------------

TEST_CASE("update dialog: it closes itself when the offer goes away")
{
    {
        rig r;
        offer(r);
        r.open();
        REQUIRE(r.dialog->isVisible());
        // A later check finds nothing newer.
        r.h.service->check_now();
        REQUIRE(r.h.transport.feed_pending());
        r.h.transport.reply_feed(transport_error::none,
                                 feed_body(6, {make_release("0.2.0")}));
        REQUIRE_EQ(r.h.state().state, check_state::current);
        settle();
        REQUIRE(!r.dialog->isVisible());
    }
    {
        // Opened with no offer at all, it closes from the event loop and not
        // from the constructor.
        rig r;
        r.h.create();
        r.h.check_with(feed_body(5, {make_release("0.2.0")}));
        REQUIRE(!r.h.state().offer.has_value());
        r.open();
        REQUIRE(!r.dialog->isVisible());
    }
}

// -- accessibility
// -----------------------------------------------------------------

namespace
{

struct announcement_log
{
    QStringList texts;
    int other = 0;
};

announcement_log* g_announcements = nullptr;

void count_announcement(QAccessibleEvent* event)
{
    if (g_announcements == nullptr)
        return;
    if (event->type() == QAccessible::Announcement)
        g_announcements->texts
            << static_cast<QAccessibleAnnouncementEvent*>(event)->message();
    else
        ++g_announcements->other;
}

/// Counts the announcements while it lives, and puts the old handler back.
struct announcement_watch
{
    announcement_watch()
        : previous(QAccessible::installUpdateHandler(&count_announcement))
    {
        g_announcements = &log;
    }
    ~announcement_watch()
    {
        QAccessible::installUpdateHandler(previous);
        g_announcements = nullptr;
    }
    announcement_watch(const announcement_watch&) = delete;
    announcement_watch& operator=(const announcement_watch&) = delete;

    announcement_log log;
    QAccessible::UpdateHandler previous;
};

/// The widgets Tab reaches in `dialog`, in order.
QList<QWidget*> tab_stops(UpdateDialog& dialog)
{
    QList<QWidget*> stops;
    QWidget* at = dialog.nextInFocusChain();
    while (at != nullptr && at != &dialog)
    {
        if (dialog.isAncestorOf(at) && at->isVisibleTo(&dialog) &&
            at->isEnabled() && (at->focusPolicy() & Qt::TabFocus))
            stops.push_back(at);
        at = at->nextInFocusChain();
    }
    return stops;
}

/// Every visible button of `dialog` has an accessible name and a Tab stop.
void require_buttons_reachable(UpdateDialog& dialog)
{
    const QList<QWidget*> stops = tab_stops(dialog);
    for (auto* push : dialog.findChildren<QPushButton*>())
    {
        if (!push->isVisibleTo(&dialog))
            continue;
        REQUIRE(!push->accessibleName().isEmpty());
        REQUIRE_EQ(push->accessibleName(), push->text());
        REQUIRE(push->isEnabled());
        REQUIRE(stops.contains(push));
    }
}

} // namespace

TEST_CASE("update dialog: the progress is announced at most once every ten "
          "percent")
{
    rig r;
    offer(r);
    r.open();
    click(*r.dialog, "updateDownload");
    const QByteArray bytes = payload_bytes(100000);
    const announcement_watch watch;
    // One percent at a time: 100 pieces.
    for (qsizetype at = 0; at < bytes.size(); at += 1000)
    {
        REQUIRE(r.h.transport.send_chunk(QByteArrayView(bytes).mid(at, 1000)));
        REQUIRE(r.dialog->current_page() == page::downloading);
    }
    r.h.transport.finish_download(transport_error::none);
    settle();
    r.h.pump_download();
    settle();

    QStringList expected;
    for (int percent = 10; percent <= 100; percent += 10)
        expected << QStringLiteral("%1 percent").arg(percent);
    REQUIRE_EQ(watch.log.texts, expected);
}

TEST_CASE("update dialog: a jump in the progress is announced once")
{
    rig r;
    offer(r);
    r.open();
    click(*r.dialog, "updateDownload");
    const announcement_watch watch;
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(53000))));
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(2000))));
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(4000))));
    REQUIRE_EQ(watch.log.texts, QStringList({QStringLiteral("50 percent")}));
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(1000))));
    REQUIRE_EQ(watch.log.texts, QStringList({QStringLiteral("50 percent"),
                                             QStringLiteral("60 percent")}));
}

TEST_CASE("update dialog: a download that starts again is announced from the "
          "start")
{
    rig r;
    offer(r);
    r.open();
    click(*r.dialog, "updateDownload");
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(30000))));
    click(*r.dialog, "updateCancel");
    r.h.transport.deliver_cancelled();
    settle();
    const announcement_watch watch;
    click(*r.dialog, "updateDownload");
    REQUIRE(r.h.transport.send_chunk(QByteArrayView(payload_bytes(20000))));
    REQUIRE_EQ(watch.log.texts, QStringList({QStringLiteral("20 percent")}));
}

TEST_CASE("update dialog: every button and the progress bar have accessible "
          "names and every control is reachable with the keyboard")
{
    // The offer page with its summary and its link.
    {
        rig r;
        offer(r);
        r.open();
        UpdateDialog& d = *r.dialog;
        require_buttons_reachable(d);
        const QList<QWidget*> stops = tab_stops(d);
        QWidget* summary = part<QScrollArea>(d, "updateSummary");
        QWidget* link = part<QLabel>(d, "updateNotesLink");
        QWidget* skip = button(d, "updateSkip");
        QWidget* download = button(d, "updateDownload");
        REQUIRE(stops.contains(summary));
        REQUIRE(stops.contains(link));
        REQUIRE_EQ(summary->accessibleName(), QStringLiteral("What is new"));
        // The body comes first and the buttons follow, left to right.
        REQUIRE(stops.indexOf(summary) < stops.indexOf(link));
        REQUIRE(stops.indexOf(link) < stops.indexOf(skip));
        REQUIRE(stops.indexOf(skip) < stops.indexOf(button(d, "updateNotNow")));
        REQUIRE(stops.indexOf(button(d, "updateNotNow")) <
                stops.indexOf(download));
        // The default button has the focus when the dialog opens.
        REQUIRE(download->hasFocus() || d.focusWidget() == download);
    }
    // The downloading page.
    {
        rig r;
        offer(r);
        r.open();
        click(*r.dialog, "updateDownload");
        UpdateDialog& d = *r.dialog;
        require_buttons_reachable(d);
        REQUIRE_EQ(part<QProgressBar>(d, "updateProgress")->accessibleName(),
                   QStringLiteral("Download progress"));
        REQUIRE_EQ(d.focusWidget(), button(d, "updateCancel"));
    }
    // The ready page with a command and its copy button.
    {
        rig r;
        r.seen->package_available = true;
        feed_options options;
        options.kind = upd::file_kind::linux_deb;
        offer(r, options);
        r.open();
        make_ready(r);
        UpdateDialog& d = *r.dialog;
        require_buttons_reachable(d);
        const QList<QWidget*> stops = tab_stops(d);
        bool copy_reachable = false;
        for (auto* widget : stops)
        {
            if (widget->accessibleName().startsWith(QStringLiteral("Copy")))
                copy_reachable = true;
        }
        REQUIRE(copy_reachable);
        REQUIRE_EQ(d.focusWidget(), button(d, "updateInstallPackage"));
    }
    // A failure.
    {
        rig r;
        offer(r);
        r.open();
        click(*r.dialog, "updateDownload");
        r.h.transport.finish_download(transport_error::network);
        settle();
        require_buttons_reachable(*r.dialog);
        REQUIRE_EQ(r.dialog->focusWidget(),
                   button(*r.dialog, "updateTryAgain"));
    }
}

// -- text at 200 percent
// -------------------------------------------------------------

namespace
{

/// The dialog fits its screen, no button is cut off, the summary keeps to
/// eight lines, and what does not fit scrolls.
void require_usable(UpdateDialog& dialog)
{
    REQUIRE(vss_test::footer_fully_visible(dialog));
    const auto* summary =
        dialog.findChild<QScrollArea*>(QStringLiteral("updateSummary"));
    const auto* summary_text =
        dialog.findChild<QLabel*>(QStringLiteral("updateSummaryText"));
    if (summary != nullptr && summary->isVisibleTo(&dialog))
    {
        const QFontMetrics metrics(summary_text->font());
        REQUIRE(summary->viewport()->height() <= 8 * metrics.lineSpacing());
    }
}

QString shots_directory()
{
    const QString directory =
        QDir(QStringLiteral(VSS_SOURCE_DIR))
            .filePath(QStringLiteral("build/update-shots"));
    REQUIRE(QDir().mkpath(directory));
    return directory;
}

void save_shot(UpdateDialog& dialog, const QString& name)
{
    settle();
    REQUIRE(dialog.grab().save(QDir(shots_directory()).filePath(name)));
}

/// Puts the theme back when a test ends, however it ends.
struct light_theme_guard
{
    light_theme_guard() = default;
    ~light_theme_guard()
    {
        vss::gui::Theme::instance().set_mode(vss::gui::theme_mode::light);
        QApplication::processEvents();
    }
    light_theme_guard(const light_theme_guard&) = delete;
    light_theme_guard& operator=(const light_theme_guard&) = delete;
};

} // namespace

TEST_CASE("update dialog: every page works at 200 percent text and at the "
          "smallest window")
{
    const light_theme_guard guard;
    for (const double factor : {1.0, 2.0})
    {
        const QString tag =
            factor == 1.0 ? QStringLiteral("100") : QStringLiteral("200");
        // The offer, with a long summary that scrolls and the ports note.
        {
            rig r;
            r.seen->ports = 3;
            feed_options options;
            options.summary.clear();
            for (int line = 1; line <= 30; ++line)
                options.summary +=
                    "Change number " + std::to_string(line) + "\\n";
            offer(r, options);
            r.open();
            if (factor != 1.0)
                vss_test::scale_text(r.dialog.get(), factor);
            settle();
            require_usable(*r.dialog);
            save_shot(*r.dialog,
                      QStringLiteral("update-offer-%1.png").arg(tag));
            vss_test::shrink_to_minimum(*r.dialog);
            require_usable(*r.dialog);
        }
        // The download.
        {
            rig r;
            offer(r);
            r.open();
            click(*r.dialog, "updateDownload");
            REQUIRE(
                r.h.transport.send_chunk(QByteArrayView(payload_bytes(42000))));
            if (factor != 1.0)
                vss_test::scale_text(r.dialog.get(), factor);
            settle();
            require_usable(*r.dialog);
            save_shot(*r.dialog,
                      QStringLiteral("update-downloading-%1.png").arg(tag));
            vss_test::shrink_to_minimum(*r.dialog);
            require_usable(*r.dialog);
        }
        // The command for a package, and a result.
        {
            rig r;
            r.seen->package_available = true;
            feed_options options;
            options.kind = upd::file_kind::linux_deb;
            offer(r, options);
            r.open();
            make_ready(r);
            click(*r.dialog, "updateInstallPackage");
            r.seen->finish(package_install_result::failed);
            settle();
            if (factor != 1.0)
                vss_test::scale_text(r.dialog.get(), factor);
            settle();
            require_usable(*r.dialog);
            save_shot(*r.dialog,
                      QStringLiteral("update-package-%1.png").arg(tag));
            vss_test::shrink_to_minimum(*r.dialog);
            require_usable(*r.dialog);
        }
        // The Windows installer, and a failure to start it.
        {
            rig r;
            offer(r);
            r.open();
            make_ready(r);
            if (factor != 1.0)
                vss_test::scale_text(r.dialog.get(), factor);
            settle();
            require_usable(*r.dialog);
            save_shot(*r.dialog,
                      QStringLiteral("update-ready-%1.png").arg(tag));
            r.h.install_answer = install_start::failed;
            click(*r.dialog, "updateInstallNow");
            settle();
            require_usable(*r.dialog);
            save_shot(*r.dialog,
                      QStringLiteral("update-failure-%1.png").arg(tag));
            vss_test::shrink_to_minimum(*r.dialog);
            require_usable(*r.dialog);
        }
    }
}

TEST_CASE("update dialog: review pictures of the offer in both themes")
{
    const light_theme_guard guard;
    // A dialog is made in the theme it is shown in, as the program does.
    for (const bool dark : {false, true})
    {
        vss::gui::Theme::instance().set_mode(
            dark ? vss::gui::theme_mode::dark : vss::gui::theme_mode::light);
        const QString tone =
            dark ? QStringLiteral("dark") : QStringLiteral("light");
        rig r;
        r.seen->ports = 2;
        feed_options options;
        options.summary = "A new canvas and a faster monitor.\\nFixes for the "
                          "driver set-up check.";
        offer(r, options);
        r.open();
        save_shot(*r.dialog, QStringLiteral("update-offer-%1.png").arg(tone));
        make_ready(r);
        r.dialog.reset();
        r.open();
        save_shot(*r.dialog, QStringLiteral("update-ready-%1.png").arg(tone));
    }
}
