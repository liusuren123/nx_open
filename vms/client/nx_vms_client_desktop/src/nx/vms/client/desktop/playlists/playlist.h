// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

#include <nx/reflect/instrument.h>
#include <nx/utils/json/qt_containers_reflect.h>
#include <nx/utils/uuid.h>

namespace nx::vms::client::desktop {

/**
 * A single entry of a local playlist. The entry is either a media file or a folder; folders are
 * scanned recursively when the playlist is played. Only the path is stored, so playlists survive
 * across runs and are independent of the resource pool.
 */
struct PlaylistEntry
{
    /** Absolute path with native separators. */
    QString path;

    /** Whether the path points to a folder to be expanded. */
    bool isFolder = false;

    bool operator==(const PlaylistEntry& other) const = default;
};
NX_REFLECTION_INSTRUMENT(PlaylistEntry, (path)(isFolder))

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
