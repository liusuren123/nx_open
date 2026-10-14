// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QVariantList>
#include <QtNetwork/QNetworkAccessManager>

#include <nx/utils/uuid.h>

#include "iptv_playlist.h"

class QNetworkReply;
class QTimer;

namespace nx::vms::client::desktop {

class LocalSettings;

/**
 * Owns the IPTV playlists imported by the user and opens their channels in the current layout.
 *
 * A playlist is imported either from an http(s) URL or from a local .m3u file, then parsed and kept
 * in the local settings. Channels are not turned into resources up front: a resource is created only
 * when a channel is actually opened, so a playlist with thousands of entries stays cheap.
 *
 * Importing a remote playlist is asynchronous: `importing` turns true, the download runs in the
 * background and `importFinished` is emitted when it is done. A playlist of tens of thousands of
 * entries downloads in seconds, so the UI must stay responsive instead of blocking on the request.
 */
class NX_VMS_CLIENT_DESKTOP_API IptvManager: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)

public:
    explicit IptvManager(LocalSettings* settings, QObject* parent = nullptr);
    virtual ~IptvManager() override;

    /** Imported playlists as a list of { id, name, source, channelCount } maps. */
    QVariantList playlists() const;

    /** True while a remote playlist is being downloaded. */
    bool importing() const;

    /** Channels of a playlist as a list of { id, name, group, logo } maps. */
    Q_INVOKABLE QVariantList channels(const QString& playlistId) const;

    /**
     * Starts importing a playlist from an http(s) URL. Returns false if the request could not be
     * started; the download itself is asynchronous and reports through importFinished().
     */
    Q_INVOKABLE bool importFromUrl(const QString& url, const QString& name);

    /** Imports a playlist from a local .m3u/.m3u8 file. */
    Q_INVOKABLE bool importFromFile(const QString& path, const QString& name);

    /** Re-downloads or re-reads a previously imported playlist. */
    Q_INVOKABLE bool reloadPlaylist(const QString& id);

    /** Removes the playlist with the given id. */
    Q_INVOKABLE bool removePlaylist(const QString& id);

    /** Last import error, for the UI. Empty when the last operation succeeded. */
    Q_INVOKABLE QString lastError() const;

    /** Opens a channel of a playlist in the current layout. */
    Q_INVOKABLE bool openChannel(const QString& playlistId, const QString& channelId);

signals:
    void playlistsChanged();
    void importingChanged();
    /** Emitted once an import completes; lastError() is set when it failed. */
    void importFinished(bool ok);

private:
    bool startImport(const QString& source, const QString& name, const nx::Uuid& replaceId);
    /** Parses, stores and reports a downloaded/read playlist. Returns false on a parse failure. */
    bool finishImport(const QByteArray& data);
    void onDownloadFinished();
    void onImportTimeout();
    void setImporting(bool value);
    /** Returns a copy: the settings property hands out its value by value. */
    std::optional<IptvPlaylist> findPlaylist(const nx::Uuid& id) const;
    bool storePlaylist(IptvPlaylist playlist);

    LocalSettings* const m_settings;
    QString m_lastError;

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_pendingReply;
    QTimer* m_timeout = nullptr;

    QString m_pendingSource;
    QString m_pendingName;
    nx::Uuid m_pendingReplaceId;
    bool m_importing = false;
};

} // namespace nx::vms::client::desktop
