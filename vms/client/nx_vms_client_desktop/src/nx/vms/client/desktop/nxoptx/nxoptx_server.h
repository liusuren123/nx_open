// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtNetwork/QLocalServer>

class QLocalSocket;

namespace nx::vms::client::desktop {

class PlaylistManager;
class LocalSettings;
class StreamCache;

/**
 * Serves the nxoptx protocol (see nxoptx_protocol.h) on a local socket while the client is running,
 * so that an external program can inspect and fill the local playlists.
 *
 * The server also registers this executable as the nxoptx:// URI handler, which turns a command line
 * such as `nxoptx://addStream?url=rtsp%3A%2F%2Fhost%2Fstream&name=Front%20door` into a command that
 * the shell can deliver to the running client.
 *
 * The local socket is only reachable by the user who runs the client: on Windows a named pipe gets
 * the default security descriptor of its creating process.
 */
class NX_VMS_CLIENT_DESKTOP_API NxOptxServer: public QObject
{
    Q_OBJECT

public:
    explicit NxOptxServer(PlaylistManager* playlists, LocalSettings* settings,
        StreamCache* streamCache, QObject* parent = nullptr);
    virtual ~NxOptxServer() override;

    /** Whether the local socket could be opened. Another running client may hold it. */
    bool isListening() const;

    /** Reason the local socket is not available; empty while it is listening. */
    QString errorString() const;

private:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();

    QLocalServer m_server;
    PlaylistManager* const m_playlists;
    LocalSettings* const m_settings;
    StreamCache* const m_streamCache;

    /** Partially received requests, one buffer per connection. */
    QHash<QLocalSocket*, QByteArray> m_buffers;
};

} // namespace nx::vms::client::desktop
