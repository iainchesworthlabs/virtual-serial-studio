//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "updates/update_service.hpp"

#include <vss/update/feed.hpp>
#include <vss/update/select.hpp>
#include <vss/update/version.hpp>

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1StringView>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace vss::gui::updates
{

namespace
{

namespace upd = vss::update;

/// How often the timer looks at the clock.
constexpr int kPollIntervalMs = 30 * 1000;
/// The most a file is read, and so hashed, in one turn of the event loop.
constexpr qint64 kHashPieceBytes = qint64{64} * 1024;
/// The length of the random name of a download's folder.
constexpr int kFolderNameLength = 16;
/// How many names " (2)", " (3)" ... are tried in the downloads folder.
constexpr int kMostNumberedNames = 1000;

QString iso_text(const QDateTime& when)
{
    return when.toUTC().toString(Qt::ISODate);
}

/// The time in a stored value, in UTC, or an invalid time.
QDateTime read_time(const QVariant& stored)
{
    const QDateTime parsed =
        QDateTime::fromString(stored.toString(), Qt::ISODate);
    if (!parsed.isValid())
        return {};
    return parsed.toUTC();
}

/// The feed serial in a stored value. Anything that is not a whole number
/// counts as 0.
std::uint64_t read_serial(const QVariant& stored)
{
    bool is_number = false;
    const qulonglong value = stored.toString().toULongLong(&is_number);
    return is_number ? static_cast<std::uint64_t>(value) : 0;
}

bool is_link(const QFileInfo& info)
{
    return info.isSymLink() || info.isJunction();
}

bool path_is_taken(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() || is_link(info);
}

/// Removes one file or link. Best effort: a file that cannot be removed is
/// left, and the next start tries again.
void remove_file(const QString& path)
{
    if (QFile::remove(path))
        return;
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (QFile::remove(path))
        return;
    // A link to a folder is removed like a folder on Windows.
    QDir().rmdir(path);
}

/// Removes `path` and everything under it. A link is removed and never
/// followed, so nothing outside `path` is touched.
void remove_tree(const QString& path)
{
    const QFileInfo info(path);
    if (is_link(info) || !info.isDir())
    {
        remove_file(path);
        return;
    }
    const QDir folder(path);
    const QFileInfoList entries = folder.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries)
        remove_tree(entry.absoluteFilePath());
    QDir().rmdir(path);
}

/// `name` with " (n)" before a known archive suffix, or at the end.
QString numbered_name(const QString& name, int number)
{
    constexpr std::array<QLatin1StringView, 4> kSuffixes{
        QLatin1StringView(".tar.gz"), QLatin1StringView(".tar.xz"),
        QLatin1StringView(".zip"), QLatin1StringView(".tgz")};
    qsizetype cut = name.size();
    for (const QLatin1StringView suffix : kSuffixes)
    {
        if (name.endsWith(suffix, Qt::CaseInsensitive))
        {
            cut = name.size() - suffix.size();
            break;
        }
    }
    return name.left(cut) + QStringLiteral(" (%1)").arg(number) + name.mid(cut);
}

/// Moves `from` into `folder` under `name`, or under a numbered name when
/// that is taken. Nothing is overwritten. Nothing when it cannot be done.
std::optional<QString> move_without_overwrite(const QString& from,
                                              const QString& folder,
                                              const QString& name)
{
    const QDir target_folder(folder);
    for (int number = 1; number <= kMostNumberedNames; ++number)
    {
        QString target = target_folder.filePath(
            number == 1 ? name : numbered_name(name, number));
        if (path_is_taken(target))
            continue;
        // `QFile::rename` never replaces a file that is there.
        if (QFile::rename(from, target))
            return target;
        if (!path_is_taken(target))
            return std::nullopt;
    }
    return std::nullopt;
}

bool is_archive(upd::file_kind kind)
{
    return kind == upd::file_kind::windows_zip ||
           kind == upd::file_kind::macos_archive ||
           kind == upd::file_kind::linux_archive;
}

offered_file make_offered_file(const upd::studio_file& file)
{
    offered_file out;
    out.name = QString::fromStdString(file.name);
    out.url = QUrl(QString::fromStdString(file.url), QUrl::StrictMode);
    out.size = file.size;
    out.sha512 = file.sha512;
    return out;
}

studio_offer make_offer(const upd::studio_release& release,
                        const upd::studio_file* file)
{
    studio_offer offer;
    offer.version = release.version;
    offer.version_text =
        QString::fromStdString(upd::to_string(release.version));
    offer.summary = QString::fromStdString(release.summary);
    offer.notes_url =
        QUrl(QString::fromStdString(release.notes_url), QUrl::StrictMode);
    if (file != nullptr)
        offer.file = make_offered_file(*file);
    return offer;
}

/// The greatest version among the Studio entries of the feed that the
/// pre-release setting allows. Null when there is none.
const upd::studio_release* newest_release(const upd::feed& source,
                                          bool include_prerelease)
{
    const upd::studio_release* best = nullptr;
    for (const upd::studio_release& release : source.studio)
    {
        if (release.channel == upd::channel::preview && !include_prerelease)
            continue;
        if (best == nullptr || upd::compare(release.version, best->version) > 0)
            best = &release;
    }
    return best;
}

QString random_folder_name()
{
    return QString::number(QRandomGenerator::system()->generate64(), 16)
        .rightJustified(kFolderNameLength, QLatin1Char('0'));
}

} // namespace

// -- the private part -------------------------------------------------------

class UpdateService::impl
{
public:
    impl(UpdateService& owner, config settings, feed_transport& transport)
        : owner_(owner), config_(std::move(settings)), transport_(transport),
          guard_(std::make_shared<int>(0)), timer_(new QTimer(&owner))
    {
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    ~impl()
    {
        // Late callbacks find the guard gone.
        guard_.reset();
        if (feed_in_flight_ || download_in_flight_)
            transport_.cancel();
        output_.reset();
        held_.reset();
        if (!pending_dir_.isEmpty() &&
            (owner_.state_.download == download_state::downloading ||
             owner_.state_.download == download_state::verifying))
            remove_tree(pending_dir_);
    }

    /// Reads the settings. Called once the owner's `state_` exists.
    void initialise()
    {
        snapshot& state = owner_.state_;
        const auto parsed =
            upd::parse_version(config_.running_version.toStdString());
        if (parsed.has_value())
            running_ = *parsed;
        state.developer_build =
            running_.has_value() && upd::is_developer_build(*running_);
        state.last_checked = read_time(QSettings().value(keys::kLastCheck));
        if (config_.disabled_by_policy)
            state.state = check_state::disabled_by_policy;
        else if (config_.keys.empty())
            state.state = check_state::no_keys;
        timer_->setInterval(kPollIntervalMs);
        QObject::connect(timer_, &QTimer::timeout, &owner_,
                         &UpdateService::poll);
    }

    // -- settings -----------------------------------------------------------

    bool available() const noexcept
    {
        return !config_.disabled_by_policy && !config_.keys.empty();
    }

    bool automatic() const
    {
        return QSettings().value(keys::kAutomatic, false).toBool();
    }

    void set_automatic(bool on)
    {
        if (!available())
            return;
        const bool was = automatic();
        QSettings().setValue(keys::kAutomatic, on);
        if (was != on)
            changed();
    }

    bool include_prerelease() const
    {
        return QSettings().value(keys::kIncludePrerelease, false).toBool();
    }

    void set_include_prerelease(bool on)
    {
        if (!available())
            return;
        const bool was = include_prerelease();
        QSettings().setValue(keys::kIncludePrerelease, on);
        if (was != on)
            changed();
    }

    bool question_due() const
    {
        return available() && !owner_.state_.developer_build &&
               !QSettings().value(keys::kAsked, false).toBool();
    }

    void answer_question(bool turn_on)
    {
        if (!available())
            return;
        {
            QSettings settings;
            settings.setValue(keys::kAsked, true);
            settings.setValue(keys::kAutomatic, turn_on);
        }
        changed();
    }

    QString skipped_version() const
    {
        return QSettings().value(keys::kSkippedVersion).toString();
    }

    // -- start and the timer --------------------------------------------------

    void start()
    {
        if (started_)
            return;
        started_ = true;
        started_at_ = config_.now();
        timer_->start();
        empty_updates_folder();
    }

    void poll()
    {
        if (!started_ || !available() || !running_.has_value() ||
            owner_.state_.developer_build || !automatic())
            return;
        const QDateTime now = config_.now();
        if (started_at_.msecsTo(now) < qint64{kStartDelaySeconds} * 1000)
            return;
        if (owner_.state_.state == check_state::checking || download_busy())
            return;
        if (!timer_check_due(now))
            return;
        begin_check(false);
    }

    void check_now() { begin_check(true); }

    // -- what to say unasked --------------------------------------------------

    bool chip_due() const
    {
        const std::optional<studio_offer>& offer = owner_.state_.offer;
        if (!offer.has_value() || !offer_showing())
            return false;
        return last_check_by_hand_ || skipped_version() != offer->version_text;
    }

    bool notice_due() const
    {
        const std::optional<studio_offer>& offer = owner_.state_.offer;
        if (!offer.has_value() || !chip_due())
            return false;
        return found_by_timer_ && !notice_hidden_ &&
               skipped_version() != offer->version_text;
    }

    void hide_notice()
    {
        if (notice_hidden_)
            return;
        notice_hidden_ = true;
        changed();
    }

    void skip_offered_version()
    {
        const std::optional<studio_offer>& offer = owner_.state_.offer;
        if (!offer.has_value() || !offer_showing())
            return;
        QSettings().setValue(keys::kSkippedVersion, offer->version_text);
        changed();
    }

    // -- the download ---------------------------------------------------------

    void start_download()
    {
        snapshot& state = owner_.state_;
        if (state.download != download_state::none &&
            state.download != download_state::failed)
            return;
        if (!available() || state.state != check_state::available ||
            !state.offer.has_value() || !state.offer->file.has_value() ||
            !kind_.has_value())
            return;

        const offered_file file = *state.offer->file;
        download_file_ = file;
        download_version_ = state.offer->version_text;
        download_kind_ = kind_;
        ++download_generation_;
        write_failed_ = false;

        state.download = download_state::downloading;
        state.download_error = download_failure::none;
        state.received = 0;
        state.total = file.size;
        state.ready_path.clear();
        changed();

        if (!open_pending_file(file.name) || !file.url.isValid() ||
            file.url.scheme() != QLatin1String("https"))
        {
            fail_download(download_failure::save_failed);
            return;
        }

        const std::weak_ptr<int> guard = guard_;
        const std::uint64_t generation = download_generation_;
        download_in_flight_ = true;
        transport_.start_download(
            file.url, file.size,
            [this, guard, generation](QByteArrayView data)
            {
                if (guard.expired() || generation != download_generation_)
                    return false;
                return on_chunk(data);
            },
            [this, guard, generation](transport_error error)
            {
                if (guard.expired() || generation != download_generation_)
                    return;
                download_in_flight_ = false;
                on_download_done(error);
            });
    }

    void cancel_download()
    {
        snapshot& state = owner_.state_;
        switch (state.download)
        {
        case download_state::downloading:
            ++download_generation_;
            // The transport's `done` follows with `cancelled` and finds the
            // generation changed.
            download_in_flight_ = false;
            transport_.cancel();
            break;
        case download_state::verifying:
            ++download_generation_;
            break;
        case download_state::ready:
            break;
        case download_state::none:
        case download_state::failed:
            return;
        }
        discard_download();
    }

    void clear_download_failure()
    {
        snapshot& state = owner_.state_;
        if (state.download != download_state::failed)
            return;
        state.download = download_state::none;
        state.download_error = download_failure::none;
        state.received = 0;
        state.total = 0;
        changed();
    }

    const checked_file* ready_file() const noexcept
    {
        return owner_.state_.download == download_state::ready ? held_.get()
                                                               : nullptr;
    }

    install_start install_ready_file()
    {
        const snapshot& state = owner_.state_;
        if (state.download != download_state::ready)
            return install_start::failed;
        if (!held_ || !download_kind_.has_value())
            // An archive was moved to the downloads folder: there is no
            // hand-over for it.
            return install_start::not_supported;
        const upd::file_kind kind = *download_kind_;
        const install_start answer = config_.start_install(kind, *held_);
        if (answer == install_start::started &&
            kind == upd::file_kind::windows_installer)
            Q_EMIT owner_.quit_requested();
        return answer;
    }

private:
    struct shown_state
    {
        check_state state = check_state::idle;
        check_failure failure = check_failure::none;
    };

    void changed() { Q_EMIT owner_.changed(); }

    bool download_busy() const
    {
        return owner_.state_.download == download_state::downloading ||
               owner_.state_.download == download_state::verifying;
    }

    /// An update is on show: the state is `available`, or a check that
    /// started from `available` is running.
    bool offer_showing() const
    {
        const snapshot& state = owner_.state_;
        if (!available() || !state.offer.has_value())
            return false;
        const check_state shown = state.state == check_state::checking
                                      ? shown_before_.state
                                      : state.state;
        return shown == check_state::available;
    }

    // -- the cache folder -----------------------------------------------------

    QString updates_folder() const
    {
        if (config_.cache_folder.isEmpty())
            return {};
        return QDir(config_.cache_folder).filePath(QStringLiteral("updates"));
    }

    /// Makes `<cache_folder>/updates` when it is missing. False when it is
    /// not a plain folder: a link there is never followed.
    bool prepare_updates_folder(const QString& folder) const
    {
        if (folder.isEmpty())
            return false;
        if (is_link(QFileInfo(folder)))
            return false;
        if (!QDir().mkpath(folder))
            return false;
        return QFileInfo(folder).isDir();
    }

    void empty_updates_folder() const
    {
        const QString folder = updates_folder();
        if (!prepare_updates_folder(folder))
            return;
        const QFileInfoList entries =
            QDir(folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                       QDir::Hidden | QDir::System);
        for (const QFileInfo& entry : entries)
            remove_tree(entry.absoluteFilePath());
    }

    // -- the check ------------------------------------------------------------

    bool timer_check_due(const QDateTime& now) const
    {
        if (retry_not_before_.has_value())
            return now >= *retry_not_before_;
        const QDateTime last = owner_.state_.last_checked;
        // A clock set back since the last check must not hold the next one
        // off for good.
        if (!last.isValid() || last > now)
            return true;
        return last.secsTo(now) >= qint64{kCheckIntervalSeconds};
    }

    void schedule_retry()
    {
        const QDateTime now = config_.now();
        if (timer_failures_ == 0)
            first_failure_ = now;
        ++timer_failures_;
        if (timer_failures_ <= kRetriesPerDay)
        {
            retry_not_before_ = now.addSecs(kRetryIntervalSeconds);
            return;
        }
        // The last retry failed too: nothing more until a day after the
        // first failure, and then a new round.
        retry_not_before_ = first_failure_.addSecs(kCheckIntervalSeconds);
        timer_failures_ = 0;
    }

    void begin_check(bool by_hand)
    {
        snapshot& state = owner_.state_;
        if (!available())
            return;
        if (state.state == check_state::checking)
        {
            // The person asked for a result: a timer check that is already
            // running is no longer silent.
            if (by_hand)
                check_by_hand_ = true;
            return;
        }
        if (download_busy())
            return;

        check_by_hand_ = by_hand;
        shown_before_ = {state.state, state.failure};
        state.state = check_state::checking;
        state.failure = check_failure::none;
        ++check_generation_;
        changed();

        if (!running_.has_value())
        {
            // The program cannot tell what is newer, so no request is made.
            end_check_failed(check_failure::untrusted);
            return;
        }
        if (kind_.has_value())
            fetch();
        else
            ask_probe();
    }

    void ask_probe()
    {
        if (probe_asked_)
            return;
        probe_asked_ = true;
        const std::weak_ptr<int> guard = guard_;
        const std::uint64_t generation = check_generation_;
        config_.probe->detect(
            [this, guard, generation](upd::file_kind found)
            {
                if (guard.expired())
                    return;
                kind_ = found;
                owner_.state_.install_kind = found;
                changed();
                if (generation == check_generation_ &&
                    owner_.state_.state == check_state::checking)
                    fetch();
            });
    }

    void fetch()
    {
        const std::weak_ptr<int> guard = guard_;
        const std::uint64_t generation = check_generation_;
        feed_in_flight_ = true;
        transport_.fetch_feed(
            [this, guard, generation](transport_error error,
                                      const QByteArray& body)
            {
                if (guard.expired() || generation != check_generation_)
                    return;
                feed_in_flight_ = false;
                on_feed(error, body);
            });
    }

    void on_feed(transport_error error, const QByteArray& body)
    {
        if (error != transport_error::none)
        {
            end_check_failed(check_failure::unreachable);
            return;
        }
        const std::string_view text(body.constData(),
                                    static_cast<std::size_t>(body.size()));
        const std::uint64_t highest =
            read_serial(QSettings().value(keys::kLastSerial));
        const auto read = upd::read_feed(text, config_.keys, highest);
        if (!read.has_value())
        {
            end_check_failed(check_failure::untrusted);
            return;
        }
        accept_feed(*read, highest);
    }

    void end_check_failed(check_failure why)
    {
        snapshot& state = owner_.state_;
        if (check_by_hand_)
        {
            state.state = check_state::failed;
            state.failure = why;
        }
        else
        {
            // A check on the timer is silent: what was shown stays shown.
            state.state = shown_before_.state;
            state.failure = shown_before_.failure;
            schedule_retry();
        }
        changed();
    }

    void accept_feed(const upd::feed& source, std::uint64_t highest)
    {
        snapshot& state = owner_.state_;
        const QString now_text = iso_text(config_.now());
        {
            QSettings settings;
            if (source.serial > highest)
                settings.setValue(
                    keys::kLastSerial,
                    QString::number(static_cast<qulonglong>(source.serial)));
            settings.setValue(keys::kLastCheck, now_text);
        }
        state.last_checked = read_time(now_text);
        timer_failures_ = 0;
        retry_not_before_.reset();

        const std::optional<studio_offer> previous = state.offer;
        std::optional<studio_offer> next;
        if (state.developer_build)
        {
            // Never offered an install: the newest release is named only.
            const upd::studio_release* const newest =
                newest_release(source, include_prerelease());
            if (newest != nullptr)
                next = make_offer(*newest, nullptr);
            state.state = check_state::current;
        }
        else
        {
            // A check asks the probe before it asks for the feed, so the
            // kind and the version are known here.
            const std::optional<upd::studio_update> chosen =
                (running_.has_value() && kind_.has_value())
                    ? upd::choose_studio(source, *running_,
                                         include_prerelease(), *kind_,
                                         config_.architecture)
                    : std::nullopt;
            if (chosen.has_value())
            {
                next = make_offer(*chosen->release, chosen->file);
                state.state = check_state::available;
            }
            else
            {
                state.state = check_state::current;
            }
        }
        state.failure = check_failure::none;

        const bool same_version = previous.has_value() && next.has_value() &&
                                  previous->version_text == next->version_text;
        found_by_timer_ =
            next.has_value() && !state.developer_build &&
            (!check_by_hand_ || (same_version && found_by_timer_));
        last_check_by_hand_ = check_by_hand_;

        drop_download_of_another_offer(next);
        state.offer = std::move(next);
        changed();
    }

    // -- the download ---------------------------------------------------------

    /// A file held for an older offer is let go of when the new offer's file
    /// is another one.
    void drop_download_of_another_offer(const std::optional<studio_offer>& next)
    {
        if (owner_.state_.download != download_state::ready)
            return;
        const bool same = next.has_value() && next->file.has_value() &&
                          download_file_.has_value() &&
                          next->version_text == download_version_ &&
                          *next->file == *download_file_;
        if (!same)
            discard_download();
    }

    /// Lets go of the file, deletes the pending folder and returns to
    /// `none`.
    void discard_download()
    {
        snapshot& state = owner_.state_;
        close_files();
        remove_pending_folder();
        state.download = download_state::none;
        state.download_error = download_failure::none;
        state.received = 0;
        state.total = 0;
        state.ready_path.clear();
        changed();
    }

    void fail_download(download_failure why)
    {
        snapshot& state = owner_.state_;
        // Anything the transport still calls is stale.
        ++download_generation_;
        close_files();
        remove_pending_folder();
        state.download = download_state::failed;
        state.download_error = why;
        state.ready_path.clear();
        changed();
    }

    void close_files()
    {
        output_.reset();
        held_.reset();
    }

    void remove_pending_folder()
    {
        if (pending_dir_.isEmpty())
            return;
        remove_tree(pending_dir_);
        pending_dir_.clear();
        file_path_.clear();
    }

    /// Makes the download's own folder and opens the file in it.
    bool open_pending_file(const QString& name)
    {
        const QString updates = updates_folder();
        if (!prepare_updates_folder(updates))
            return false;
        QString folder;
        const QDir parent(updates);
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const QString candidate = random_folder_name();
            if (parent.mkdir(candidate))
            {
                folder = parent.filePath(candidate);
                break;
            }
        }
        if (folder.isEmpty())
            return false;
        pending_dir_ = folder;
        file_path_ = QDir(folder).filePath(name);

        std::unique_ptr<QFile> file = config_.create_file
                                          ? config_.create_file(file_path_)
                                          : std::make_unique<QFile>(file_path_);
        if (!file || !file->open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        output_ = std::move(file);
        return true;
    }

    bool on_chunk(QByteArrayView data)
    {
        if (!output_)
            return false;
        const qint64 size = data.size();
        if (output_->write(data.constData(), size) != size)
        {
            // The transport stops and says `cancelled`; the failure is
            // reported then.
            write_failed_ = true;
            return false;
        }
        owner_.state_.received += static_cast<std::uint64_t>(size);
        Q_EMIT owner_.download_progress();
        return true;
    }

    void on_download_done(transport_error error)
    {
        snapshot& state = owner_.state_;
        if (write_failed_)
        {
            fail_download(download_failure::save_failed);
            return;
        }
        if (error != transport_error::none)
        {
            fail_download(error == transport_error::too_large
                              ? download_failure::mismatch
                              : download_failure::unreachable);
            return;
        }
        if (!output_ || !output_->flush() ||
            output_->error() != QFileDevice::NoError)
        {
            fail_download(download_failure::save_failed);
            return;
        }
        output_.reset();
        if (state.received != state.total)
        {
            fail_download(download_failure::mismatch);
            return;
        }
        begin_verify();
    }

    void begin_verify()
    {
        owner_.state_.download = download_state::verifying;
        changed();
        held_ = config_.open_file(file_path_);
        if (!held_)
        {
            fail_download(download_failure::save_failed);
            return;
        }
        hasher_.reset();
        verified_ = 0;
        schedule_verify_step();
    }

    /// One piece per turn of the event loop, so the GUI thread is never held
    /// for long.
    void schedule_verify_step()
    {
        const std::weak_ptr<int> guard = guard_;
        const std::uint64_t generation = download_generation_;
        QMetaObject::invokeMethod(
            &owner_,
            [this, guard, generation]()
            {
                if (guard.expired() || generation != download_generation_)
                    return;
                verify_step();
            },
            Qt::QueuedConnection);
    }

    void verify_step()
    {
        if (!held_)
            return;
        const std::optional<QByteArray> piece = held_->read(kHashPieceBytes);
        if (!piece.has_value())
        {
            fail_download(download_failure::save_failed);
            return;
        }
        if (piece->isEmpty())
        {
            finish_verify();
            return;
        }
        verified_ += static_cast<std::uint64_t>(piece->size());
        if (verified_ > owner_.state_.total)
        {
            fail_download(download_failure::mismatch);
            return;
        }
        hasher_.addData(*piece);
        schedule_verify_step();
    }

    void finish_verify()
    {
        const QByteArray digest = hasher_.result();
        const bool same =
            download_file_.has_value() && verified_ == owner_.state_.total &&
            std::cmp_equal(digest.size(), download_file_->sha512.size()) &&
            std::equal(digest.begin(), digest.end(),
                       download_file_->sha512.begin(),
                       [](char left, std::uint8_t right)
                       { return static_cast<std::uint8_t>(left) == right; });
        if (!same)
        {
            fail_download(download_failure::mismatch);
            return;
        }
        if (download_kind_.has_value() && is_archive(*download_kind_))
            finish_archive();
        else
            finish_held_file();
    }

    /// The file is kept open and checked until the install step.
    void finish_held_file()
    {
        snapshot& state = owner_.state_;
        state.ready_path = held_->path();
        state.download = download_state::ready;
        changed();
    }

    /// An archive is moved into the person's downloads folder, and nothing
    /// is held.
    void finish_archive()
    {
        snapshot& state = owner_.state_;
        held_.reset();
        if (config_.downloads_folder.isEmpty() ||
            !QDir().mkpath(config_.downloads_folder) ||
            !download_file_.has_value())
        {
            fail_download(download_failure::save_failed);
            return;
        }
        const auto moved = move_without_overwrite(
            file_path_, config_.downloads_folder, download_file_->name);
        if (!moved.has_value())
        {
            fail_download(download_failure::save_failed);
            return;
        }
        remove_pending_folder();
        state.ready_path = *moved;
        state.download = download_state::ready;
        changed();
    }

    UpdateService& owner_;
    config config_;
    feed_transport& transport_;
    /// Callbacks hold a weak pointer to this: it is gone once the service is.
    std::shared_ptr<int> guard_;
    QTimer* timer_;
    std::optional<upd::version> running_;

    bool started_ = false;
    QDateTime started_at_;

    // The check.
    std::uint64_t check_generation_ = 0;
    bool check_by_hand_ = false;
    shown_state shown_before_;
    std::optional<upd::file_kind> kind_;
    bool probe_asked_ = false;
    bool feed_in_flight_ = false;

    // The timer's retries live in memory only.
    std::optional<QDateTime> retry_not_before_;
    int timer_failures_ = 0;
    QDateTime first_failure_;

    // The notices.
    bool last_check_by_hand_ = false;
    bool found_by_timer_ = false;
    bool notice_hidden_ = false;

    // The download.
    std::uint64_t download_generation_ = 0;
    bool download_in_flight_ = false;
    bool write_failed_ = false;
    std::optional<offered_file> download_file_;
    QString download_version_;
    std::optional<upd::file_kind> download_kind_;
    QString pending_dir_;
    QString file_path_;
    std::unique_ptr<QFile> output_;
    std::unique_ptr<checked_file> held_;
    QCryptographicHash hasher_{QCryptographicHash::Sha512};
    std::uint64_t verified_ = 0;
};

// -- the public part --------------------------------------------------------

UpdateService::UpdateService(config settings, feed_transport& transport,
                             QObject* parent)
    : QObject(parent),
      impl_(std::make_unique<impl>(*this, std::move(settings), transport))
{
    // `state_` is made after `impl_`, so the settings are read here.
    impl_->initialise();
}

UpdateService::~UpdateService()
{
    // While `state_` and the timer still exist.
    impl_.reset();
}

bool UpdateService::automatic() const
{
    return impl_->automatic();
}

void UpdateService::set_automatic(bool on)
{
    impl_->set_automatic(on);
}

bool UpdateService::include_prerelease() const
{
    return impl_->include_prerelease();
}

void UpdateService::set_include_prerelease(bool on)
{
    impl_->set_include_prerelease(on);
}

bool UpdateService::available() const noexcept
{
    return impl_->available();
}

bool UpdateService::question_due() const
{
    return impl_->question_due();
}

void UpdateService::answer_question(bool turn_on)
{
    impl_->answer_question(turn_on);
}

void UpdateService::start()
{
    impl_->start();
}

void UpdateService::poll()
{
    impl_->poll();
}

void UpdateService::check_now()
{
    impl_->check_now();
}

bool UpdateService::chip_due() const
{
    return impl_->chip_due();
}

bool UpdateService::notice_due() const
{
    return impl_->notice_due();
}

void UpdateService::hide_notice()
{
    impl_->hide_notice();
}

void UpdateService::skip_offered_version()
{
    impl_->skip_offered_version();
}

QString UpdateService::skipped_version() const
{
    return impl_->skipped_version();
}

void UpdateService::start_download()
{
    impl_->start_download();
}

void UpdateService::cancel_download()
{
    impl_->cancel_download();
}

void UpdateService::clear_download_failure()
{
    impl_->clear_download_failure();
}

const checked_file* UpdateService::ready_file() const noexcept
{
    return impl_->ready_file();
}

install_start UpdateService::install_ready_file()
{
    return impl_->install_ready_file();
}

} // namespace vss::gui::updates
