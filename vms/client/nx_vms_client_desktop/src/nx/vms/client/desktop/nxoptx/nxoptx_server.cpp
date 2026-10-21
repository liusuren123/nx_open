// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "nxoptx_server.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtNetwork/QLocalSocket>

#include <nx/utils/log/log.h>

#include "nxoptx_protocol.h"

namespace nx::vms::client::desktop {

namespace {

const nx::log::Tag kLogTag(QStringLiteral("nxoptx"));

} // namespace

NxOptxServer::NxOptxServer(PlaylistManager* playlists, LocalSettings* settings,
    StreamCache* streamCache, QObject* parent):
    QObject(parent),
    m_playlists(playlists),
    m_settings(settings),
    m_streamCache(streamCache)
{
    // Only the user who runs the client may talk to it. On Windows the named pipe is created with
    // the default security descriptor of this process anyway; the option matters elsewhere.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);

    connect(&m_server, &QLocalServer::newConnection, this, &NxOptxServer::onNewConnection);

    if (m_server.listen(nxoptx::socketName()))
    {
        NX_INFO(kLogTag, "Listening on the local socket '%1'.", nxoptx::socketName());
    }
    else
    {
        // Another running client serves the protocol, or the socket name is taken by something else.
        NX_WARNING(kLogTag, "The local socket '%1' is not available: %2",
            nxoptx::socketName(), m_server.errorString());
    }

    nxoptx::registerUriHandler();
}

NxOptxServer::~NxOptxServer()
{
    // The connections are children of the server, so they are destroyed with it; drop the signal
    // connections and the buffers before that happens.
    m_buffers.clear();

    const auto connections = m_server.findChildren<QLocalSocket*>();
    for (QLocalSocket* socket: connections)
        socket->disconnect(this);
}

bool NxOptxServer::isListening() const
{
    return m_server.isListening();
}

QString NxOptxServer::errorString() const
{
    return m_server.errorString();
}

void NxOptxServer::onNewConnection()
{
    while (QLocalSocket* socket = m_server.nextPendingConnection())
    {
        m_buffers.insert(socket, QByteArray());

        connect(socket, &QLocalSocket::readyRead, this, &NxOptxServer::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &NxOptxServer::onDisconnected);
    }
}

void NxOptxServer::onReadyRead()
{
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket || !m_buffers.contains(socket))
        return;

    QByteArray& buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    // Requests are newline delimited, so that a program can send several of them over one connection
    // and read one response line per request.
    int newline = buffer.indexOf('\n');
    while (newline >= 0)
    {
        const QByteArray line = buffer.left(newline);
        buffer.remove(0, newline + 1);

        QJsonObject response;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            NX_WARNING(kLogTag, "Ignoring a malformed request: %1",
                parseError.errorString());
            response = {
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"), QStringLiteral("invalidJson")},
                {QStringLiteral("message"), parseError.errorString()}};
        }
        else
        {
            response = nxoptx::execute(document.object(), m_playlists, m_settings, m_streamCache);
        }

        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();

        newline = buffer.indexOf('\n');
    }
}

void NxOptxServer::onDisconnected()
{
    if (auto* socket = qobject_cast<QLocalSocket*>(sender()))
    {
        m_buffers.remove(socket);
        socket->deleteLater();
    }
}

} // namespace nx::vms::client::desktop
