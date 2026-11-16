//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transport that makes the requests (docs/updates.md, section 6.1). This
// and its source file are the only ones in Studio that include
// `QNetworkAccessManager`.
//
// What a request carries is decided here and nowhere else: a `GET`, the
// fixed `User-Agent` and nothing more. No query string, no cookie, no
// stored state, no body. TLS errors end the request; there is no "continue
// anyway". The feed follows no redirect; a download follows redirects to
// `https` addresses only.
//
// The requests run on a thread of their own. Finding the system's proxy and
// loading the certificate store can each take longer than the window may
// be held up, so neither happens on the GUI thread. The thread is started
// by the first request: a Studio that never checks starts no thread, loads
// no TLS code and opens no socket. Replies come back to the thread this
// object lives on through queued calls, in order.
//

#pragma once

#include "updates/feed_transport.hpp"

#include <QObject>
#include <QThread>
#include <QUrl>

#include <cstdint>
#include <functional>

namespace vss::gui::updates
{

class NetworkWorker;

class NetworkTransport final : public QObject, public feed_transport
{
    Q_OBJECT

public:
    /// What a test may change. The program uses the defaults and has no
    /// way to change them.
    struct options
    {
        /// Where the feed is read from: `kFeedAddress`.
        QUrl feed_address;
        /// The time a feed request may take: `kFeedTimeoutMs`.
        int feed_timeout_ms = 0;
        /// A download is refused unless its address is `https`. A test
        /// that serves bytes from this computer turns it off.
        bool https_only = true;
    };

    explicit NetworkTransport(QObject* parent = nullptr);
    NetworkTransport(const options& settings, QObject* parent);
    ~NetworkTransport() override;

    void
    fetch_feed(std::function<void(transport_error, QByteArray)> done) override;
    void start_download(const QUrl& url, std::uint64_t size_limit,
                        std::function<bool(QByteArrayView)> chunk,
                        std::function<void(transport_error)> done) override;
    void cancel() override;

    /// Whether the Qt this program runs with can make a TLS connection.
    /// The self-test prints it, so a package without a TLS backend fails
    /// the release job and not a person's first check. It loads the TLS
    /// backend, so it is not called on an ordinary start.
    static bool tls_available();

    /// Whether the thread that makes the requests has been started.
    bool running() const { return thread_.isRunning(); }

private:
    /// The worker reports through the three functions below.
    friend class NetworkWorker;

    void ensure_worker();
    void feed_done(quint64 id, transport_error error, const QByteArray& body);
    void download_chunk(quint64 id, const QByteArray& bytes);
    void download_done(quint64 id, transport_error error);

    options options_;
    QThread thread_;
    NetworkWorker* worker_ = nullptr;
    quint64 next_id_ = 1;

    quint64 feed_id_ = 0;
    std::function<void(transport_error, QByteArray)> feed_done_;

    quint64 download_id_ = 0;
    bool download_stopped_ = false;
    std::function<bool(QByteArrayView)> download_chunk_;
    std::function<void(transport_error)> download_done_;
};

} // namespace vss::gui::updates
