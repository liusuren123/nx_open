// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QFileInfo>
#include <QtCore/QList>
#include <QtCore/QString>

#include <nx/reflect/instrument.h>
#include <nx/utils/json/qt_containers_reflect.h>
#include <nx/utils/uuid.h>

namespace nx::vms::client::desktop {

/**
 * A single entry of a local playlist. The entry is either a media file, a folder, or a network
 * stream (an IPTV channel and the like). Folders are scanned recursively when the playlist is
 * played. Only the path or the URL is stored, so playlists survive across runs and are independent
 * of the resource pool.
 */
struct PlaylistEntry
{
    /** Absolute path with native separators; empty for a network stream. */
    QString path;

    /** Whether the path points to a folder to be expanded. */
    bool isFolder = false;

    /** URL of a network stream; empty for a local file or folder. */
    QString url;

    /**
     * High quality tracks of the same stream, when the provider hands them out separately (a DASH
     * video and audio pair, for example). Only the local caching uses them: they are downloaded and
     * merged into one file. Playing the stream itself uses url, which already has both tracks.
     */
    QString videoUrl;
    QString audioUrl;

    /**
     * Identifies the media behind the URLs across their refreshes, so that a cached copy and a fresh
     * address still belong together. Empty when the provider gives no such identity.
     */
    QString group;

    /** Credentials of a stream; ignored for local media. */
    QString username;
    QString password;

    /**
     * Name the entry is shown under. Empty for local media added from a path, which is then named
     * after its file; set to the URL or the channel name for a stream, and replaced by the user when
     * an entry is renamed.
     */
    QString title;

    /** Whether the entry is a network stream rather than a local file or folder. */
    bool isStream() const { return !url.isEmpty(); }

    /** Name the entry is shown under: its title when it has one, the file name otherwise. */
    QString displayName() const { return title.isEmpty() ? QFileInfo(path).fileName() : title; }

    bool operator==(const PlaylistEntry& other) const = default;
};
NX_REFLECTION_INSTRUMENT(PlaylistEntry,
    (path)(isFolder)(url)(videoUrl)(audioUrl)(group)(username)(password)(title))

/** A named, ordered collection of local media entries. */
struct Playlist
{
    nx::Uuid id;
    QString name;
    QList<PlaylistEntry> entries;

    bool operator==(const Playlist& other) const = default;
};
NX_REFLECTION_INSTRUMENT(Playlist, (id)(name)(entries))

} // namespace nx::vms::client::desktop
