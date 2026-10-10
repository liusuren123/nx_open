// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include "playlist.h"

namespace nx::vms::client::desktop {

class LocalSettings;

/**
 * Owns the persistent list of local playlists and exposes mutation operations on them.
 *
 * The data itself is stored in LocalSettings, so every change is persisted automatically. Playlist
 * identifiers are exposed to QML as strings, which is why the public API uses QString ids while the
 * stored Playlist::id remains an nx::Uuid.
 */
class NX_VMS_CLIENT_DESKTOP_API PlaylistManager: public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentPlaylistId
        READ currentPlaylistId
        WRITE setCurrentPlaylistId
        NOTIFY currentPlaylistChanged)
    Q_PROPERTY(QString currentPlaylistName
        READ currentPlaylistName
        NOTIFY currentPlaylistChanged)

public:
    explicit PlaylistManager(LocalSettings* settings, QObject* parent = nullptr);

    QList<Playlist> playlists() const;
    Playlist playlist(const QString& id) const;

    QString currentPlaylistId() const;
    void setCurrentPlaylistId(const QString& id);

    /** Name of the current playlist, or an empty string if there is none. */
    QString currentPlaylistName() const;

    /** Creates a new empty playlist and returns its id. */
    Q_INVOKABLE QString createPlaylist(const QString& name = QString());

    Q_INVOKABLE bool removePlaylist(const QString& id);
    Q_INVOKABLE bool renamePlaylist(const QString& id, const QString& name);

    /** Adds files and/or folders, detecting the kind of each path and filtering media files. */
    Q_INVOKABLE bool addPaths(const QString& id, const QStringList& paths);

    /** QML-friendly variant of addPaths() that accepts file/folder dialog URLs. */
    Q_INVOKABLE bool addFiles(const QString& id, const QList<QUrl>& urls);

    /**
     * Returns the id of the current playlist, creating a default one if no playlist exists yet.
     */
    Q_INVOKABLE QString ensureCurrentPlaylist(const QString& name = QString());

    /**
     * Adds files and/or folders to the current playlist (creating a default one if needed) and
     * requests playback of the first added entry. Returns the row of the first added entry, or -1
     * on failure.
     */
    Q_INVOKABLE int addPathsToCurrentPlaylist(const QStringList& paths);

    /** QML-friendly variant of addPathsToCurrentPlaylist() that accepts dialog URLs. */
    Q_INVOKABLE int addFilesToCurrentPlaylist(const QList<QUrl>& urls);

    /** Sorts the entries of the given playlist by file name (case-insensitive, stable). */
    Q_INVOKABLE bool sortEntriesByName(const QString& id);

    Q_INVOKABLE bool removeEntries(const QString& id, const QList<int>& rows);
    Q_INVOKABLE bool moveEntry(const QString& id, int fromRow, int toRow);
    Q_INVOKABLE bool clearEntries(const QString& id);

    /** Replaces the whole entry list of the given playlist. */
    bool setEntries(const QString& id, const QList<PlaylistEntry>& entries);

    /** Recursively expands all entries into an ordered list of playable media file paths. */
    static QStringList expandedFilePaths(const Playlist& playlist);

signals:
    void playlistsChanged();
    void playlistChanged(const QString& id);
    void currentPlaylistChanged();

    /**
     * Asks the local player to start playing the given entry of the given playlist. Emitted when
     * entries are added to the current playlist, so that "open file/folder" starts playback.
     */
    void playbackRequested(const QString& playlistId, int row);

private:
    void setPlaylists(const QList<Playlist>& playlists);

    LocalSettings* const m_settings;
};

} // namespace nx::vms::client::desktop
