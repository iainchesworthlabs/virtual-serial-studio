//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What every test of the update service shares: signed feeds made with the
// throwaway test key, a fake probe, a fake clock, a fake checked file and a
// harness that wires them to a service. Nothing here makes a request or reads
// the system clock, and the settings are the test program's own (see
// `main.cpp`).
//

#pragma once

#include "fake_transport.hpp"
#include "test_framework.hpp"

#include "updates/update_service.hpp"

#include <vss/license/sign.hpp>
#include <vss/license/signed_file.hpp>
#include <vss/license/test_keys.hpp>
#include <vss/update/feed.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QTime>
#include <QTimeZone>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::gui::updates::testing
{

namespace upd = vss::update;
namespace sf = vss::license::signed_file;
namespace signing = vss::license::signing;
namespace test_keys = vss::license::test_keys;

// -- the feed -----------------------------------------------------------------

/// `size` bytes that differ from one `seed` to the next.
inline QByteArray payload_bytes(int size, char seed = 'a')
{
    QByteArray bytes;
    bytes.reserve(size);
    for (int i = 0; i < size; ++i)
        bytes.append(static_cast<char>(seed + (i * 7) % 23));
    return bytes;
}

struct file_spec
{
    upd::file_kind kind = upd::file_kind::windows_installer;
    upd::architecture arch = upd::architecture::x86_64;
    std::string name = "studio-0.3.0-windows-x86_64.exe";
    QByteArray bytes = payload_bytes(1000);
    /// What the feed says, when it is not to be what `bytes` is.
    std::optional<std::uint64_t> size;
    std::optional<std::string> sha512_hex;
};

struct release_spec
{
    std::string channel = "stable";
    std::string version = "0.3.0";
    std::string summary = "What is new.";
    std::vector<file_spec> files{file_spec{}};
};

/// A stable or preview release with one Windows installer.
inline release_spec make_release(std::string version,
                                 std::string channel = "stable",
                                 const QByteArray& bytes = payload_bytes(1000))
{
    release_spec out;
    out.channel = std::move(channel);
    out.files[0].name = "studio-" + version + "-windows-x86_64.exe";
    out.files[0].bytes = bytes;
    out.version = std::move(version);
    return out;
}

inline std::string hex_of(const QByteArray& bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha512)
        .toHex()
        .toStdString();
}

inline std::string payload_text(std::uint64_t serial,
                                const std::vector<release_spec>& releases)
{
    std::string text = R"({"feed_version": 1, "serial": )" +
                       std::to_string(serial) +
                       R"(, "issued_at": "2026-12-01T03:40:00Z", "studio": [)";
    bool first_release = true;
    for (const release_spec& entry : releases)
    {
        if (!first_release)
            text += ", ";
        first_release = false;
        text += R"({"channel": ")" + entry.channel + R"(", "version": ")" +
                entry.version + R"(", "released": "2026-12-01", "summary": ")" +
                entry.summary +
                R"(", "notes_url": "https://example.org/notes/)" +
                entry.version + R"(", "files": [)";
        bool first_file = true;
        for (const file_spec& file : entry.files)
        {
            if (!first_file)
                text += ", ";
            first_file = false;
            const std::uint64_t size = file.size.value_or(
                static_cast<std::uint64_t>(file.bytes.size()));
            text += R"({"kind": ")" + std::string(upd::to_string(file.kind)) +
                    R"(", "arch": ")" + std::string(upd::to_string(file.arch)) +
                    R"(", "name": ")" + file.name +
                    R"(", "url": "https://example.org/dl/)" + file.name +
                    R"(", "size": )" + std::to_string(size) +
                    R"(, "sha512": ")" +
                    file.sha512_hex.value_or(hex_of(file.bytes)) + R"("})";
        }
        text += "]}";
    }
    return text + R"(], "plugins": []})";
}

inline std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    std::vector<std::uint8_t> bytes;
    for (const char c : text)
        bytes.push_back(static_cast<std::uint8_t>(c));
    return bytes;
}

inline sf::trusted_key feed_key()
{
    sf::trusted_key key;
    key.key_id = std::string(test_keys::key_id);
    key.purpose = std::string(upd::feed_purpose);
    key.key = signing::public_key_of(test_keys::private_key());
    return key;
}

/// The envelope around `payload`, signed with the throwaway test key.
inline QByteArray signed_body(std::string_view payload,
                              std::string_view purpose = upd::feed_purpose)
{
    const std::string text =
        sf::write(signing::sign(purpose, test_keys::key_id, bytes_of(payload),
                                test_keys::private_key()));
    return QByteArray(text.data(), static_cast<qsizetype>(text.size()));
}

/// A feed with `releases`, signed and ready to be a reply.
inline QByteArray feed_body(std::uint64_t serial,
                            const std::vector<release_spec>& releases)
{
    return signed_body(payload_text(serial, releases));
}

/// A valid envelope whose signature has one bit wrong.
inline QByteArray badly_signed_body(std::uint64_t serial,
                                    const std::vector<release_spec>& releases)
{
    const std::string text = payload_text(serial, releases);
    auto envelope = signing::sign(upd::feed_purpose, test_keys::key_id,
                                  bytes_of(text), test_keys::private_key());
    envelope.sig[0] = static_cast<std::uint8_t>(envelope.sig[0] ^ 0x01);
    const std::string written = sf::write(envelope);
    return QByteArray(written.data(), static_cast<qsizetype>(written.size()));
}

/// The digest as the 128 lower-case hex characters the feed writes.
inline QString digest_hex(const upd::sha512_digest& digest)
{
    const QByteArray bytes(reinterpret_cast<const char*>(digest.data()),
                           static_cast<qsizetype>(digest.size()));
    return QString::fromLatin1(bytes.toHex());
}

inline QByteArray read_all(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

inline void write_all(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw vss_test::failure("cannot write " + path.toStdString());
}

/// The name of a download's own folder: 16 hex characters.
inline bool is_random_name(const QString& name)
{
    if (name.size() != 16)
        return false;
    for (const QChar c : name)
    {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    }
    return true;
}

// -- the fakes ----------------------------------------------------------------

class fake_probe final : public install_kind_probe
{
public:
    upd::file_kind answer = upd::file_kind::windows_installer;
    /// False: `detect` keeps `done` until the test calls `answer_now`.
    bool at_once = true;
    int calls = 0;

    void detect(std::function<void(upd::file_kind)> done) override
    {
        ++calls;
        if (at_once)
            done(answer);
        else
            pending_ = std::move(done);
    }

    bool waiting() const { return static_cast<bool>(pending_); }

    void answer_now()
    {
        auto done = std::move(pending_);
        pending_ = nullptr;
        done(answer);
    }

private:
    std::function<void(upd::file_kind)> pending_;
};

/// What the test sees of a fake checked file after the service has used it.
struct file_log
{
    bool released = false;
    int reads = 0;
    qint64 largest_read = 0;
    /// A read after this many reads fails.
    std::optional<int> fail_at_read;
};

/// Reads the real file, in the pieces it is asked for.
class fake_checked_file final : public checked_file
{
public:
    fake_checked_file(const QString& path, std::shared_ptr<file_log> log)
        : file_(path), log_(std::move(log))
    {
        opened_ = file_.open(QIODevice::ReadOnly);
    }

    ~fake_checked_file() override { log_->released = true; }

    bool opened() const { return opened_; }

    QString path() const override { return file_.fileName(); }

    std::optional<QByteArray> read(qint64 max) override
    {
        ++log_->reads;
        if (log_->largest_read < max)
            log_->largest_read = max;
        if (log_->fail_at_read.has_value() && log_->reads > *log_->fail_at_read)
            return std::nullopt;
        return file_.read(max);
    }

private:
    QFile file_;
    std::shared_ptr<file_log> log_;
    bool opened_ = false;
};

/// A file that takes `limit` bytes and then fails, as a full disk does.
class failing_file final : public QFile
{
public:
    failing_file(const QString& path, qint64 limit_bytes)
        : QFile(path), limit_(limit_bytes)
    {
    }

protected:
    qint64 writeData(const char* data, qint64 size) override
    {
        if (written_ + size > limit_)
            return -1;
        written_ += size;
        return QFile::writeData(data, size);
    }

private:
    qint64 limit_;
    qint64 written_ = 0;
};

struct install_call
{
    upd::file_kind kind;
    const checked_file* file = nullptr;
};

// -- the harness --------------------------------------------------------------

inline QDateTime start_time()
{
    return QDateTime(QDate(2026, 12, 1), QTime(12, 0), QTimeZone::UTC);
}

class harness
{
public:
    harness()
    {
        QSettings().clear();
        if (!cache_dir.isValid() || !downloads_dir.isValid())
            throw vss_test::failure("no temporary folder");
        settings.running_version = QStringLiteral("0.2.0");
        settings.keys = {feed_key()};
        now = start_time();
    }

    ~harness()
    {
        // The service first: it uses everything below.
        service.reset();
    }

    harness(const harness&) = delete;
    harness& operator=(const harness&) = delete;

    /// Makes the service from `settings`, which the test may change first.
    void create()
    {
        UpdateService::config given = settings;
        given.now = [this]() { return now; };
        given.probe = probe;
        given.cache_folder = no_cache_folder ? QString() : cache_dir.path();
        given.downloads_folder =
            downloads_override.value_or(downloads_dir.path());
        given.open_file = [this](const QString& path)
        { return open_file(path); };
        given.start_install =
            [this](upd::file_kind kind, const checked_file& file)
        {
            installs.push_back({kind, &file});
            return install_answer;
        };
        if (use_failing_file)
        {
            given.create_file = [this](const QString& path)
            {
                return std::unique_ptr<QFile>(
                    std::make_unique<failing_file>(path, failing_limit));
            };
        }
        service = std::make_unique<UpdateService>(std::move(given), transport);
        QObject::connect(service.get(), &UpdateService::changed, service.get(),
                         [this]() { ++changes; });
        QObject::connect(service.get(), &UpdateService::download_progress,
                         service.get(), [this]() { ++progress; });
        QObject::connect(service.get(), &UpdateService::quit_requested,
                         service.get(), [this]() { ++quits; });
    }

    /// Replaces the service with a new one on the same settings.
    void recreate()
    {
        service.reset();
        create();
    }

    const snapshot& state() const { return service->state(); }

    /// A check by hand answered with `body`.
    void check_with(const QByteArray& body)
    {
        service->check_now();
        REQUIRE(transport.feed_pending());
        transport.reply_feed(transport_error::none, body);
    }

    /// A check by hand answered with a transport error.
    void check_failing(transport_error error)
    {
        service->check_now();
        REQUIRE(transport.feed_pending());
        transport.reply_feed(error);
    }

    /// Moves the clock and lets the service look at it.
    void advance(qint64 seconds)
    {
        now = now.addSecs(seconds);
        service->poll();
    }

    /// Sets the clock to `seconds` after the start and polls.
    void set_time(qint64 seconds)
    {
        now = start_time().addSecs(seconds);
        service->poll();
    }

    /// Runs the event loop until `done` is true. The service defers work one
    /// piece per turn, so this is bounded by turns and never by time.
    void pump_until(const std::function<bool()>& done, int turns = 2000)
    {
        for (int i = 0; i < turns && !done(); ++i)
            QCoreApplication::processEvents();
    }

    void pump_download()
    {
        pump_until([this]()
                   { return state().download != download_state::verifying; });
    }

    /// The names inside `<cache>/updates`.
    QStringList cache_entries() const
    {
        return QDir(cache_dir.path() + QStringLiteral("/updates"))
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    }

    QStringList downloads_entries() const
    {
        return QDir(downloads_dir.path())
            .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    }

    // The test sets these before `create`.
    UpdateService::config settings;
    bool use_failing_file = false;
    qint64 failing_limit = 0;
    bool open_returns_null = false;
    install_start install_answer = install_start::started;
    /// The next fake checked file fails its reads after this many.
    std::optional<int> next_fail_at_read;

    /// The folder the service moves archives to, when it is not
    /// `downloads_dir`.
    std::optional<QString> downloads_override;
    /// The service is given no cache folder.
    bool no_cache_folder = false;

    QTemporaryDir cache_dir;
    QTemporaryDir downloads_dir;
    fake_transport transport;
    QDateTime now;
    std::shared_ptr<fake_probe> probe = std::make_shared<fake_probe>();

    // The test reads these.
    std::vector<install_call> installs;
    std::shared_ptr<file_log> last_file;
    int changes = 0;
    int progress = 0;
    int quits = 0;

    /// Last, so it goes first.
    std::unique_ptr<UpdateService> service;

private:
    std::unique_ptr<checked_file> open_file(const QString& path)
    {
        if (open_returns_null)
            return nullptr;
        auto log = std::make_shared<file_log>();
        log->fail_at_read = next_fail_at_read;
        auto file = std::make_unique<fake_checked_file>(path, log);
        if (!file->opened())
            return nullptr;
        last_file = std::move(log);
        return file;
    }
};

/// The offer on show, or a failed requirement.
inline const studio_offer& require_offer(const harness& h)
{
    if (!h.state().offer.has_value())
        throw vss_test::failure("the service shows no offer");
    return *h.state().offer;
}

/// `kind` as the optional the snapshot holds, for a comparison.
inline std::optional<upd::file_kind> upd_kind(upd::file_kind kind)
{
    return kind;
}

} // namespace vss::gui::updates::testing
