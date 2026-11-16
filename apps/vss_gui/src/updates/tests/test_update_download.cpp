//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The download: the bytes written, the check against the feed's size and
// SHA-512, what is kept and what is deleted, and the hand-over.
//

#include "fixture.hpp"

#include <QCoreApplication>

#include <algorithm>

using namespace vss::gui::updates;
using namespace vss::gui::updates::testing;

namespace
{

/// A service with an available update whose file is `bytes`, for an install
/// of `kind`.
void prepare(harness& h, const QByteArray& bytes,
             upd::file_kind kind = upd::file_kind::windows_installer,
             const std::string& name = "")
{
    release_spec spec = make_release("0.3.0", "stable", bytes);
    spec.files[0].kind = kind;
    if (!name.empty())
        spec.files[0].name = name;
    h.probe->answer = kind;
    h.create();
    h.check_with(feed_body(5, {spec}));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE(require_offer(h).file.has_value());
}

/// A download of `bytes`, delivered in pieces of `piece` bytes, and ended
/// without an error.
void deliver(harness& h, const QByteArray& bytes, int piece = 40000)
{
    for (qsizetype at = 0; at < bytes.size(); at += piece)
        REQUIRE(h.transport.send_chunk(QByteArrayView(bytes).mid(at, piece)));
    h.transport.finish_download(transport_error::none);
}

/// Downloads and checks `bytes`: the state ends `ready`.
void download_ready(harness& h, const QByteArray& bytes)
{
    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::downloading);
    deliver(h, bytes);
    h.pump_download();
    REQUIRE_EQ(h.state().download, download_state::ready);
}

/// The path of the one folder a download made under the cache.
QString the_pending_folder(const harness& h)
{
    const QStringList names = h.cache_entries();
    REQUIRE_EQ(names.size(), 1);
    REQUIRE(is_random_name(names.front()));
    return h.cache_dir.path() + QStringLiteral("/updates/") + names.front();
}

void require_failed(const harness& h, download_failure why)
{
    REQUIRE_EQ(h.state().download, download_state::failed);
    REQUIRE_EQ(h.state().download_error, why);
    REQUIRE(h.cache_entries().isEmpty());
    REQUIRE(h.downloads_entries().isEmpty());
    REQUIRE(h.service->ready_file() == nullptr);
    REQUIRE(h.state().ready_path.isEmpty());
}

} // namespace

// -- the folders
// -----------------------------------------------------------------

TEST_CASE("update download: start empties what an earlier run left and makes "
          "the folder")
{
    harness h;
    h.create();
    const QString updates = h.cache_dir.path() + QStringLiteral("/updates");
    const QString keep = h.cache_dir.path() + QStringLiteral("/keep.txt");
    write_all(keep, QByteArray("mine"));
    REQUIRE(QDir().mkpath(updates + QStringLiteral("/earlier/deeper")));
    write_all(updates + QStringLiteral("/earlier/deeper/a.exe"),
              QByteArray("x"));
    write_all(updates + QStringLiteral("/loose.zip"), QByteArray("y"));
    write_all(updates + QStringLiteral("/.hidden"), QByteArray("z"));

    h.service->start();
    REQUIRE(QFileInfo(updates).isDir());
    REQUIRE(h.cache_entries().isEmpty());
    // What is outside that folder is not touched.
    REQUIRE_EQ(read_all(keep), QByteArray("mine"));
    REQUIRE_EQ(h.transport.feed_requests, 0);
}

TEST_CASE("update download: start makes the folder when it is missing")
{
    harness h;
    h.create();
    REQUIRE(
        !QFileInfo::exists(h.cache_dir.path() + QStringLiteral("/updates")));
    h.service->start();
    REQUIRE(QFileInfo(h.cache_dir.path() + QStringLiteral("/updates")).isDir());
}

TEST_CASE("update download: with no cache folder start does nothing and a "
          "download cannot be saved")
{
    harness h;
    h.no_cache_folder = true;
    prepare(h, payload_bytes(1000));
    h.service->start();
    REQUIRE(h.cache_entries().isEmpty());
    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::failed);
    REQUIRE_EQ(h.state().download_error, download_failure::save_failed);
    REQUIRE_EQ(h.transport.download_requests, 0);
}

// -- a good download
// -----------------------------------------------------------------

TEST_CASE("update download: a good download is written, checked and held")
{
    harness h;
    const QByteArray bytes = payload_bytes(200000);
    prepare(h, bytes);

    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::downloading);
    REQUIRE_EQ(h.state().total, std::uint64_t{200000});
    REQUIRE_EQ(h.state().received, std::uint64_t{0});
    REQUIRE_EQ(h.transport.download_requests, 1);
    REQUIRE_EQ(h.transport.size_limit, std::uint64_t{200000});
    REQUIRE_EQ(h.transport.download_url,
               QUrl(QStringLiteral(
                   "https://example.org/dl/studio-0.3.0-windows-x86_64.exe")));
    // The bytes go to a folder of their own, under the feed's file name.
    const QString folder = the_pending_folder(h);
    const QString path =
        folder + QStringLiteral("/studio-0.3.0-windows-x86_64.exe");
    REQUIRE(QFileInfo(path).isFile());

    int progress = h.progress;
    std::uint64_t seen = 0;
    for (const int piece : {60000, 70000, 70000})
    {
        REQUIRE(h.transport.send_chunk(
            QByteArrayView(bytes).mid(static_cast<qsizetype>(seen), piece)));
        seen += static_cast<std::uint64_t>(piece);
        ++progress;
        REQUIRE_EQ(h.progress, progress);
        REQUIRE_EQ(h.state().received, seen);
        REQUIRE_EQ(h.state().total, std::uint64_t{200000});
        REQUIRE_EQ(h.state().download, download_state::downloading);
    }

    h.transport.finish_download(transport_error::none);
    REQUIRE_EQ(h.state().download, download_state::verifying);
    // Nothing is read until the event loop turns.
    REQUIRE_EQ(h.last_file->reads, 0);
    REQUIRE(h.service->ready_file() == nullptr);

    h.pump_download();
    REQUIRE_EQ(h.state().download, download_state::ready);
    REQUIRE_EQ(h.state().download_error, download_failure::none);
    REQUIRE(h.service->ready_file() != nullptr);
    REQUIRE_EQ(h.service->ready_file()->path(), path);
    REQUIRE_EQ(h.state().ready_path, path);
    REQUIRE(!h.last_file->released);
    REQUIRE_EQ(read_all(path), bytes);
    REQUIRE(h.downloads_entries().isEmpty());
    // 200000 bytes in pieces of at most 64 KiB: four pieces and the end.
    REQUIRE_EQ(h.last_file->reads, 5);
    REQUIRE(h.last_file->largest_read <= qint64{64} * 1024);
}

TEST_CASE("update download: the file is read one piece for each turn of the "
          "event loop")
{
    harness h;
    prepare(h, payload_bytes(300000));
    h.service->start_download();
    deliver(h, payload_bytes(300000));
    REQUIRE_EQ(h.state().download, download_state::verifying);

    int turns = 0;
    int reads = 0;
    while (h.state().download == download_state::verifying && turns < 100)
    {
        QCoreApplication::processEvents();
        ++turns;
        // No turn reads two pieces.
        REQUIRE(h.last_file->reads - reads <= 1);
        reads = h.last_file->reads;
    }
    REQUIRE_EQ(h.state().download, download_state::ready);
    REQUIRE(turns >= 5);
}

TEST_CASE("update download: it does nothing without an offer that has a file")
{
    {
        // Never checked.
        harness h;
        h.create();
        h.service->start_download();
        REQUIRE_EQ(h.state().download, download_state::none);
        REQUIRE_EQ(h.transport.download_requests, 0);
    }
    {
        // Nothing newer.
        harness h;
        h.create();
        h.check_with(feed_body(5, {make_release("0.2.0")}));
        h.service->start_download();
        REQUIRE_EQ(h.state().download, download_state::none);
    }
    {
        // A release with no file for this kind of install.
        harness h;
        h.probe->answer = upd::file_kind::linux_rpm;
        h.create();
        h.check_with(feed_body(5, {make_release("0.3.0")}));
        REQUIRE_EQ(h.state().state, check_state::available);
        h.service->start_download();
        REQUIRE_EQ(h.state().download, download_state::none);
        REQUIRE_EQ(h.transport.download_requests, 0);
    }
}

TEST_CASE("update download: a second start while it runs does nothing")
{
    harness h;
    prepare(h, payload_bytes(1000));
    h.service->start_download();
    h.service->start_download();
    REQUIRE_EQ(h.transport.download_requests, 1);
    REQUIRE_EQ(h.cache_entries().size(), 1);
}

TEST_CASE("update download: starting again when it is ready keeps the file")
{
    harness h;
    prepare(h, payload_bytes(1000));
    download_ready(h, payload_bytes(1000));
    const checked_file* held = h.service->ready_file();
    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::ready);
    REQUIRE(h.service->ready_file() == held);
    REQUIRE_EQ(h.transport.download_requests, 1);
}

TEST_CASE("update download: a check by hand does nothing while the file is "
          "downloaded")
{
    harness h;
    prepare(h, payload_bytes(1000));
    h.service->start_download();
    h.service->check_now();
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(h.state().state, check_state::available);
}

// -- a download that is not the file
// ---------------------------------------------------------

TEST_CASE("update download: a wrong hash is a mismatch and the folder is "
          "gone")
{
    harness h;
    release_spec spec = make_release("0.3.0", "stable", payload_bytes(5000));
    spec.files[0].sha512_hex = hex_of(payload_bytes(5000, 'q'));
    h.create();
    h.check_with(feed_body(5, {spec}));
    h.service->start_download();
    deliver(h, payload_bytes(5000));
    h.pump_download();
    require_failed(h, download_failure::mismatch);
    REQUIRE(h.last_file->released);
}

TEST_CASE("update download: a short body is a mismatch")
{
    harness h;
    prepare(h, payload_bytes(5000));
    h.service->start_download();
    REQUIRE(h.transport.send_chunk(QByteArrayView(payload_bytes(4999))));
    h.transport.finish_download(transport_error::none);
    // It is refused at once and nothing is opened for checking.
    require_failed(h, download_failure::mismatch);
    REQUIRE(h.last_file == nullptr);
}

TEST_CASE("update download: a body that is too large is a mismatch")
{
    harness h;
    prepare(h, payload_bytes(5000));
    h.service->start_download();
    REQUIRE(h.transport.send_chunk(QByteArrayView(payload_bytes(3000))));
    h.transport.finish_download(transport_error::too_large);
    require_failed(h, download_failure::mismatch);
}

TEST_CASE("update download: a file larger than the feed says is a mismatch")
{
    // The transport allowed more than the feed's size, or the file grew.
    harness h;
    release_spec spec = make_release("0.3.0", "stable", payload_bytes(5000));
    spec.files[0].size = 4000;
    h.create();
    h.check_with(feed_body(5, {spec}));
    h.service->start_download();
    deliver(h, payload_bytes(5000));
    require_failed(h, download_failure::mismatch);
}

TEST_CASE("update download: a transport error is unreachable and the folder "
          "is gone")
{
    for (const transport_error error :
         {transport_error::network, transport_error::tls,
          transport_error::timeout, transport_error::redirected})
    {
        harness h;
        prepare(h, payload_bytes(5000));
        h.service->start_download();
        REQUIRE(h.transport.send_chunk(QByteArrayView(payload_bytes(2000))));
        h.transport.finish_download(error);
        require_failed(h, download_failure::unreachable);
    }
}

TEST_CASE("update download: a failed download can be started again")
{
    harness h;
    prepare(h, payload_bytes(2000));
    h.service->start_download();
    h.transport.finish_download(transport_error::network);
    REQUIRE_EQ(h.state().download, download_state::failed);

    h.service->clear_download_failure();
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE_EQ(h.state().download_error, download_failure::none);

    download_ready(h, payload_bytes(2000));
    REQUIRE_EQ(h.transport.download_requests, 2);

    // Also without clearing the failure first.
    harness again;
    prepare(again, payload_bytes(2000));
    again.service->start_download();
    again.transport.finish_download(transport_error::timeout);
    download_ready(again, payload_bytes(2000));
}

// -- the disk
// ----------------------------------------------------------------------------------

TEST_CASE("update download: a write that fails stops the transport and is "
          "save_failed")
{
    harness h;
    h.use_failing_file = true;
    h.failing_limit = 5000;
    prepare(h, payload_bytes(20000));
    h.service->start_download();
    REQUIRE(h.transport.send_chunk(QByteArrayView(payload_bytes(4000))));
    REQUIRE(!h.transport.send_chunk(QByteArrayView(payload_bytes(4000))));
    REQUIRE(h.transport.download_stopped());
    // The transport reports the stop, and then the failure shows.
    h.transport.finish_download(transport_error::cancelled);
    require_failed(h, download_failure::save_failed);
}

TEST_CASE("update download: a folder that cannot be written is save_failed")
{
    harness h;
    prepare(h, payload_bytes(1000));
    // `<cache>/updates` is a file, so no folder can be made in it.
    const QString updates = h.cache_dir.path() + QStringLiteral("/updates");
    write_all(updates, QByteArray("not a folder"));
    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::failed);
    REQUIRE_EQ(h.state().download_error, download_failure::save_failed);
    REQUIRE_EQ(h.transport.download_requests, 0);
    REQUIRE(h.downloads_entries().isEmpty());
}

TEST_CASE("update download: a file that cannot be opened for checking is "
          "save_failed")
{
    harness h;
    h.open_returns_null = true;
    prepare(h, payload_bytes(3000));
    h.service->start_download();
    deliver(h, payload_bytes(3000));
    require_failed(h, download_failure::save_failed);
}

TEST_CASE("update download: a read that fails is save_failed")
{
    harness h;
    h.next_fail_at_read = 1;
    prepare(h, payload_bytes(200000));
    h.service->start_download();
    deliver(h, payload_bytes(200000));
    h.pump_download();
    require_failed(h, download_failure::save_failed);
    REQUIRE(h.last_file->released);
}

// -- cancel
// -------------------------------------------------------------------------------------

TEST_CASE("update download: cancel during the download deletes what arrived")
{
    harness h;
    prepare(h, payload_bytes(50000));
    h.service->start_download();
    REQUIRE(h.transport.send_chunk(QByteArrayView(payload_bytes(20000))));
    REQUIRE_EQ(h.cache_entries().size(), 1);

    h.service->cancel_download();
    REQUIRE_EQ(h.transport.cancels, 1);
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE_EQ(h.state().received, std::uint64_t{0});
    REQUIRE(h.cache_entries().isEmpty());

    // The transport's answer to the cancel changes nothing.
    const int changes = h.changes;
    h.transport.deliver_cancelled();
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE_EQ(h.changes, changes);
    REQUIRE(h.downloads_entries().isEmpty());
}

TEST_CASE("update download: a chunk that arrives after a cancel is refused")
{
    harness h;
    prepare(h, payload_bytes(50000));
    h.service->start_download();
    h.service->cancel_download();
    REQUIRE(!h.transport.send_chunk(QByteArrayView(payload_bytes(100))));
    REQUIRE_EQ(h.state().received, std::uint64_t{0});
}

TEST_CASE("update download: cancel while checking stops the hashing")
{
    harness h;
    prepare(h, payload_bytes(300000));
    h.service->start_download();
    deliver(h, payload_bytes(300000));
    REQUIRE_EQ(h.state().download, download_state::verifying);
    QCoreApplication::processEvents();
    const int reads = h.last_file->reads;

    h.service->cancel_download();
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE(h.last_file->released);
    REQUIRE(h.cache_entries().isEmpty());
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents();
    REQUIRE_EQ(h.last_file->reads, reads);
    REQUIRE_EQ(h.state().download, download_state::none);
    // The transport was not asked to stop: it had finished.
    REQUIRE_EQ(h.transport.cancels, 0);
}

TEST_CASE("update download: cancel does nothing when there is no download")
{
    harness h;
    prepare(h, payload_bytes(1000));
    const int changes = h.changes;
    h.service->cancel_download();
    REQUIRE_EQ(h.changes, changes);
    REQUIRE_EQ(h.transport.cancels, 0);
}

TEST_CASE("update download: a download started after a cancel is a new one")
{
    harness h;
    prepare(h, payload_bytes(3000));
    h.service->start_download();
    h.service->cancel_download();
    h.transport.deliver_cancelled();
    download_ready(h, payload_bytes(3000));
    REQUIRE_EQ(h.transport.download_requests, 2);
}

TEST_CASE("update download: the service going while a download runs deletes "
          "the folder")
{
    harness h;
    prepare(h, payload_bytes(5000));
    h.service->start_download();
    REQUIRE(h.transport.send_chunk(QByteArrayView(payload_bytes(2000))));
    h.service.reset();
    REQUIRE_EQ(h.transport.cancels, 1);
    REQUIRE(h.cache_entries().isEmpty());
    // The transport's late answers are not used.
    REQUIRE(!h.transport.send_chunk(QByteArrayView(payload_bytes(100))));
    h.transport.finish_download(transport_error::cancelled);
}

TEST_CASE("update download: the service going while it checks stops, deletes "
          "the folder and reads no more")
{
    harness h;
    prepare(h, payload_bytes(300000));
    h.service->start_download();
    deliver(h, payload_bytes(300000));
    const std::shared_ptr<file_log> log = h.last_file;
    h.service.reset();
    REQUIRE(log->released);
    REQUIRE(h.cache_entries().isEmpty());
    for (int i = 0; i < 10; ++i)
        QCoreApplication::processEvents();
    REQUIRE_EQ(log->reads, 0);
}

// -- archives
// ---------------------------------------------------------------------------------------

TEST_CASE("update download: an archive is moved to the downloads folder and "
          "nothing is held")
{
    harness h;
    const QByteArray bytes = payload_bytes(100000);
    prepare(h, bytes, upd::file_kind::windows_zip,
            "studio-0.3.0-windows-x86_64.zip");
    download_ready(h, bytes);

    const QString expected = h.downloads_dir.path() +
                             QStringLiteral("/studio-0.3.0-windows-x86_64.zip");
    REQUIRE_EQ(h.state().ready_path, expected);
    REQUIRE(h.service->ready_file() == nullptr);
    REQUIRE(h.last_file->released);
    REQUIRE_EQ(read_all(expected), bytes);
    // The pending folder is gone.
    REQUIRE(h.cache_entries().isEmpty());
    REQUIRE_EQ(h.downloads_entries().size(), 1);

    // There is no hand-over for it.
    REQUIRE_EQ(h.service->install_ready_file(), install_start::not_supported);
    REQUIRE(h.installs.empty());
    REQUIRE_EQ(h.quits, 0);
}

TEST_CASE("update download: an archive never replaces a file that is there")
{
    harness h;
    const QByteArray bytes = payload_bytes(3000);
    const QString name = QStringLiteral("studio-0.3.0-windows-x86_64.zip");
    write_all(h.downloads_dir.path() + QLatin1Char('/') + name,
              QByteArray("old one"));
    prepare(h, bytes, upd::file_kind::windows_zip, name.toStdString());
    download_ready(h, bytes);

    REQUIRE_EQ(h.state().ready_path,
               h.downloads_dir.path() +
                   QStringLiteral("/studio-0.3.0-windows-x86_64 (2).zip"));
    REQUIRE_EQ(read_all(h.downloads_dir.path() + QLatin1Char('/') + name),
               QByteArray("old one"));
    REQUIRE_EQ(read_all(h.state().ready_path), bytes);

    // And a third time: " (3)".
    h.service->cancel_download();
    h.service->start_download();
    deliver(h, bytes);
    h.pump_download();
    REQUIRE_EQ(h.state().ready_path,
               h.downloads_dir.path() +
                   QStringLiteral("/studio-0.3.0-windows-x86_64 (3).zip"));
    REQUIRE_EQ(h.downloads_entries().size(), 3);
}

TEST_CASE("update download: the number goes before a known suffix, or at the "
          "end")
{
    struct sample
    {
        upd::file_kind kind = upd::file_kind::linux_archive;
        const char* name = "";
        const char* second = "";
    };
    const std::vector<sample> samples{
        {upd::file_kind::linux_archive, "studio-0.3.0.tar.gz",
         "studio-0.3.0 (2).tar.gz"},
        {upd::file_kind::linux_archive, "studio-0.3.0.tar.xz",
         "studio-0.3.0 (2).tar.xz"},
        {upd::file_kind::linux_archive, "studio-0.3.0.tgz",
         "studio-0.3.0 (2).tgz"},
        {upd::file_kind::macos_archive, "Studio-0.3.0.ZIP",
         "Studio-0.3.0 (2).ZIP"},
        {upd::file_kind::linux_archive, "studio-0.3.0.pkg",
         "studio-0.3.0.pkg (2)"}};
    for (const sample& entry : samples)
    {
        harness h;
        const QByteArray bytes = payload_bytes(500);
        write_all(h.downloads_dir.path() + QLatin1Char('/') +
                      QString::fromLatin1(entry.name),
                  QByteArray("old"));
        prepare(h, bytes, entry.kind, entry.name);
        download_ready(h, bytes);
        REQUIRE_EQ(h.state().ready_path, h.downloads_dir.path() +
                                             QLatin1Char('/') +
                                             QString::fromLatin1(entry.second));
    }
}

TEST_CASE("update download: an archive that cannot be moved is save_failed "
          "and nothing is left")
{
    harness h;
    const QByteArray bytes = payload_bytes(500);
    // The downloads folder is a file, so nothing can be put in it.
    const QString blocked = h.downloads_dir.path() + QStringLiteral("/blocked");
    write_all(blocked, QByteArray("file"));
    h.downloads_override = blocked;
    prepare(h, bytes, upd::file_kind::linux_archive, "studio-0.3.0.tgz");
    h.service->start_download();
    deliver(h, bytes);
    h.pump_download();
    REQUIRE_EQ(h.state().download, download_state::failed);
    REQUIRE_EQ(h.state().download_error, download_failure::save_failed);
    REQUIRE(h.cache_entries().isEmpty());
    REQUIRE_EQ(h.downloads_entries().size(), 1);
    REQUIRE(h.last_file->released);
}

// -- an earlier offer
// ---------------------------------------------------------------------------------

TEST_CASE("update download: a different offer replaces a file that is ready")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));
    const std::shared_ptr<file_log> log = h.last_file;
    REQUIRE(!log->released);

    h.check_with(feed_body(
        6, {make_release("0.4.0", "stable", payload_bytes(2000, 'k'))}));
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.4.0"));
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE(h.service->ready_file() == nullptr);
    REQUIRE(h.state().ready_path.isEmpty());
    REQUIRE(log->released);
    REQUIRE(h.cache_entries().isEmpty());
}

TEST_CASE("update download: the same version with another hash replaces a "
          "file that is ready")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));
    h.check_with(feed_body(
        6, {make_release("0.3.0", "stable", payload_bytes(2000, 'k'))}));
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE(h.cache_entries().isEmpty());
}

TEST_CASE("update download: the same offer keeps the file that is ready")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));
    const checked_file* held = h.service->ready_file();
    const std::shared_ptr<file_log> log = h.last_file;

    h.check_with(
        feed_body(6, {make_release("0.3.0", "stable", payload_bytes(2000))}));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE_EQ(h.state().download, download_state::ready);
    REQUIRE(h.service->ready_file() == held);
    REQUIRE(!log->released);
    REQUIRE_EQ(h.cache_entries().size(), 1);
}

TEST_CASE("update download: an offer that has gone replaces a file that is "
          "ready")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));
    // The feed now says nothing is newer than the running version.
    h.check_with(feed_body(6, {make_release("0.2.0")}));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE_EQ(h.state().download, download_state::none);
}

TEST_CASE("update download: a check that fails leaves the file that is ready")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));
    h.check_failing(transport_error::network);
    REQUIRE_EQ(h.state().download, download_state::ready);
    REQUIRE(h.service->ready_file() != nullptr);
}

// -- the hand-over
// ------------------------------------------------------------------------------------------

TEST_CASE("update download: the install is started with the kind and the "
          "file, and a Windows installer asks Studio to close")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));

    REQUIRE_EQ(h.service->install_ready_file(), install_start::started);
    REQUIRE_EQ(h.installs.size(), std::size_t{1});
    REQUIRE_EQ(h.installs[0].kind, upd::file_kind::windows_installer);
    REQUIRE(h.installs[0].file == h.service->ready_file());
    REQUIRE_EQ(h.quits, 1);
    // The file stays held while the installer runs.
    REQUIRE_EQ(h.state().download, download_state::ready);
    REQUIRE(h.service->ready_file() != nullptr);
    REQUIRE(!h.last_file->released);
}

TEST_CASE("update download: an install that did not start does not close "
          "Studio and keeps the file")
{
    for (const install_start answer :
         {install_start::declined, install_start::failed,
          install_start::not_supported})
    {
        harness h;
        h.install_answer = answer;
        prepare(h, payload_bytes(2000));
        download_ready(h, payload_bytes(2000));
        REQUIRE_EQ(h.service->install_ready_file(), answer);
        REQUIRE_EQ(h.quits, 0);
        REQUIRE_EQ(h.state().download, download_state::ready);
        REQUIRE(h.service->ready_file() != nullptr);
        REQUIRE(!h.last_file->released);

        // The person may try again.
        h.install_answer = install_start::started;
        REQUIRE_EQ(h.service->install_ready_file(), install_start::started);
        REQUIRE_EQ(h.installs.size(), std::size_t{2});
        REQUIRE_EQ(h.quits, 1);
    }
}

TEST_CASE("update download: only a Windows installer asks Studio to close")
{
    for (const upd::file_kind kind :
         {upd::file_kind::macos_dmg, upd::file_kind::linux_deb,
          upd::file_kind::linux_rpm})
    {
        harness h;
        prepare(h, payload_bytes(2000), kind, "studio-0.3.0-package");
        download_ready(h, payload_bytes(2000));
        REQUIRE_EQ(h.service->install_ready_file(), install_start::started);
        REQUIRE_EQ(h.installs.size(), std::size_t{1});
        REQUIRE_EQ(h.installs[0].kind, kind);
        REQUIRE_EQ(h.quits, 0);
    }
}

TEST_CASE("update download: nothing is installed unless a file is ready")
{
    harness h;
    prepare(h, payload_bytes(2000));
    REQUIRE_EQ(h.service->install_ready_file(), install_start::failed);
    h.service->start_download();
    REQUIRE_EQ(h.service->install_ready_file(), install_start::failed);
    deliver(h, payload_bytes(2000));
    REQUIRE_EQ(h.service->install_ready_file(), install_start::failed);
    REQUIRE(h.installs.empty());
    REQUIRE_EQ(h.quits, 0);
}

TEST_CASE("update download: the service being destroyed leaves a ready file "
          "for the next start to delete")
{
    harness h;
    prepare(h, payload_bytes(2000));
    download_ready(h, payload_bytes(2000));
    const std::shared_ptr<file_log> log = h.last_file;
    h.service.reset();
    // The file is let go of, and not deleted: the installer may run from it.
    REQUIRE(log->released);
    REQUIRE_EQ(h.cache_entries().size(), 1);
    h.create();
    h.service->start();
    REQUIRE(h.cache_entries().isEmpty());
}
