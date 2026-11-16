//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The update service: the one object the window reads for everything about
// updates (docs/updates.md; docs/gui-design/docs/screens/12-updates.md). It
// owns the settings, the timer, the check and the download. It lives on the
// GUI thread, never blocks it, and touches nothing of the engine.
//
// The network, the clock, the kind of install and the folders are given to
// it, so a test drives every state with fakes and makes no request.
//
// The order of a check is fixed, and is the reason the feed is signed:
//   1. the administrator's policy and the built-in keys decide whether a
//      request is made at all;
//   2. the reply is verified by `vss::update::read_feed` (signature first,
//      then the payload, the key's window and the serial) before anything
//      in it is used;
//   3. only a version greater than the running one is ever offered.
//

#pragma once

#include "updates/checked_file.hpp"
#include "updates/feed_transport.hpp"
#include "updates/install_kind.hpp"
#include "updates/update_types.hpp"

#include <vss/license/signed_file.hpp>

#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QTimer;

namespace vss::gui::updates
{

/// The names of the stored values (`QSettings`), all under `updates/`.
namespace keys
{
inline constexpr const char* kAutomatic = "updates/automatic";
inline constexpr const char* kAsked = "updates/asked";
inline constexpr const char* kIncludePrerelease = "updates/includePrerelease";
/// UTC, ISO 8601. When a feed was last accepted.
inline constexpr const char* kLastCheck = "updates/lastCheck";
/// The highest feed serial accepted, as a decimal string.
inline constexpr const char* kLastSerial = "updates/lastSerial";
/// The version the person chose to hear no more about.
inline constexpr const char* kSkippedVersion = "updates/skippedVersion";
} // namespace keys

/// The times of section 6.2.
inline constexpr int kStartDelaySeconds = 60;
inline constexpr int kCheckIntervalSeconds = 24 * 60 * 60;
inline constexpr int kRetryIntervalSeconds = 60 * 60;
inline constexpr int kRetriesPerDay = 3;

class UpdateService final : public QObject
{
    Q_OBJECT

public:
    struct config
    {
        /// The running program's version, as `build_info::version` gives it.
        /// Text that is not a semantic version makes every check answer
        /// `failed` with `untrusted`: the program cannot tell what is newer.
        QString running_version;
        /// The keys a feed is verified with: `vss::update::
        /// product_feed_keys()` in the program, a test key in a test. Empty
        /// means `no_keys`.
        std::vector<vss::license::signed_file::trusted_key> keys;
        /// An administrator's policy turned the check off (section 6.3).
        bool disabled_by_policy = false;
        /// The time now, in UTC. Never null.
        std::function<QDateTime()> now;
        /// Says how this copy was installed. Never null.
        std::shared_ptr<install_kind_probe> probe;
        vss::update::architecture architecture =
            vss::update::architecture::x86_64;
        /// Studio's own cache folder for downloads. The service makes and
        /// empties `<cache_folder>/updates`.
        QString cache_folder;
        /// Where an archive is put once it is checked: the person's
        /// Downloads folder.
        QString downloads_folder;
        /// Opens a downloaded file for checking: `open_checked_file` in the
        /// program, a fake in a test. Required, never null. The service
        /// does not name `open_checked_file` itself, so it links without the
        /// platform sources.
        std::function<std::unique_ptr<checked_file>(const QString&)> open_file;
        /// Hands a checked file to the system: `start_install` in the
        /// program, a fake in a test. Required, never null.
        std::function<install_start(vss::update::file_kind,
                                    const checked_file&)>
            start_install;
        /// Makes the object a download is written to. Optional: empty means
        /// a `QFile`. The service opens it for writing itself. A test gives
        /// one whose writes fail.
        std::function<std::unique_ptr<QFile>(const QString&)> create_file;
    };

    /// `transport` outlives the service.
    UpdateService(config settings, feed_transport& transport,
                  QObject* parent = nullptr);
    ~UpdateService() override;

    /// Everything on show.
    const snapshot& state() const noexcept { return state_; }

    // -- settings ---------------------------------------------------------

    /// "Check for updates automatically". False until the person says
    /// otherwise.
    bool automatic() const;
    void set_automatic(bool on);
    /// "Include pre-release versions".
    bool include_prerelease() const;
    void set_include_prerelease(bool on);
    /// Whether the settings may be changed and a check made at all: false
    /// in `disabled_by_policy` and `no_keys`.
    bool available() const noexcept;

    // -- the question at first start (section 3.5) ------------------------

    /// True when the banner "Check for updates automatically?" is to be
    /// shown: the person has not answered, no policy turns the check off,
    /// the build can check and it is not a developer build.
    bool question_due() const;
    /// Stores the answer for good and sets `automatic` to it. With
    /// `turn_on` the first check then runs as the timer rules say.
    void answer_question(bool turn_on);

    // -- the check --------------------------------------------------------

    /// The window is shown. Starts the clock of section 6.2 and deletes
    /// what an earlier run left in the download folder. Makes no request.
    void start();
    /// Looks at the time and starts a timer check when one is due: the
    /// automatic check is on, `start` was at least 60 seconds ago, no check
    /// or download is running, it is not a developer build, and 24 hours
    /// have passed since the last accepted feed (or, after a failed timer
    /// check, an hour, three times, and then not again until 24 hours after
    /// the first of those failures). The service calls this itself from a
    /// timer every 30 seconds; a test calls it after moving its clock.
    void poll();
    /// A check by hand. Always allowed unless `available()` is false. Does
    /// nothing while a check is running.
    void check_now();

    // -- what to say unasked (screens 12, "Update notice", "Update chip") --

    /// True when the status bar chip is shown: an update is on offer and
    /// either the last check was by hand or its version was not skipped.
    bool chip_due() const;
    /// True when the notice banner is shown: `chip_due()`, the offer was
    /// found by a timer check, and "Later" was not pressed in this run.
    bool notice_due() const;
    /// "Later": hides the notice until the next start. The chip stays.
    void hide_notice();
    /// "Skip this version": stores the offered version. A timer check stays
    /// quiet about it and speaks again for a later one.
    void skip_offered_version();
    QString skipped_version() const;

    // -- the download (section 7.2) ---------------------------------------

    /// Starts the download of the offered file. Needs `available` with a
    /// file and the install kind known. Does nothing while one is running.
    /// When a checked file for the same offer is already held (`ready`), it
    /// stays `ready`.
    void start_download();
    /// "Cancel": stops the download, deletes what arrived and returns to
    /// `download_state::none`. Reports nothing else.
    void cancel_download();
    /// Forgets a failed download, so the first page shows again.
    void clear_download_failure();
    /// The checked file, in `download_state::ready`. Null otherwise. The
    /// service keeps it until it is destroyed or a different offer replaces
    /// it.
    const checked_file* ready_file() const noexcept;
    /// Hands the checked file to the system (`start_install`). `started`
    /// for a Windows installer also emits `quit_requested`, so the window
    /// closes the way Quit does.
    install_start install_ready_file();

Q_SIGNALS:
    /// Anything in `state()`, `chip_due()`, `notice_due()`,
    /// `question_due()` or the settings changed.
    void changed();
    /// Bytes arrived. `state().received` and `total` hold the numbers.
    void download_progress();
    /// The installer has started and Studio is to close.
    void quit_requested();

private:
    class impl;
    std::unique_ptr<impl> impl_;
    snapshot state_;
};

} // namespace vss::gui::updates
