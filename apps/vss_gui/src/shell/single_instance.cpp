//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/single_instance.hpp"

#include "support/account_id.hpp"

#include <vss/core/profile.hpp>

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>

namespace vss::gui
{
namespace
{

constexpr int kConnectMs = 500;
constexpr QByteArrayView kShow = "show\n";

} // namespace

SingleInstance::SingleInstance(const QString& name, QObject* parent)
    : QObject(parent), name_(name)
{
}

SingleInstance::~SingleInstance()
{
    close();
}

QString SingleInstance::default_name()
{
    // The account's id keeps two people on one computer apart; the hash keeps
    // the pipe name short and free of characters a pipe name cannot hold.  A
    // user name from the environment would not do: whoever starts the program
    // can change or empty it, and then two accounts ask for the same name.  It
    // is used only where the system cannot be asked for the id.
    QString user = support::account_id();
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty())
        user = qEnvironmentVariable("USER");
    const QByteArray hash =
        QCryptographicHash::hash(user.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16);
    return QStringLiteral("virtual-serial-studio-%1")
        .arg(QString::fromLatin1(hash));
}

bool SingleInstance::activate_running() const
{
    VSS_PROFILE_ZONE("gui.shell.single_instance.activate");
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (!socket.waitForConnected(kConnectMs))
        return false;
    socket.write(kShow.data(), kShow.size());
    socket.waitForBytesWritten(kConnectMs);
    socket.disconnectFromServer();
    return true;
}

bool SingleInstance::answering() const
{
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (!socket.waitForConnected(kConnectMs))
        return false;
    socket.disconnectFromServer();
    return true;
}

bool SingleInstance::listen(int wait_ms)
{
    VSS_PROFILE_ZONE("gui.shell.single_instance.listen");
    if (server_ != nullptr)
        return true;
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    connect(server_, &QLocalServer::newConnection, this,
            &SingleInstance::on_connection);

    // A Studio that is quitting still answers for a moment.  Wait for it to
    // go before taking the name, without asking it to show itself.
    const QDeadlineTimer deadline(wait_ms);
    while (answering() && !deadline.hasExpired())
        QThread::msleep(100);

    if (!answering())
    {
        if (server_->listen(name_))
            return true;
        // A socket file left by a Studio that crashed blocks the name on Unix
        // until it is removed.  Nobody answers on it, so remove it.
        QLocalServer::removeServer(name_);
        if (server_->listen(name_))
            return true;
    }
    delete server_;
    server_ = nullptr;
    return false;
}

void SingleInstance::close()
{
    if (server_ == nullptr)
        return;
    server_->close();
    delete server_;
    server_ = nullptr;
}

void SingleInstance::on_connection()
{
    VSS_PROFILE_ZONE("gui.shell.single_instance.connection");
    while (QLocalSocket* socket = server_->nextPendingConnection())
    {
        connect(socket, &QLocalSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket]
                {
                    if (socket->readAll().startsWith(kShow.data()))
                        Q_EMIT activation_requested();
                });
    }
}

} // namespace vss::gui
