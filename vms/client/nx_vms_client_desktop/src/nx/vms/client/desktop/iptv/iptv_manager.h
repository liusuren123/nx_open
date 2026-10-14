// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <optional>

#include <QtCore/QObject>
#include <QtCore/QVariantList>

#include <nx/utils/uuid.h>

#include "iptv_playlist.h"

namespace nx::vms::client::desktop {

class LocalSettings;

/**
 * Owns the IPTV playlists imported by the user and opens their channels in the current layout.
 *
 * A playlist is imported either from an http(s) URL or from a local .m3u file, then parsed and kept
 * in the local settings. Channels are not turned into resources up front: a resource is created only
 * when a channel is actually opened, so a playlist with thousands of entries stays cheap.
 */
class NX_VMS_CLIENT_DESKTOP_API IptvManager: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)

public:
    explicit IptvManager(LocalSettings* settings, QObject* parent = nullptr);
    virtual ~IptvManager() override;

    /** Imported playlists as a list of { id, name, source, channelCount } maps. */
    QVariantList playlists() const;

    /** Channels of a playlist as a list of { id, name, group, logo } maps. */
    Q_INVOKABLE QVariantList channels(const QString& playlistId) const;

    /**
     * Imports a playlist from an http(s) URL. The download is performed on the calling thread, so
     * this is meant for an explicit user action.
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

private:
    bool importSource(const QString& source, const QString& name, const nx::Uuid& replaceId);
    bool readSource(const QString& source, QByteArray* data) const;
    /** Returns a copy: the settings property hands out its value by value. */
    std::optional<IptvPlaylist> findPlaylist(const nx::Uuid& id) const;
    bool storePlaylist(IptvPlaylist playlist);

    LocalSettings* const m_settings;
    QString m_lastError;
};

} // namespace nx::vms::client::desktop
