//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A transport that makes no request. It records what the service asked for
// and the test delivers the reply when it likes, so a test can also leave a
// request outstanding and destroy the service.
//

#pragma once

#include "updates/feed_transport.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <utility>

namespace vss::gui::updates::testing
{

class fake_transport final : public feed_transport
{
public:
    void
    fetch_feed(std::function<void(transport_error, QByteArray)> done) override
    {
        ++feed_requests;
        feed_done_ = std::move(done);
    }

    void start_download(const QUrl& url, std::uint64_t size_limit_bytes,
                        std::function<bool(QByteArrayView)> chunk,
                        std::function<void(transport_error)> done) override
    {
        ++download_requests;
        download_url = url;
        size_limit = size_limit_bytes;
        chunk_ = std::move(chunk);
        download_done_ = std::move(done);
        download_stopped_ = false;
    }

    void cancel() override { ++cancels; }

    // -- what the test reads --------------------------------------------------

    int feed_requests = 0;
    int download_requests = 0;
    int cancels = 0;
    QUrl download_url;
    std::uint64_t size_limit = 0;

    bool feed_pending() const { return static_cast<bool>(feed_done_); }
    bool download_pending() const { return static_cast<bool>(download_done_); }
    /// The service returned false from `chunk`.
    bool download_stopped() const { return download_stopped_; }

    // -- what the test does ---------------------------------------------------

    /// Answers the request of the feed. The callback is called from here, as
    /// it is from an event of the real transport.
    void reply_feed(transport_error error, QByteArray body = {})
    {
        auto done = std::move(feed_done_);
        feed_done_ = nullptr;
        done(error, std::move(body));
    }

    /// Delivers bytes of the download. False when the service stopped it.
    bool send_chunk(QByteArrayView data)
    {
        if (!chunk_ || download_stopped_)
            return false;
        const bool go_on = chunk_(data);
        if (!go_on)
            download_stopped_ = true;
        return go_on;
    }

    /// Ends the download. `cancelled` is what the real transport says after
    /// a chunk returned false.
    void finish_download(transport_error error)
    {
        auto done = std::move(download_done_);
        download_done_ = nullptr;
        chunk_ = nullptr;
        done(error);
    }

    /// Delivers the `cancelled` that follows a call of `cancel`, after the
    /// call has returned.
    void deliver_cancelled()
    {
        if (download_done_)
            finish_download(transport_error::cancelled);
        else if (feed_done_)
            reply_feed(transport_error::cancelled);
    }

private:
    std::function<void(transport_error, QByteArray)> feed_done_;
    std::function<bool(QByteArrayView)> chunk_;
    std::function<void(transport_error)> download_done_;
    bool download_stopped_ = false;
};

} // namespace vss::gui::updates::testing
