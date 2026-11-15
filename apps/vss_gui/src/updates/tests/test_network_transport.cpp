//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transport against a server on this computer. Nothing here leaves the
// loopback address. What a request carries is checked header by header,
// because "nothing about you or this computer is sent" is a promise the
// program makes to the person (docs/updates.md, section 6.1).
//

#include "test_framework.hpp"

#include "updates/network_transport.hpp"
#include "updates/update_types.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

using namespace vss::gui::updates;

namespace
{

/// Turns the event loop until `done` says yes, for ten seconds at most.
bool wait_until(const std::function<bool()>& done, int limit_ms = 10000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done())
    {
        if (clock.elapsed() > limit_ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

/// A web server for one test. It keeps the head of every request it was
/// sent and answers through `respond`.
class test_server final : public QObject
{
public:
    test_server()
    {
        server_.listen(QHostAddress::LocalHost, 0);
        connect(&server_, &QTcpServer::newConnection, this,
                [this]
                {
                    while (QTcpSocket* socket = server_.nextPendingConnection())
                        accept(socket);
                });
    }

    QUrl url(const QString& path) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                        .arg(server_.serverPort())
                        .arg(path));
    }

    /// Writes a whole reply and closes the connection.
    static void send(QTcpSocket* socket, const QByteArray& status,
                     const QByteArray& body, const QByteArray& headers = {})
    {
        socket->write("HTTP/1.1 " + status +
                      "\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n" + headers + "\r\n" + body);
        socket->disconnectFromHost();
    }

    /// The names of the headers of request `index`, lower case, sorted.
    QList<QByteArray> header_names(qsizetype index) const
    {
        QList<QByteArray> names;
        const QList<QByteArray> lines = heads.at(index).split('\n');
        for (qsizetype line = 1; line < lines.size(); ++line)
        {
            const QByteArray text = lines.at(line).trimmed();
            const qsizetype colon = text.indexOf(':');
            if (colon > 0)
                names.append(text.left(colon).toLower());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    /// The value of header `name` of request `index`, or nothing.
    std::optional<QByteArray> header(qsizetype index,
                                     const QByteArray& name) const
    {
        const QList<QByteArray> lines = heads.at(index).split('\n');
        for (qsizetype line = 1; line < lines.size(); ++line)
        {
            const QByteArray text = lines.at(line).trimmed();
            const qsizetype colon = text.indexOf(':');
            if (colon > 0 && text.left(colon).toLower() == name)
                return text.mid(colon + 1).trimmed();
        }
        return std::nullopt;
    }

    QByteArray request_line(qsizetype index) const
    {
        return heads.at(index).split('\n').first().trimmed();
    }

    QList<QByteArray> heads;
    std::function<void(QTcpSocket*, const QByteArray&)> respond;

private:
    void accept(QTcpSocket* socket)
    {
        socket->setParent(this);
        const auto head = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket, head]
                {
                    if (head->endsWith("\r\n\r\n"))
                        return;
                    head->append(socket->readAll());
                    if (!head->endsWith("\r\n\r\n"))
                        return;
                    heads.append(*head);
                    if (respond)
                        respond(socket, *head);
                });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
    }

    QTcpServer server_;
};

NetworkTransport::options
options_for(const test_server& server,
            const QString& path = QStringLiteral("/updates/v1/feed.json"))
{
    NetworkTransport::options settings;
    settings.feed_address = server.url(path);
    settings.https_only = false;
    return settings;
}

struct feed_result
{
    bool done = false;
    int calls = 0;
    transport_error error = transport_error::none;
    QByteArray body;
};

std::function<void(transport_error, QByteArray)> into(feed_result& result)
{
    return [&result](transport_error error, const QByteArray& body)
    {
        result.done = true;
        ++result.calls;
        result.error = error;
        result.body = body;
    };
}

struct download_result
{
    bool done = false;
    int calls = 0;
    transport_error error = transport_error::none;
    QByteArray bytes;
    qsizetype largest_piece = 0;
    int pieces = 0;
    /// Stop after this many pieces. Zero means never.
    int stop_after = 0;
    bool piece_after_done = false;
};

std::function<bool(QByteArrayView)> pieces_into(download_result& result)
{
    return [&result](QByteArrayView piece)
    {
        if (result.done)
            result.piece_after_done = true;
        ++result.pieces;
        result.largest_piece = std::max(result.largest_piece, piece.size());
        result.bytes.append(piece.data(), piece.size());
        return result.stop_after == 0 || result.pieces < result.stop_after;
    };
}

std::function<void(transport_error)> into(download_result& result)
{
    return [&result](transport_error error)
    {
        result.done = true;
        ++result.calls;
        result.error = error;
    };
}

/// The names of the headers every request carries, sorted. Each has one
/// value, the same on every computer: the two cache headers ask a proxy for
/// a fresh copy, and the rest are set in `prepare`.
QList<QByteArray> fixed_headers()
{
    return {"accept-encoding",
            "accept-language",
            "cache-control",
            "connection",
            "host",
            "pragma",
            "user-agent"};
}

/// Bytes that differ along their length, so a piece out of order shows.
QByteArray patterned(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype index = 0; index < size; ++index)
        bytes[index] = static_cast<char>((index * 31 + index / 251) % 251);
    return bytes;
}

} // namespace

TEST_CASE("update transport: a feed request carries a fixed set of headers "
          "and nothing about the computer")
{
    test_server server;
    server.respond = [](QTcpSocket* socket, const QByteArray&)
    { test_server::send(socket, "200 OK", "the feed"); };
    NetworkTransport transport(options_for(server), nullptr);

    feed_result result;
    transport.fetch_feed(into(result));
    // Never from inside the call.
    REQUIRE(!result.done);
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::none);
    REQUIRE(result.body == QByteArray("the feed"));
    REQUIRE_EQ(result.calls, 1);

    REQUIRE_EQ(server.heads.size(), qsizetype{1});
    REQUIRE(server.request_line(0) ==
            QByteArray("GET /updates/v1/feed.json HTTP/1.1"));
    REQUIRE(server.header_names(0) == fixed_headers());
    REQUIRE(server.header(0, "user-agent") ==
            std::optional<QByteArray>(QByteArray(kUserAgent)));
    REQUIRE(server.header(0, "accept-language") ==
            std::optional<QByteArray>(QByteArray("*")));
    REQUIRE(server.header(0, "accept-encoding") ==
            std::optional<QByteArray>(QByteArray("identity")));
    REQUIRE(server.header(0, "cache-control") ==
            std::optional<QByteArray>(QByteArray("no-cache")));
    REQUIRE(server.header(0, "pragma") ==
            std::optional<QByteArray>(QByteArray("no-cache")));
    REQUIRE(server.header(0, "connection") ==
            std::optional<QByteArray>(QByteArray("Keep-Alive")));
}

TEST_CASE("update transport: a cookie the server sets is never sent back")
{
    test_server server;
    server.respond = [](QTcpSocket* socket, const QByteArray&)
    {
        test_server::send(socket, "200 OK", "x",
                          "Set-Cookie: id=12345; Path=/\r\n");
    };
    NetworkTransport transport(options_for(server), nullptr);

    for (int round = 0; round < 2; ++round)
    {
        feed_result result;
        transport.fetch_feed(into(result));
        REQUIRE(wait_until([&] { return result.done; }));
        REQUIRE(result.error == transport_error::none);
    }
    REQUIRE_EQ(server.heads.size(), qsizetype{2});
    REQUIRE(!server.header(1, "cookie").has_value());
}

TEST_CASE("update transport: the feed follows no redirect")
{
    test_server server;
    server.respond = [&server](QTcpSocket* socket, const QByteArray&)
    {
        test_server::send(
            socket, "302 Found", "",
            "Location: " +
                server.url(QStringLiteral("/elsewhere")).toEncoded() + "\r\n");
    };
    NetworkTransport transport(options_for(server), nullptr);

    feed_result result;
    transport.fetch_feed(into(result));
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::redirected);
    REQUIRE(result.body.isEmpty());
    // The other address was never asked.
    REQUIRE_EQ(server.heads.size(), qsizetype{1});
}

TEST_CASE("update transport: a status other than 200 is a failed feed")
{
    for (const QByteArray& status :
         {QByteArray("404 Not Found"), QByteArray("500 Server Error"),
          QByteArray("204 No Content")})
    {
        test_server server;
        server.respond = [status](QTcpSocket* socket, const QByteArray&)
        { test_server::send(socket, status, ""); };
        NetworkTransport transport(options_for(server), nullptr);

        feed_result result;
        transport.fetch_feed(into(result));
        REQUIRE(wait_until([&] { return result.done; }));
        REQUIRE(result.error == transport_error::network);
        REQUIRE(result.body.isEmpty());
    }
}

TEST_CASE("update transport: a feed of the limit is read and one byte more "
          "is refused")
{
    for (const qsizetype extra : {qsizetype{0}, qsizetype{1}, qsizetype{70000}})
    {
        const QByteArray body =
            patterned(static_cast<qsizetype>(kMaxFeedBytes) + extra);
        test_server server;
        server.respond = [body](QTcpSocket* socket, const QByteArray&)
        { test_server::send(socket, "200 OK", body); };
        NetworkTransport transport(options_for(server), nullptr);

        feed_result result;
        transport.fetch_feed(into(result));
        REQUIRE(wait_until([&] { return result.done; }));
        if (extra == 0)
        {
            REQUIRE(result.error == transport_error::none);
            REQUIRE(result.body == body);
        }
        else
        {
            REQUIRE(result.error == transport_error::too_large);
            REQUIRE(result.body.isEmpty());
        }
    }
}

TEST_CASE("update transport: a server that does not answer is a time-out")
{
    test_server server; // No `respond`: the request is read and left.
    NetworkTransport::options settings = options_for(server);
    settings.feed_timeout_ms = 300;
    NetworkTransport transport(settings, nullptr);

    feed_result result;
    transport.fetch_feed(into(result));
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::timeout);
}

TEST_CASE("update transport: nobody listening is a network failure")
{
    NetworkTransport::options settings;
    {
        // A port that was free a moment ago and has no listener now.
        const test_server gone;
        settings = options_for(gone);
    }
    NetworkTransport transport(settings, nullptr);

    feed_result result;
    transport.fetch_feed(into(result));
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::network);
}

TEST_CASE("update transport: cancel ends a feed request, after it returned")
{
    test_server server;
    NetworkTransport transport(options_for(server), nullptr);

    feed_result result;
    transport.fetch_feed(into(result));
    REQUIRE(wait_until([&] { return server.heads.size() == 1; }));
    transport.cancel();
    REQUIRE(!result.done);
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::cancelled);
    REQUIRE_EQ(result.calls, 1);
}

TEST_CASE("update transport: a transport that is destroyed calls nothing")
{
    test_server server;
    feed_result result;
    {
        NetworkTransport transport(options_for(server), nullptr);
        transport.fetch_feed(into(result));
        REQUIRE(wait_until([&] { return server.heads.size() == 1; }));
    }
    for (int turn = 0; turn < 20; ++turn)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    REQUIRE(!result.done);
}

TEST_CASE("update transport: a transport that made no request starts no "
          "thread")
{
    test_server server;
    NetworkTransport transport(options_for(server), nullptr);
    REQUIRE(!transport.running());
    transport.cancel();
    REQUIRE(!transport.running());

    feed_result result;
    transport.fetch_feed(into(result));
    REQUIRE(transport.running());
    transport.cancel();
    REQUIRE(wait_until([&] { return result.done; }));
}

TEST_CASE("update transport: a download that is not https is refused "
          "unasked")
{
    test_server server;
    NetworkTransport::options settings = options_for(server);
    settings.https_only = true;
    NetworkTransport transport(settings, nullptr);

    download_result result;
    transport.start_download(server.url(QStringLiteral("/file")), 100,
                             pieces_into(result), into(result));
    REQUIRE(!result.done);
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::network);
    REQUIRE_EQ(result.pieces, 0);
    REQUIRE(server.heads.isEmpty());
}

TEST_CASE("update transport: an address with a user name in it is refused")
{
    test_server server;
    NetworkTransport transport(options_for(server), nullptr);

    QUrl url = server.url(QStringLiteral("/file"));
    url.setUserName(QStringLiteral("someone"));
    download_result result;
    transport.start_download(url, 100, pieces_into(result), into(result));
    REQUIRE(wait_until([&] { return result.done; }));
    REQUIRE(result.error == transport_error::network);
    REQUIRE(server.heads.isEmpty());
}

TEST_CASE("update transport: a download arrives whole, in order, in pieces "
          "of at most 64 KiB")
{
    const QByteArray body = patterned(qsizetype{3} * 1024 * 1024 + 123);
    test_server server;
    server.respond = [body](QTcpSocket* socket, const QByteArray&)
    { test_server::send(socket, "200 OK", body); };
    NetworkTransport transport(options_for(server), nullptr);

    download_result result;
    transport.start_download(server.url(QStringLiteral("/file.exe")),
                             static_cast<std::uint64_t>(body.size()),
                             pieces_into(result), into(result));
    REQUIRE(wait_until([&] { return result.done; }, 60000));
    REQUIRE(result.error == transport_error::none);
    REQUIRE_EQ(result.calls, 1);
    REQUIRE(result.bytes == body);
    REQUIRE(result.largest_piece <= qsizetype{64} * 1024);
    REQUIRE(!result.piece_after_done);

    // A download says no more about the computer than a feed request.
    REQUIRE(server.header_names(0) == fixed_headers());
    REQUIRE(server.request_line(0) == QByteArray("GET /file.exe HTTP/1.1"));
}

TEST_CASE("update transport: more bytes than the feed promised stop the "
          "download")
{
    const QByteArray body = patterned(qsizetype{400} * 1024);
    test_server server;
    server.respond = [body](QTcpSocket* socket, const QByteArray&)
    { test_server::send(socket, "200 OK", body); };
    NetworkTransport transport(options_for(server), nullptr);

    download_result result;
    transport.start_download(server.url(QStringLiteral("/file")),
                             static_cast<std::uint64_t>(body.size() - 1),
                             pieces_into(result), into(result));
    REQUIRE(wait_until([&] { return result.done; }, 60000));
    REQUIRE(result.error == transport_error::too_large);
    REQUIRE(result.bytes.size() < body.size());
}

TEST_CASE("update transport: a receiver that says stop ends the download "
          "as cancelled")
{
    const QByteArray body = patterned(qsizetype{2} * 1024 * 1024);
    test_server server;
    server.respond = [body](QTcpSocket* socket, const QByteArray&)
    { test_server::send(socket, "200 OK", body); };
    NetworkTransport transport(options_for(server), nullptr);

    download_result result;
    result.stop_after = 2;
    transport.start_download(server.url(QStringLiteral("/file")),
                             static_cast<std::uint64_t>(body.size()),
                             pieces_into(result), into(result));
    REQUIRE(wait_until([&] { return result.done; }, 60000));
    REQUIRE(result.error == transport_error::cancelled);
    REQUIRE_EQ(result.pieces, 2);
    REQUIRE(!result.piece_after_done);
}

TEST_CASE("update transport: cancel ends a download and no piece follows")
{
    const QByteArray body = patterned(qsizetype{2} * 1024 * 1024);
    test_server server;
    // The server sends half of the file and keeps the connection open, so
    // the download cannot end by itself before it is cancelled.
    server.respond = [body](QTcpSocket* socket, const QByteArray&)
    {
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                      QByteArray::number(body.size()) +
                      "\r\nConnection: close\r\n\r\n" +
                      body.left(body.size() / 2));
    };
    NetworkTransport transport(options_for(server), nullptr);

    download_result result;
    transport.start_download(server.url(QStringLiteral("/file")),
                             static_cast<std::uint64_t>(body.size()),
                             pieces_into(result), into(result));
    REQUIRE(wait_until([&] { return result.pieces > 0; }, 60000));
    const int pieces_before = result.pieces;
    transport.cancel();
    REQUIRE(!result.done);
    REQUIRE(wait_until([&] { return result.done; }, 60000));
    REQUIRE(result.error == transport_error::cancelled);
    REQUIRE_EQ(result.pieces, pieces_before);
}

TEST_CASE("update transport: a download follows a redirect and a missing "
          "file is a failure")
{
    const QByteArray body = patterned(5000);
    test_server server;
    server.respond = [&server, body](QTcpSocket* socket, const QByteArray& head)
    {
        if (head.startsWith("GET /first "))
            test_server::send(
                socket, "302 Found", "",
                "Location: " +
                    server.url(QStringLiteral("/second")).toEncoded() + "\r\n");
        else if (head.startsWith("GET /second "))
            test_server::send(socket, "200 OK", body);
        else
            test_server::send(socket, "404 Not Found", "no such file");
    };
    NetworkTransport transport(options_for(server), nullptr);

    download_result followed;
    transport.start_download(server.url(QStringLiteral("/first")),
                             static_cast<std::uint64_t>(body.size()),
                             pieces_into(followed), into(followed));
    REQUIRE(wait_until([&] { return followed.done; }));
    REQUIRE(followed.error == transport_error::none);
    REQUIRE(followed.bytes == body);

    download_result missing;
    transport.start_download(server.url(QStringLiteral("/gone")), 5000,
                             pieces_into(missing), into(missing));
    REQUIRE(wait_until([&] { return missing.done; }));
    REQUIRE(missing.error == transport_error::network);
    REQUIRE_EQ(missing.pieces, 0);
}

TEST_CASE("update transport: the update server can be reached over TLS "
          "(runs only when asked)")
{
    // Every other test stays on this computer. This one makes the request
    // the program makes, to the address the program asks, so it runs only
    // for a person who sets VSS_UPDATE_LIVE_TEST=1. It tells a computer
    // that cannot make the TLS connection from one that can: the answer may
    // be a feed, or "no such file" while the site has none.
    if (qEnvironmentVariable("VSS_UPDATE_LIVE_TEST") != QStringLiteral("1"))
        VSS_SKIP("set VSS_UPDATE_LIVE_TEST=1 to ask the update server");

    NetworkTransport transport;
    feed_result result;
    transport.fetch_feed(into(result));
    REQUIRE(wait_until([&] { return result.done; }, 30000));
    REQUIRE(result.error == transport_error::none ||
            result.error == transport_error::network);
}
