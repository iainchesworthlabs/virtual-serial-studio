//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the update service reaches the network. The service knows this
// interface only, so every test gives it a fake and no test opens a socket
// (docs/updates.md, section 9). `network_transport.cpp` is the one file
// that makes a request.
//

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QUrl>

#include <cstdint>
#include <functional>

namespace vss::gui::updates
{

enum class transport_error
{
    none,
    /// No route, no name, the connection was refused or dropped, or a status
    /// other than 200.
    network,
    /// The certificate or the TLS session was not accepted. Never ignored.
    tls,
    timeout,
    /// More bytes arrived than the caller allowed.
    too_large,
    /// A redirect that is not followed: any for the feed, and one to an
    /// address that is not `https` for a download.
    redirected,
    /// `cancel` was called.
    cancelled
};

class feed_transport
{
public:
    feed_transport() = default;
    feed_transport(const feed_transport&) = delete;
    feed_transport& operator=(const feed_transport&) = delete;
    virtual ~feed_transport() = default;

    /// One `GET` of the feed address. `done` is called exactly once, on the
    /// thread that called this, and never from inside this call. The body is
    /// empty unless the error is `none`.
    virtual void
    fetch_feed(std::function<void(transport_error, QByteArray)> done) = 0;

    /// Starts a download of `url`, which is an `https` address. Bytes arrive
    /// in order through `chunk`, in pieces of at most 64 KiB; when `chunk`
    /// returns false the download stops and `done` reports `cancelled`.
    /// More than `size_limit` bytes stops it with `too_large`. `done` is
    /// called exactly once, after the last `chunk`, and never from inside
    /// this call.
    virtual void start_download(const QUrl& url, std::uint64_t size_limit,
                                std::function<bool(QByteArrayView)> chunk,
                                std::function<void(transport_error)> done) = 0;

    /// Stops the request in flight, if any. Its `done` is then called with
    /// `cancelled`, after this call has returned.
    virtual void cancel() = 0;
};

} // namespace vss::gui::updates
