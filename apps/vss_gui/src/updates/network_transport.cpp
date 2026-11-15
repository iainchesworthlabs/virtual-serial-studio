//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "updates/network_transport.hpp"

#include "updates/update_types.hpp"

#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QString>
#include <QTimer>

#include <utility>

namespace vss::gui::updates
{
namespace
{

/// A download that receives nothing for this long is given up.
constexpr int kDownloadIdleTimeoutMs = 30 * 1000;
/// The largest piece handed to the caller.
constexpr qint64 kPieceBytes = 64 * 1024;
/// How many pieces may wait for the GUI thread. With the reply's read
/// buffer this bounds the memory a slow receiver can make the download
/// hold, whatever the size of the file.
constexpr int kMaxPendingPieces = 16;
constexpr qint64 kReadBufferBytes = 1024 * 1024;
constexpr int kMaxRedirects = 5;

/// The system's proxy settings, for this manager only. The application's
/// own proxy factory is left alone.
class system_proxy_factory final : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override
    {
        return QNetworkProxyFactory::systemProxyForQuery(query);
    }
};

/// Everything a request of Studio carries beyond the address. Each header
/// has a fixed value: Qt would otherwise send the system's language and the
/// encodings this build of Qt knows, which say something about the
/// computer.
void prepare(QNetworkRequest& request)
{
    request.setRawHeader(QByteArrayLiteral("User-Agent"),
                         QByteArray(kUserAgent));
    request.setRawHeader(QByteArrayLiteral("Accept-Language"),
                         QByteArrayLiteral("*"));
    // With this header set by the caller, Qt changes nothing in the body,
    // so the bytes counted are the bytes sent.
    request.setRawHeader(QByteArrayLiteral("Accept-Encoding"),
                         QByteArrayLiteral("identity"));
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

/// One request in flight and why it was stopped, if it was.
struct flight
{
    quint64 id = 0;
    QNetworkReply* reply = nullptr;
    bool cancelled = false;
    bool timed_out = false;
    bool too_large = false;
    bool tls = false;
    bool redirected = false;
};

transport_error outcome(const flight& request)
{
    if (request.cancelled)
        return transport_error::cancelled;
    if (request.too_large)
        return transport_error::too_large;
    if (request.redirected)
        return transport_error::redirected;
    if (request.timed_out)
        return transport_error::timeout;
    if (request.tls)
        return transport_error::tls;
    switch (request.reply->error())
    {
    case QNetworkReply::NoError:
        break;
    case QNetworkReply::SslHandshakeFailedError:
        return transport_error::tls;
    case QNetworkReply::TimeoutError:
    // The flags above account for every `abort` made here, so what is left
    // is Qt giving up a transfer that stood still.
    case QNetworkReply::OperationCanceledError:
        return transport_error::timeout;
    case QNetworkReply::InsecureRedirectError:
    case QNetworkReply::TooManyRedirectsError:
        return transport_error::redirected;
    default:
        return transport_error::network;
    }
    const int status =
        request.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
            .toInt();
    if (status >= 300 && status < 400)
        return transport_error::redirected;
    return status == 200 ? transport_error::none : transport_error::network;
}

} // namespace

/// Lives on the transport's thread and owns everything of Qt Network.
/// Every function here runs on that thread.
class NetworkWorker final : public QObject
{
public:
    NetworkWorker(NetworkTransport* owner,
                  const NetworkTransport::options& settings)
        : owner_(owner), options_(settings)
    {
    }

    void fetch(quint64 id)
    {
        stop_flight(feed_);
        QNetworkRequest request{options_.feed_address};
        prepare(request);
        // The feed has one address. A reply that points anywhere else is
        // refused, not followed.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply* const reply = manager().get(request);
        feed_ = flight{};
        feed_.id = id;
        feed_.reply = reply;
        body_.clear();
        connect(reply, &QNetworkReply::sslErrors, this,
                [this, reply]
                {
                    if (feed_.reply == reply)
                        feed_.tls = true;
                });
        connect(reply, &QNetworkReply::readyRead, this,
                [this, reply] { read_feed(reply); });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply] { finish_feed(reply); });
        deadline_->start(options_.feed_timeout_ms);
    }

    void download(quint64 id, const QUrl& url, quint64 size_limit)
    {
        stop_flight(download_);
        const bool refused =
            options_.https_only && url.scheme() != QLatin1String("https");
        if (refused || !url.userInfo().isEmpty())
        {
            report_download(id, transport_error::network);
            return;
        }
        QNetworkRequest request{url};
        prepare(request);
        // GitHub serves a release file from a second host. `https` to
        // `https` is followed; anything less safe ends the download.
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setMaximumRedirectsAllowed(kMaxRedirects);
        request.setTransferTimeout(kDownloadIdleTimeoutMs);
        QNetworkReply* const reply = manager().get(request);
        reply->setReadBufferSize(kReadBufferBytes);
        download_ = flight{};
        download_.id = id;
        download_.reply = reply;
        size_limit_ = size_limit;
        received_ = 0;
        pending_ = 0;
        finished_ = false;
        connect(reply, &QNetworkReply::sslErrors, this,
                [this, reply]
                {
                    if (download_.reply == reply)
                        download_.tls = true;
                });
        connect(reply, &QNetworkReply::redirected, this,
                [this, reply](const QUrl& target)
                {
                    if (download_.reply != reply || !options_.https_only ||
                        target.scheme() == QLatin1String("https"))
                        return;
                    download_.redirected = true;
                    reply->abort();
                });
        connect(reply, &QNetworkReply::readyRead, this,
                [this, reply] { pump(reply); });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply]
                {
                    if (download_.reply != reply)
                    {
                        reply->deleteLater();
                        return;
                    }
                    finished_ = true;
                    pump(reply);
                });
    }

    /// The GUI thread took one piece.
    void acknowledge(quint64 id)
    {
        if (download_.id != id || download_.reply == nullptr)
            return;
        --pending_;
        pump(download_.reply);
    }

    /// `cancel`, or the receiver wants no more.
    void stop(quint64 id)
    {
        if (feed_.id == id && feed_.reply != nullptr)
        {
            feed_.cancelled = true;
            feed_.reply->abort();
        }
        if (download_.id == id && download_.reply != nullptr)
        {
            download_.cancelled = true;
            download_.reply->abort();
        }
    }

    /// Ends every request, closes every socket and stops the thread.
    void shutdown()
    {
        owner_ = nullptr;
        stop_flight(feed_);
        stop_flight(download_);
        delete manager_;
        manager_ = nullptr;
        thread()->quit();
    }

private:
    QNetworkAccessManager& manager()
    {
        if (manager_ == nullptr)
        {
            manager_ = new QNetworkAccessManager(this);
            // The manager owns the factory.
            manager_->setProxyFactory(new system_proxy_factory);
            deadline_ = new QTimer(this);
            deadline_->setSingleShot(true);
            connect(deadline_, &QTimer::timeout, this,
                    [this]
                    {
                        if (feed_.reply == nullptr)
                            return;
                        feed_.timed_out = true;
                        feed_.reply->abort();
                    });
        }
        return *manager_;
    }

    /// Drops a request whose answer nobody waits for any more.
    static void stop_flight(flight& request)
    {
        QNetworkReply* const reply = request.reply;
        request = flight{};
        if (reply == nullptr)
            return;
        reply->abort();
        reply->deleteLater();
    }

    void read_feed(QNetworkReply* reply)
    {
        if (feed_.reply != reply)
            return;
        // One byte over the limit is read, to tell a feed of exactly the
        // limit from one that is too large.
        body_.append(reply->read(kMaxFeedBytes + 1 - body_.size()));
        if (body_.size() <= kMaxFeedBytes)
            return;
        feed_.too_large = true;
        reply->abort();
    }

    void finish_feed(QNetworkReply* reply)
    {
        reply->deleteLater();
        if (feed_.reply != reply)
            return;
        deadline_->stop();
        transport_error error = outcome(feed_);
        if (error == transport_error::none)
        {
            read_feed(reply);
            if (feed_.reply != reply)
                return;
            if (body_.size() > kMaxFeedBytes)
                error = transport_error::too_large;
        }
        const quint64 id = feed_.id;
        feed_ = flight{};
        QByteArray body;
        if (error == transport_error::none)
            body = std::exchange(body_, {});
        body_.clear();
        if (NetworkTransport* const owner = owner_)
            QMetaObject::invokeMethod(
                owner, [owner, id, error, body]
                { owner->feed_done(id, error, body); }, Qt::QueuedConnection);
    }

    /// Hands on what has arrived, a bounded number of pieces at a time,
    /// and ends the download when there is nothing left to hand on.
    void pump(QNetworkReply* reply)
    {
        if (download_.reply != reply)
            return;
        const auto stopped = [this]
        {
            return download_.cancelled || download_.too_large ||
                   download_.redirected;
        };
        // Only the bytes of a file are handed on, never the page that
        // comes with a refusal.
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool wanted = status == 200;
        while (wanted && !stopped() && pending_ < kMaxPendingPieces &&
               reply->bytesAvailable() > 0)
        {
            const QByteArray piece = reply->read(kPieceBytes);
            if (piece.isEmpty())
                break;
            received_ += static_cast<quint64>(piece.size());
            if (received_ > size_limit_)
            {
                download_.too_large = true;
                // `abort` reports `finished` before it returns, and that
                // comes back into this function.
                reply->abort();
                break;
            }
            ++pending_;
            const quint64 id = download_.id;
            if (NetworkTransport* const owner = owner_)
                QMetaObject::invokeMethod(
                    owner,
                    [owner, id, piece] { owner->download_chunk(id, piece); },
                    Qt::QueuedConnection);
        }
        if (download_.reply != reply || !finished_)
            return;
        const bool failed =
            stopped() || reply->error() != QNetworkReply::NoError;
        if (!failed && wanted && reply->bytesAvailable() > 0)
            return;
        const transport_error error = outcome(download_);
        const quint64 id = download_.id;
        download_ = flight{};
        reply->deleteLater();
        report_download(id, error);
    }

    void report_download(quint64 id, transport_error error)
    {
        if (NetworkTransport* const owner = owner_)
            QMetaObject::invokeMethod(
                owner, [owner, id, error] { owner->download_done(id, error); },
                Qt::QueuedConnection);
    }

    /// Valid until `shutdown`: the transport waits for this thread to end
    /// before it is gone.
    NetworkTransport* owner_ = nullptr;
    NetworkTransport::options options_;
    QNetworkAccessManager* manager_ = nullptr;
    QTimer* deadline_ = nullptr;

    flight feed_;
    QByteArray body_;

    flight download_;
    quint64 size_limit_ = 0;
    quint64 received_ = 0;
    int pending_ = 0;
    bool finished_ = false;
};

NetworkTransport::NetworkTransport(QObject* parent)
    : NetworkTransport(options{}, parent)
{
}

NetworkTransport::NetworkTransport(const options& settings, QObject* parent)
    : QObject(parent), options_(settings)
{
    if (options_.feed_address.isEmpty())
        options_.feed_address = QUrl(QString::fromLatin1(kFeedAddress));
    if (options_.feed_timeout_ms <= 0)
        options_.feed_timeout_ms = kFeedTimeoutMs;
}

NetworkTransport::~NetworkTransport()
{
    if (worker_ == nullptr)
        return;
    QMetaObject::invokeMethod(
        worker_, [worker = worker_] { worker->shutdown(); },
        Qt::QueuedConnection);
    thread_.wait();
}

void NetworkTransport::ensure_worker()
{
    if (worker_ != nullptr)
        return;
    worker_ = new NetworkWorker(this, options_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_.setObjectName(QStringLiteral("vss-updates"));
    thread_.start();
}

void NetworkTransport::fetch_feed(
    std::function<void(transport_error, QByteArray)> done)
{
    ensure_worker();
    if (feed_id_ != 0 && feed_done_)
        QMetaObject::invokeMethod(
            this, [superseded = std::move(feed_done_)]
            { superseded(transport_error::cancelled, QByteArray()); },
            Qt::QueuedConnection);
    const quint64 id = next_id_++;
    feed_id_ = id;
    feed_done_ = std::move(done);
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, id] { worker->fetch(id); },
        Qt::QueuedConnection);
}

void NetworkTransport::start_download(const QUrl& url, std::uint64_t size_limit,
                                      std::function<bool(QByteArrayView)> chunk,
                                      std::function<void(transport_error)> done)
{
    ensure_worker();
    if (download_id_ != 0 && download_done_)
        QMetaObject::invokeMethod(
            this, [superseded = std::move(download_done_)]
            { superseded(transport_error::cancelled); }, Qt::QueuedConnection);
    const quint64 id = next_id_++;
    download_id_ = id;
    download_stopped_ = false;
    download_chunk_ = std::move(chunk);
    download_done_ = std::move(done);
    const auto limit = static_cast<quint64>(size_limit);
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, id, url, limit]
        { worker->download(id, url, limit); }, Qt::QueuedConnection);
}

void NetworkTransport::cancel()
{
    if (worker_ == nullptr)
        return;
    if (feed_id_ != 0)
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, id = feed_id_] { worker->stop(id); },
            Qt::QueuedConnection);
    if (download_id_ != 0)
    {
        download_stopped_ = true;
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, id = download_id_]
            { worker->stop(id); }, Qt::QueuedConnection);
    }
}

bool NetworkTransport::tls_available()
{
    return QSslSocket::supportsSsl();
}

void NetworkTransport::feed_done(quint64 id, transport_error error,
                                 const QByteArray& body)
{
    if (id != feed_id_)
        return;
    feed_id_ = 0;
    const auto done = std::exchange(feed_done_, nullptr);
    if (done)
        done(error, body);
}

void NetworkTransport::download_chunk(quint64 id, const QByteArray& bytes)
{
    if (id != download_id_ || download_stopped_ || !download_chunk_)
        return;
    if (download_chunk_(QByteArrayView(bytes)))
    {
        QMetaObject::invokeMethod(
            worker_, [worker = worker_, id] { worker->acknowledge(id); },
            Qt::QueuedConnection);
        return;
    }
    download_stopped_ = true;
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, id] { worker->stop(id); },
        Qt::QueuedConnection);
}

void NetworkTransport::download_done(quint64 id, transport_error error)
{
    if (id != download_id_)
        return;
    const bool stopped = download_stopped_;
    download_id_ = 0;
    download_stopped_ = false;
    download_chunk_ = nullptr;
    const auto done = std::exchange(download_done_, nullptr);
    if (done)
        done(stopped ? transport_error::cancelled : error);
}

} // namespace vss::gui::updates
